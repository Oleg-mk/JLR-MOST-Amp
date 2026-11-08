/* Selftest of the message layer and the amplifier emulation (console 'amp test') and of the phone's converter and
   rate control ('phone test'); no OS8104A needed, both run at boot in the QEMU variant. The asks follow the start-up
   seen in a JLR car (JLR-HU startUpFlow.json), addressed to the amplifier instead of the screen; the expected answers
   are those of amp.c's sources */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "amp.h"
#include "dsp.h"
#include "most.h"
#include "phone.h"
#include "src.h"
#include "u2m_golden.h"     /* usb2most's own bytes, read out of its firmware (fw/tools/host/golden.py) */

/* the test's buffers sit in PSRAM: internal RAM is short with Bluetooth and Wi-Fi on */
EXT_RAM_BSS_ATTR static most_msg_t s_out[32];
static int s_nout, s_fail, s_pass;

static bool capture(const most_msg_t *m)
{
    if (s_nout < (int)(sizeof s_out / sizeof s_out[0])) s_out[s_nout++] = *m;
    return true;
}

static void check(bool ok, const char *what)
{
    printf("  %s  %s\n", ok ? "pass" : "FAIL", what);
    if (ok) s_pass++; else s_fail++;
}

/* stage 4: what the emulation would drive, caught instead of the OS8104A's routes, REM and the ring's allocation */
static int s_route_dsp, s_route_n, s_routes, s_sound = -1, s_allocs, s_deallocs;
static uint8_t s_route_ch[8];
static bool s_alloc_ok = true;
static bool s_route_fail;               /* the chip refuses every route (I2C) */

static bool cap_route(int dsp, const uint8_t *ch, int n)
{
    s_routes++;
    s_route_dsp = dsp;
    s_route_n = ch ? n : 0;
    if (ch) memcpy(s_route_ch, ch, n > 8 ? 8 : n);
    return !s_route_fail;
}

static void cap_sound(bool on) { s_sound = on; }

static int cap_alloc(int n, uint8_t ch[8], uint8_t *delay)
{
    s_allocs++;
    if (!s_alloc_ok) return 0;
    for (int i = 0; i < n; i++) ch[i] = (uint8_t)(0x30 + i);
    *delay = 5;
    return n;
}

static bool s_dealloc_ok = true;        /* false: the master keeps the channels (busy throughout) */
static bool cap_dealloc(const uint8_t *ch, int n) { s_deallocs++; return s_dealloc_ok; }

static bool s_alloc_valid = true;       /* false: the ring's master started afresh, our channels are gone */
static bool cap_alloc_ok(const uint8_t *ch, int n) { return s_alloc_valid; }

static int cap_position(void) { return 3; }

static const amp_io_t s_cap_io = { cap_route, cap_sound, cap_alloc, cap_dealloc, cap_alloc_ok, cap_position };

/* the last route: the processing's sink and its channels (n 0: off) */
static bool routed(int dsp, const uint8_t *ch, int n)
{
    return s_route_dsp == dsp && s_route_n == n && (!n || !memcmp(s_route_ch, ch, n));
}

/* a telegram as the INT task would hand it over */
static void rx(uint8_t type, uint16_t src, uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, uint8_t tel,
               const uint8_t *d, int n)
{
    most_msg_t m = { .type = type, .src = src, .fblock = fb, .inst = inst, .fkt = fkt, .op = op, .tel_id = tel,
                     .len = (uint8_t)n };
    if (n) memcpy(m.data, d, n);
    s_nout = 0;
    amp_on_msg(&m);
}

/* a request to the amplifier 22.05 (Premium) from the gateway 0161, single-cast */
static void ask(uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x05, fkt, op, 0, d, n);
}

/* a message of any length to any FBlock, split into segments as the sender would */
static void rx_to(uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    EXT_RAM_BSS_ATTR static most_long_t l;
    EXT_RAM_BSS_ATTR static most_msg_t t[24];
    l = (most_long_t){ .type = MOST_RX_LOGICAL, .src = 0x161, .fblock = fb, .inst = inst, .fkt = fkt, .op = op,
                       .len = (uint8_t)n };
    if (n) memcpy(l.data, d, n);
    int k = most_split(&l, t, 24);
    s_nout = 0;
    for (int i = 0; i < k; i++) {
        t[i].src = 0x161;
        t[i].type = MOST_RX_LOGICAL;
        amp_on_msg(&t[i]);
    }
}

static void rx_long(uint16_t fkt, uint8_t op, const uint8_t *d, int n) { rx_to(0x22, 0x05, fkt, op, d, n); }

static bool sent(int i, uint16_t dst, uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    if (i >= s_nout) return false;
    const most_msg_t *m = &s_out[i];
    return m->dst == dst && m->fblock == fb && m->inst == inst && m->fkt == fkt && m->op == op && m->tel_id == 0 &&
           m->len == n && !memcmp(m->data, d, n);
}

/* the amplifier's only answer, a single telegram to 0161 */
static bool answered(uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    return s_nout == 1 && sent(0, 0x161, 0x22, 0x05, fkt, op, d, n);
}

/* the captured telegrams put together again */
static bool sent_long(most_long_t *out)
{
    EXT_RAM_BSS_ATTR static most_rx_seg_t s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < s_nout; i++) if (most_reassemble(&s, &s_out[i], out)) return true;
    return false;
}

static void buffers(void)
{
    /* the master's NetBlock Get as mRCMB holds it: physical, from 0161h, 01.04 000h Get, no data */
    uint8_t r[MOST_RCMB_LEN] = { 0x01, 0x01, 0x61, 0x01, 0x04, 0x00, 0x01, 0x00 };
    most_msg_t m;
    most_decode(r, &m);
    check(m.type == 1 && m.src == 0x161 && m.fblock == 1 && m.inst == 4 && m.fkt == 0 && m.op == 1 && m.len == 0,
          "mRCMB: type, source, 01.04 000 Get");
    /* 22.05 46Dh Status, 3 data bytes: FktID splits over two bytes with the OPType in the low nibble */
    uint8_t r2[MOST_RCMB_LEN] = { 0x00, 0x01, 0x86, 0x22, 0x05, 0x46, 0xDC, 0x03, 0xAA, 0xBB, 0xCC };
    most_decode(r2, &m);
    check(m.fkt == 0x46D && m.op == 0xC && m.tel_id == 0 && m.len == 3 && m.data[2] == 0xCC, "mRCMB: FktID 46D, OPType C");
    uint8_t x[MOST_XCMB_LEN];
    most_msg_t t = { .dst = 0x161, .fblock = 0x01, .inst = 0x04, .fkt = 0x000, .op = 0xC, .len = 2,
                     .data = { 0x22, 0x05 } };
    most_encode(&t, 0x01, x);
    const uint8_t want[13] = { 0x01, 0x00, 0x01, 0x61, 0x01, 0x04, 0x00, 0x0C, 0x02, 0x22, 0x05, 0x00, 0x00 };
    check(!memcmp(x, want, sizeof want), "mXCMB: priority, normal, target 0161, 01.04 000 Status [22 05]");
}

static void segments(void)
{
    EXT_RAM_BSS_ATTR static most_long_t l, back;
    EXT_RAM_BSS_ATTR static most_msg_t t[8];
    EXT_RAM_BSS_ATTR static most_rx_seg_t s;
    l = (most_long_t){ .dst = 0x161, .fblock = 0x22, .inst = 0x05, .fkt = 0x461, .op = 0xC, .len = 30 };
    for (int i = 0; i < 30; i++) l.data[i] = (uint8_t)i;
    int n = most_split(&l, t, 8);
    check(n == 3 && t[0].tel_id == 1 && t[1].tel_id == 2 && t[2].tel_id == 3 && t[0].data[0] == 0 &&
          t[2].data[0] == 2 && t[0].len == 12 && t[2].len == 9, "split 30 bytes: 3 segments, counters 0..2");
    memset(&s, 0, sizeof s);
    bool done = false;
    for (int i = 0; i < n; i++) { t[i].src = 0x161; done = most_reassemble(&s, &t[i], &back); }
    check(done && back.len == 30 && !memcmp(back.data, l.data, 30), "reassemble: the same 30 bytes");
    l.dst = MOST_BROADCAST;
    l.len = 11;
    check(most_split(&l, t, 8) == 1 && t[0].len == 11, "broadcast: 11 bytes still one telegram (16 data bytes)");
    /* MOST 2.5, 2.3.9: functions 000-001, 004-005, 022-023, A00-A01 -> (002 004 006 022 024 A00 A02 0), the bit
       changing at each */
    static const uint16_t ex[] = { 0x000, 0x001, 0x004, 0x005, 0x022, 0x023, 0xA00, 0xA01 };
    const uint8_t ex_want[] = { 0x00, 0x20, 0x04, 0x00, 0x60, 0x22, 0x02, 0x4A, 0x00, 0xA0, 0x20 };
    uint8_t b[16];
    check(most_fktids(ex, 8, b, sizeof b) == 11 && !memcmp(b, ex_want, 11), "FktIDs coding: the example of MOST 2.5");
    /* JLR's own lists are not all ascending, and one ends in zeros: every function must still be found */
    uint16_t f[64];
    int nf = most_fktids_decode(U2M_FKT_AUD3_DIAG, sizeof U2M_FKT_AUD3_DIAG, f, 64);
    bool d19 = false, d2e = false, d85 = false;
    for (int i = 0; i < nf; i++) d19 |= f[i] == 0xD19, d2e |= f[i] == 0xD2E, d85 |= f[i] == 0xD85;
    check(nf == 12 && d19 && d2e && d85, "FktIDs decode: AUD3's Diagnosis list, not ascending: 12 functions, D19 D2E D85");
    nf = most_fktids_decode(U2M_FKT_HI, sizeof U2M_FKT_HI, f, 64);
    check(nf == 16 && f[15] == 0xE08, "FktIDs decode: Hi-Line's list, zeros at its end: 16 functions, the last E08");
    nf = most_fktids_decode(U2M_FKT_DIAG_HI, sizeof U2M_FKT_DIAG_HI, f, 64);
    bool e10 = false;
    for (int i = 0; i < nf; i++) e10 |= f[i] == 0xE10;
    check(e10, "FktIDs decode: Hi-Line's Diagnosis list, D35 D36 twice: goes on to E10");
}

static void start_up(void)
{
    rx(MOST_RX_PHYSICAL, 0x161, 0x01, 0x04, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x000, MOST_OP_STATUS, U2M_FBLOCKS_PREM, 8),
          "Premium: NetBlock 000 Get -> Status [22.05 22.20 23.01 06.86] back to 0161, same instance");
    const uint8_t ok[] = { 0x01 };
    rx(MOST_RX_BROADCAST, 0x161, 0x02, 0x01, 0xA00, MOST_OP_STATUS, 0, ok, 1);
    const uint8_t sub[] = { 0x01, 0x01, 0x86, 0x03, 0x03 };
    check(s_nout == 1 && sent(0, 0x161, 0xF5, 0x01, 0x001, MOST_OP_SET, sub, 5),
          "network OK -> Notification Set to its sender F5.01: [01 0186 0303]");
    rx(MOST_RX_BROADCAST, 0x161, 0x02, 0x01, 0xA00, MOST_OP_STATUS, 0, ok, 1);
    check(s_nout == 0, "second network OK: no second subscription");
    const uint8_t nok[] = { 0x00 };
    rx(MOST_RX_BROADCAST, 0x161, 0x02, 0x01, 0xA00, MOST_OP_STATUS, 0, nok, 1);
    bool quiet = s_nout == 0;
    rx(MOST_RX_BROADCAST, 0x161, 0x02, 0x01, 0xA00, MOST_OP_STATUS, 0, ok, 1);
    check(quiet && s_nout == 1 && sent(0, 0x161, 0xF5, 0x01, 0x001, MOST_OP_SET, sub, 5),
          "network NotOK, then OK -> subscribed again: the gateway's matrix was deleted (MOST 2.5, p. 105)");

    static most_long_t l;
    ask(0x000, MOST_OP_GET, NULL, 0);
    check(s_nout == 3 && sent_long(&l) && l.op == MOST_OP_STATUS && l.len == sizeof U2M_FKT_PREM &&
          !memcmp(l.data, U2M_FKT_PREM, sizeof U2M_FKT_PREM),
          "FktIDs Get -> Status, usb2most's 32 bytes of Premium, 3 segments");
}

/* NetBlock, usb2most's answers (decode_prefacelift.md sect. 6) */
static void netblock(void)
{
    const uint8_t di[] = { 0xFF, 0xFF, 0x00 }, pos[] = { 0x40, 0x03 }, na[] = { 0x01, 0x86 }, ga[] = { 0x03, 0x22 };
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x04, 0x001, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x001, MOST_OP_STATUS, di, 3), "NetBlock DeviceInfo Get -> [FF FF 00]");
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x04, 0x002, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x002, MOST_OP_STATUS, pos, 2),
          "NetBlock NodePositionAddress Get -> [40, the position]");
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x04, 0x003, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x003, MOST_OP_STATUS, na, 2), "NetBlock NodeAddress Get -> [01 86]");
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x04, 0x004, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x004, MOST_OP_STATUS, ga, 2), "NetBlock GroupAddress Get -> [03 22]");
    const uint8_t query[] = { 0x00 }, exec[] = { 0x02 }, reg[] = { 0x01, 0x01, 0x61, 0x0C, 0x01 };
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x04, 0x006, MOST_OP_START, 0, query, 1);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x006, MOST_OP_RESULT, query, 1), "NetBlock ShutDown Query -> Result [00]");
    rx(MOST_RX_BROADCAST, 0x161, 0x01, 0xFF, 0x006, MOST_OP_START, 0, exec, 1);
    check(s_nout == 0, "NetBlock ShutDown Execute -> no answer (power.c's)");
    rx(MOST_RX_LOGICAL, 0x161, 0x01, 0x02, 0x401, MOST_OP_START, 0, reg, 5);
    check(s_nout == 0, "NetBlock 401 [01 0161 0C01], the head unit's registration -> no answer");
}

static void notification(void)
{
    const uint8_t note[] = { 0x01, 0x01, 0x61, 0x01, 0x13 }, m1[] = { 0x01, 0x00 }, m4[] = { 0x04, 0x00 };
    ask(0x001, MOST_OP_SET, note, 5);
    check(s_nout == 4 && sent(0, 0x161, 0x22, 0x05, 0x113, MOST_OP_STATUS, m1, 2) &&
          sent(3, 0x161, 0x22, 0x05, 0x113, MOST_OP_STATUS, m4, 2),
          "Notification Set for 113 from 0161 -> Mute of sinks 1..4 at once");
    const uint8_t q[] = { 0x01, 0x13 }, who[] = { 0x01, 0x13, 0x01, 0x61 };
    ask(0x001, MOST_OP_GET, q, 2);
    check(answered(0x001, MOST_OP_STATUS, who, 4), "Notification Get [0113] -> Status [0113 0161]");
    const uint8_t dev[] = { 0x01, 0x61 }, its[] = { 0x01, 0x61, 0x01, 0x13 };
    ask(0x002, MOST_OP_GET, dev, 2);
    check(answered(0x002, MOST_OP_STATUS, its, 4), "NotificationCheck Get [0161] -> Status [0161 0113] (GFB25 2.1.3)");
    const uint8_t no[] = { 0x01, 0x01, 0x61, 0x04, 0x00 };
    ask(0x001, MOST_OP_SET, no, 5);
    check(s_nout == 0, "Notification Set for 400 (not here) -> no answer, as usb2most");
}

static void mute(void)
{
    const uint8_t on1[] = { 0x01, 0x01 }, all0[] = { 0x00, 0x00 }, g1[] = { 0x01 }, st1[] = { 0x01, 0x00 };
    ask(0x113, MOST_OP_SETGET, on1, 2);
    check(s_nout == 2 && sent(0, 0x161, 0x22, 0x05, 0x113, MOST_OP_STATUS, on1, 2) &&
          sent(1, 0x161, 0x22, 0x05, 0x113, MOST_OP_STATUS, on1, 2),
          "Mute SetGet [01 01] -> Status to the sender and the notification to 0161");
    ask(0x113, MOST_OP_SET, on1, 2);
    check(s_nout == 0, "the same Mute again: no change, no notification");
    ask(0x113, MOST_OP_SET, all0, 2);
    check(answered(0x113, MOST_OP_STATUS, st1, 2),
          "Mute Set [00 00], all sinks -> a notification per sink that changed: [01 00] (GFB25 2.1.29)");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x00, 0x113, MOST_OP_GET, 0, g1, 1);
    check(answered(0x113, MOST_OP_STATUS, st1, 2), "Mute Get [01] to any instance -> Status [01 00] from 22.05");
    const uint8_t s7[] = { 0x07, 0x01 }, e6[] = { MOST_ERR_PARAM, 0x01, 0x07 }, e4[] = { MOST_ERR_OPTYPE, MOST_OP_INC };
    ask(0x113, MOST_OP_SETGET, s7, 2);
    check(answered(0x113, MOST_OP_ERROR, e6, 3), "Mute sink 7 -> Error [06 01 07]: parameter 1, its value");
    ask(0x113, MOST_OP_INC, g1, 1);
    check(answered(0x113, MOST_OP_ERROR, e4, 2), "Mute Increment -> Error [04 03]: the OPType");

    const uint8_t mp[] = { 0x02, 0x00, 0x64, 0x01, 0xF4, 0x01, 0x02 }, g2[] = { 0x02 }, g3[] = { 0x03 };
    ask(0x466, MOST_OP_SET, mp, 7);
    bool quiet = s_nout == 0;
    ask(0x466, MOST_OP_GET, g2, 1);
    check(quiet && answered(0x466, MOST_OP_STATUS, mp, 7),
          "MuteParameters Set [02, 100 ms, 500 ms, linear, degressive], Get [02] -> the same");
    const uint8_t e7[] = { MOST_ERR_NOTAVAIL, 0x01, 0x03 };
    ask(0x466, MOST_OP_GET, g3, 1);
    check(answered(0x466, MOST_OP_ERROR, e7, 3), "MuteParameters Get [03], never set -> Error [07 01 03]");
    const uint8_t row5[] = { 0x05, 0x00, 0x64, 0x01, 0xF4, 0x01, 0x02 };
    ask(0x466, MOST_OP_SET, row5, 7);
    check(s_nout == 0, "MuteParameters of row 5 -> no answer, not stored, as usb2most");
    ask(0x466, MOST_OP_SET, mp, 5);
    check(s_nout == 0, "MuteParameters with 5 bytes -> no answer, not stored, as usb2most");
}

/* a sink of the sound processing on a constant, the others silent: its left and right gains once the 10-ms level glide
   and the balance's glide are over */
static void lr_gains(int sink, double *gl, double *gr)
{
    EXT_RAM_BSS_ATTR static int32_t in[2 * 192], one[192], out[2 * 192];
    const int32_t *mono[3] = { NULL, NULL, NULL };
    for (int i = 0; i < 192; i++) {
        one[i] = 0x40000000;
        in[2 * i] = in[2 * i + 1] = sink == 1 ? 0x40000000 : 0;
    }
    if (sink > 1) mono[sink - 2] = one;
    for (int b = 0; b < 12; b++) dsp_process(in, mono, out, 192);
    *gl = (double)out[2 * 191] / 0x40000000;
    *gr = (double)out[2 * 191 + 1] / 0x40000000;
}

static void levels(void)
{
    /* JLR: [row, 0 = all speakers, 5 levels in dB: fl, c, fr, rl, rr], usb2most 80330E8, 8034140; Get answers [00 00, the
       row], the row's number not echoed, 8033DE0 */
    static most_long_t l;
    const uint8_t lv[] = { 0x01, 0x00, 0xE8, 0xE8, 0xEC, 0xEC, 0xF0 }, g1[] = { 0x01, 0x00 };
    const uint8_t st1[] = { 0x00, 0x00, 0xE8, 0xE8, 0xEC, 0xEC, 0xF0 };
    ask(0x46D, MOST_OP_SET, lv, 7);
    bool quiet = s_nout == 0;
    ask(0x46D, MOST_OP_GET, g1, 2);
    check(quiet && answered(0x46D, MOST_OP_STATUS, st1, 7), "SpeakerLevel Set [01 00, 5 levels] -> no answer, Get [01 00] -> [00 00, the row]");
    const uint8_t g3[] = { 0x03, 0x00 }, st3[] = { 0x00, 0x00, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D };
    ask(0x46D, MOST_OP_GET, g3, 2);
    check(answered(0x46D, MOST_OP_STATUS, st3, 7), "SpeakerLevel Get of row 3, never set -> [00 00, 5 x -99 dB]");
    const uint8_t gs[] = { 0x01, 0x01 }, g5[] = { 0x05, 0x00 }, e6[] = { MOST_ERR_PARAM };
    ask(0x46D, MOST_OP_GET, gs, 2);
    bool empty = answered(0x46D, MOST_OP_STATUS, NULL, 0);
    ask(0x46D, MOST_OP_GET, g5, 2);
    check(empty && answered(0x46D, MOST_OP_STATUS, NULL, 0), "SpeakerLevel Get [01 01], [05 00] -> an empty Status");
    ask(0x46D, MOST_OP_GET, g1, 1);
    check(answered(0x46D, MOST_OP_ERROR, e6, 1), "SpeakerLevel Get of 1 byte -> Error [06]");
    const uint8_t g0[] = { 0x00, 0x00 };
    uint8_t all[2 + 20];
    memset(all, 0x9D, sizeof all);
    all[0] = all[1] = 0;
    memcpy(all + 2, lv + 2, 5);
    ask(0x46D, MOST_OP_GET, g0, 2);
    check(sent_long(&l) && l.len == sizeof all && !memcmp(l.data, all, sizeof all), "SpeakerLevel Get [00 00] -> all four rows");
    const uint8_t one[] = { 0x01, 0x02, 0xF6 };
    const uint8_t lv2[] = { 0x00, 0x00, 0xE8, 0xF6, 0xEC, 0xEC, 0xF0 };
    ask(0x46D, MOST_OP_SETGET, one, 3);
    bool echo = answered(0x46D, MOST_OP_STATUS, one, 3);
    ask(0x46D, MOST_OP_GET, g1, 2);
    check(echo && answered(0x46D, MOST_OP_STATUS, lv2, 7), "SpeakerLevel SetGet [01 02 F6] (the catalogue's run) -> Status, speaker 2 changed");
    /* a head unit's centre can be above 0 dB at full volume (usb2most 8033604 does not clamp it): the row must not be
       refused for that, and the music must follow */
    const uint8_t up[] = { 0x01, 0x00, 0xF0, 0x04, 0xF0, 0xF0, 0xF0 };
    ask(0x46D, MOST_OP_SET, up, 7);
    check(s_nout == 0, "SpeakerLevel with the centre at +4 dB -> taken, no error");

    const uint8_t z[] = { 0x00 }, m3[] = { 0xFD }, five[] = { 0x05 }, p2[] = { 0x02 }, p6[] = { 0x06 }, n1[] = { 0x01 };
    ask(0x202, MOST_OP_GET, NULL, 0);
    check(answered(0x202, MOST_OP_STATUS, z, 1), "Bass Get -> Status [00]");
    ask(0x202, MOST_OP_SETGET, m3, 1);
    check(answered(0x202, MOST_OP_STATUS, m3, 1), "Bass SetGet [-3] -> Status [-3]");
    ask(0x202, MOST_OP_INC, five, 1);
    check(answered(0x202, MOST_OP_STATUS, p2, 1), "Bass Increment 5 -> Status [+2]");
    ask(0x202, MOST_OP_INC, five, 1);
    check(answered(0x202, MOST_OP_STATUS, p6, 1), "Bass Increment 5 -> Status [+6], the end of the range");
    ask(0x202, MOST_OP_INC, n1, 1);
    check(answered(0x202, MOST_OP_STATUS, p6, 1), "Bass Increment at the end -> Status [+6] again");
    const uint8_t e6z[] = { MOST_ERR_PARAM, 0x01, 0x00 };
    ask(0x202, MOST_OP_DEC, z, 1);
    check(answered(0x202, MOST_OP_ERROR, e6z, 3), "Bass Decrement 0 steps -> Error [06 01 00]");
}

/* the pre-facelift's levels per speaker as the two outputs' levels; on an emulation of its own, the state stays out of
   the other checks */
static void sides(void)
{
    /* the pre-facelift's balance: the left output follows the left speakers, the right the right ones, in usb2most's
       order fl, c, fr, rl, rr; the centre alone sets both */
    dsp_init(48000);
    const uint8_t bal[] = { 0x01, 0x00, 0xFA, 0x81, 0xEE, 0xFA, 0xEE };
    ask(0x46D, MOST_OP_SET, bal, 7);
    double gl, gr;
    lr_gains(1, &gl, &gr);
    check(fabs(gl - 0.5012) < 0.002 && fabs(gr - 0.1259) < 0.002,
          "SpeakerLevel fl -6, c off, fr -18, rl -6, rr -18 -> the left output -6 dB, the right -18 dB");
    const uint8_t cen[] = { 0x01, 0x00, 0x81, 0xF6, 0x81, 0x81, 0x81 };
    ask(0x46D, MOST_OP_SET, cen, 7);
    lr_gains(1, &gl, &gr);
    check(fabs(gl - 0.3162) < 0.002 && fabs(gr - 0.3162) < 0.002,
          "SpeakerLevel the centre alone at -10 -> both outputs -10 dB");
    /* a side at -127 dB is silence, whatever the other side has: going down from -126 never makes it louder */
    const uint8_t l126[] = { 0x01, 0x00, 0x82, 0x81, 0x00, 0x82, 0x00 };
    ask(0x46D, MOST_OP_SET, l126, 7);
    lr_gains(1, &gl, &gr);
    check(gl > 0 && gl < 1e-5 && gr == 1, "SpeakerLevel the left speakers -126, the right 0 -> the left output next to nothing");
    const uint8_t l127[] = { 0x01, 0x00, 0x81, 0x81, 0x00, 0x81, 0x00 };
    ask(0x46D, MOST_OP_SET, l127, 7);
    lr_gains(1, &gl, &gr);
    check(gl == 0 && gr == 1, "SpeakerLevel the left speakers -127, the right 0 -> the left output silent, the right full");
    const uint8_t r127[] = { 0x01, 0x00, 0x00, 0x81, 0x81, 0x00, 0x81 };
    ask(0x46D, MOST_OP_SET, r127, 7);
    lr_gains(1, &gl, &gr);
    check(gl == 1 && gr == 0, "SpeakerLevel the right speakers -127, the left 0 -> the right output silent, the left full");
    const uint8_t off[] = { 0x01, 0x00, 0x81, 0x81, 0x81, 0x81, 0x81 };
    ask(0x46D, MOST_OP_SET, off, 7);
    lr_gains(1, &gl, &gr);
    check(gl == 0 && gr == 0, "SpeakerLevel all speakers -127 -> both outputs silent");
    /* one level, the side the head unit never set follows it: a mono sink, a speaker set alone */
    const uint8_t mono1[] = { 0x03, 0x00, 0xF2 };
    ask(0x46D, MOST_OP_SET, mono1, 3);
    lr_gains(3, &gl, &gr);
    check(fabs(gl - 0.1995) < 0.002 && gr == gl, "SpeakerLevel [03 00 F2], one level on row 3 -> both outputs -14 dB");
    const uint8_t fr4[] = { 0x04, 0x03, 0xF6 };
    ask(0x46D, MOST_OP_SET, fr4, 3);
    lr_gains(4, &gl, &gr);
    check(fabs(gl - 0.3162) < 0.002 && gr == gl, "SpeakerLevel [04 03 F6], speaker 3 of row 4 alone -> both outputs -10 dB");
    /* The bytes usb2most sends a real amplifier: 80330E8 the volume, level 5 of its table; 8033088 the overlays. A row is
       its own: what comes for the phone or the parking sensors must not touch the music. This is what amp.c once had the
       wrong way round, reading byte 0 as the speaker: row 2 landed in every sink from speaker 2 on, music included */
    const uint8_t vol[] = { 0x01, 0x00, 0xC2, 0x9D, 0xC2, 0xBC, 0xBC }, ph[] = { 0x02, 0x00, 0xE4, 0x9D, 0xE4, 0x9D, 0x9D };
    const uint8_t pdc[] = { 0x04, 0x00, 0x9D, 0x9D, 0x9D, 0xE2, 0xE2 };
    ask(0x46D, MOST_OP_SET, vol, 7);
    bool q = s_nout == 0;
    lr_gains(1, &gl, &gr);
    bool music = fabs(gl - 7.94e-4) < 2e-5 && gr == gl;
    check(q && music, "usb2most's volume [01 00 C2 9D C2 BC BC] -> the music at -62 dB both sides, no answer");
    ask(0x46D, MOST_OP_SET, ph, 7);
    lr_gains(1, &gl, &gr);
    bool same = fabs(gl - 7.94e-4) < 2e-5 && gr == gl;
    lr_gains(2, &gl, &gr);
    check(same && fabs(gl - 0.0398) < 0.001 && gr == gl, "row 2 [02 00 E4 9D E4 9D 9D] -> sink 2 at -28 dB, the music stays at -62 dB");
    ask(0x46D, MOST_OP_SET, pdc, 7);
    lr_gains(1, &gl, &gr);
    same = fabs(gl - 7.94e-4) < 2e-5 && gr == gl;
    lr_gains(4, &gl, &gr);
    check(same && fabs(gl - 0.0316) < 0.001 && gr == gl, "row 4 [04 00 9D 9D 9D E2 E2] -> sink 4 at -30 dB, the music stays");
    /* all rows at once, the start of a head unit's start-up (8033778): the music silent, nothing else moved in the table */
    uint8_t whole[2 + 20];
    memset(whole, 0x9D, sizeof whole);
    whole[0] = whole[1] = 0;
    whole[2 + 5] = whole[2 + 7] = 0xEC;     /* and row 2 with something new in it: it must land in row 2 */
    rx_to(0x22, 0x05, 0x46D, MOST_OP_SET, whole, sizeof whole);
    lr_gains(2, &gl, &gr);
    check(s_nout == 0 && fabs(gl - 0.1) < 0.002, "[00 00, 4 x 5 levels] -> row by row: row 2 at -20 dB from its place in the array");
    /* above 0 dB is 0 dB at the output */
    const uint8_t over[] = { 0x01, 0x00, 0x03, 0x9D, 0x03, 0x03, 0x03 };
    ask(0x46D, MOST_OP_SET, over, 7);
    lr_gains(1, &gl, &gr);
    check(gl == 1 && gr == 1, "levels above 0 dB -> the output at 0 dB, not above");
    dsp_init(48000);
}

/* the arrays of JLR's own E0xh, as usb2most keeps them */
static void e0x(void)
{
    static most_long_t l;
    const uint8_t pos[] = { 0x00, 0x00 }, pos1[] = { 0x01, 0x00 }, e6[] = { MOST_ERR_PARAM };
    ask(0xE05, MOST_OP_GET, pos, 2);
    check(answered(0xE05, MOST_OP_STATUS, U2M_E05_INIT, 7), "E05 Get [00 00] before any Set -> usb2most's starting value");
    ask(0xE05, MOST_OP_GET, pos1, 2);
    bool empty = answered(0xE05, MOST_OP_STATUS, NULL, 0);
    ask(0xE05, MOST_OP_GET, pos, 1);
    check(empty && answered(0xE05, MOST_OP_ERROR, e6, 1), "E05 Get of another Pos -> an empty Status; of another length -> Error [06]");
    ask(0xE04, MOST_OP_GET, pos, 2);
    bool e04 = answered(0xE04, MOST_OP_STATUS, U2M_E04_INIT, 4);
    ask(0xE07, MOST_OP_GET, pos, 2);
    check(e04 && answered(0xE07, MOST_OP_STATUS, U2M_E07_INIT, 6), "E04, E07 Get -> usb2most's starting values");
    uint8_t e08[14];
    memcpy(e08, U2M_E08_INIT, 14);
    e08[0] = e08[1] = 0;                    /* usb2most's Get zeroes the first two: its table holds no Pos */
    ask(0xE08, MOST_OP_GET, pos, 2);
    check(sent_long(&l) && l.len == 14 && !memcmp(l.data, e08, 14), "E08 Get -> usb2most's starting value, its first two bytes zeroed");
    const uint8_t set5[] = { 0x00, 0x00, 0x03, 0x01, 0x14, 0x03, 0x00 }, part[] = { 0x02, 0x00, 0x01 };
    ask(0xE05, MOST_OP_SET, set5, 7);
    bool q = s_nout == 0;
    ask(0xE05, MOST_OP_SET, part, 3);
    q &= s_nout == 0;
    ask(0xE05, MOST_OP_GET, pos, 2);
    check(q && answered(0xE05, MOST_OP_STATUS, set5, 7), "E05 Set of the whole -> kept; Set [02 00 mode] -> passed over; no answers");
    const uint8_t half[] = { 0x02, 0x00, 1, 2, 3, 4, 5, 6, 7 }, bad[] = { 0x03, 0x00, 1, 2, 3, 4, 5, 6, 7 };
    ask(0xE08, MOST_OP_SET, half, 9);
    q = s_nout == 0;
    ask(0xE08, MOST_OP_SET, bad, 9);
    check(q && answered(0xE08, MOST_OP_ERROR, e6, 1), "E08 Set of its second half -> no answer; of a third -> Error [06]");
}

/* The start-up a pre-facelift head unit gives the amplifier, as usb2most's driver sends it to a real one (method +40,
   decode_prefacelift.md sect. 8.3; its tables' bytes out of its firmware, u2m_golden.h). All of it Set, and the driver's
   own amplifier emulation answers none of it: nor must this */
static int s_loud;

static void say_nothing(uint8_t fb, uint8_t inst, uint16_t fkt, const uint8_t *d, int n)
{
    rx_to(fb, inst, fkt, MOST_OP_SET, d, n);
    if (s_nout) {
        printf("    answered: %02X.%02X %03X op %X\n", fb, inst, fkt, s_out[0].op);
        s_loud++;
    }
}

static void startup_common(uint8_t inst)
{
    uint8_t lv[2 + 20];
    const uint8_t lim[] = { 0x01, 0x00, 0x00, 0x20, 0x03, 0xE8 }, z[] = { 0x00 },
                  e08[] = { 0x00, 0x00, 0x00, 0x50, 0x00, 0x1E, 0x09, 0x01, 0x01, 0x07, 0xD0, 0x13, 0x88, 0x06, 0x01, 0x01 },
                  e00[] = { 0x00, 0x00, 0xC1, 0xC1, 0xC1, 0xC1 }, nb[] = { 0x01, 0x01, 0x61, 0x0C, 0x01 },
                  dg[] = { 0x01, 0x01, 0x61, 0x0E, 0x10 }, xo1[] = { 0x01, 0x02, 0x00, 0x5A }, sl1[] = { 0x01, 0x02, 0x04 },
                  xo2[] = { 0x02, 0x01, 0x00, 0x70 }, sl2[] = { 0x02, 0x01, 0x02 };
    s_loud = 0;
    say_nothing(0x22, 0x20, 0x466, U2M_MUTE_ROWS, 7);
    for (int i = 1; i < 4; i++) say_nothing(0x22, inst, 0x466, U2M_MUTE_ROWS + 7 * i, 7);
    memset(lv, 0x9D, sizeof lv);
    lv[0] = lv[1] = 0;
    say_nothing(0x22, inst, 0x46D, lv, sizeof lv);
    say_nothing(0x22, inst, 0x427, lim, sizeof lim);
    say_nothing(0x22, inst, 0x202, z, 1);
    say_nothing(0x22, inst, 0x203, z, 1);
    say_nothing(0x22, inst, 0xE08, e08, sizeof e08);
    say_nothing(0x23, 0x01, 0x466, U2M_MUTE_ROWS, 7);
    say_nothing(0x23, 0x01, 0xE00, e00, sizeof e00);
    say_nothing(0x01, 0x02, 0x401, nb, sizeof nb);
    say_nothing(0x06, 0x86, 0x001, dg, sizeof dg);
    say_nothing(0x22, inst, 0x430, xo1, sizeof xo1);
    say_nothing(0x22, inst, 0x431, sl1, sizeof sl1);
    say_nothing(0x22, inst, 0x430, xo2, sizeof xo2);
    say_nothing(0x22, inst, 0x431, sl2, sizeof sl2);
}

static void startup_premium(void)
{
    static most_long_t l;
    uint8_t b[2 + 64];
    const uint8_t xo3[] = { 0x03, 0x02, U2M_XO_PREM[0x0B], U2M_XO_PREM[0x0A] }, sl3[] = { 0x03, 0x02, U2M_SL_PREM[5] },
                  xo4[] = { 0x04, 0x01, U2M_XO_PREM[0x0D], U2M_XO_PREM[0x0C] }, sl4[] = { 0x04, 0x01, U2M_SL_PREM[6] },
                  e04[] = { 0x00, 0x00, 0x00, 0x00 }, e05p[] = { 0x02, 0x00, 0x00 }, e04p[] = { 0x01, 0x00, 0x00 },
                  pos[] = { 0x00, 0x00 };
    startup_common(0x05);
    say_nothing(0x22, 0x05, 0x430, xo3, sizeof xo3);
    say_nothing(0x22, 0x05, 0x431, sl3, sizeof sl3);
    say_nothing(0x22, 0x05, 0x430, xo4, sizeof xo4);
    say_nothing(0x22, 0x05, 0x431, sl4, sizeof sl4);
    say_nothing(0x22, 0x05, 0x441, U2M_DLY_INIT, sizeof U2M_DLY_INIT);
    b[1] = 0;
    b[0] = 0xF0;
    memcpy(b + 2, U2M_EQ_PREM, 60);
    say_nothing(0x22, 0x05, 0x461, b, 62);
    b[0] = 0xF1;
    memcpy(b + 2, U2M_EQ_PREM + 60, 60);
    say_nothing(0x22, 0x05, 0x461, b, 62);
    b[0] = 0xF3;
    memcpy(b + 2, U2M_EQ_PREM + 120, 60);
    memset(b + 62, 0, 4);
    say_nothing(0x22, 0x05, 0x461, b, 66);
    say_nothing(0x22, 0x05, 0xE05, U2M_E05_INIT, sizeof U2M_E05_INIT);
    say_nothing(0x22, 0x05, 0xE04, e04, sizeof e04);
    say_nothing(0x22, 0x05, 0xE07, U2M_E07_INIT, sizeof U2M_E07_INIT);
    say_nothing(0x22, 0x05, 0xE05, e05p, sizeof e05p);
    say_nothing(0x22, 0x05, 0xE04, e04p, sizeof e04p);
    say_nothing(0x22, 0x05, 0x441, U2M_DLY_INIT, sizeof U2M_DLY_INIT);
    check(!s_loud, "Premium: a head unit's whole start-up, 32 messages, usb2most's bytes -> none answered");
    uint8_t lv[2 + 20];
    memset(lv, 0x9D, sizeof lv);
    lv[0] = lv[1] = 0;
    ask(0x46D, MOST_OP_GET, pos, 2);
    check(sent_long(&l) && l.len == sizeof lv && !memcmp(l.data, lv, sizeof lv), "  ... SpeakerLevel Get [00 00] -> all rows at -99 dB, as it set them");
    const uint8_t e08[] = { 0x00, 0x00, 0x00, 0x1E, 0x09, 0x01, 0x01, 0x07, 0xD0, 0x13, 0x88, 0x06, 0x01, 0x01 };
    ask(0xE08, MOST_OP_GET, pos, 2);
    check(sent_long(&l) && l.len == sizeof e08 && !memcmp(l.data, e08, sizeof e08), "  ... E08 Get -> what it set, the first two bytes zeroed, as usb2most");
    ask(0xE05, MOST_OP_GET, pos, 2);
    check(answered(0xE05, MOST_OP_STATUS, U2M_E05_INIT, 7), "  ... E05 Get -> what it set");
}

/* Hi-Line: 22.02 and its lists, byte for byte; the start-up's single EQ rows; the serial number asked last */
static void hiline(void)
{
    static most_long_t l;
    const uint8_t dly[] = { 0x00, 0x00, 0x07, 0xD0, 0x00, 0x00, 0x07, 0xD0, 0x15, 0xE0, 0x15, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    rx(MOST_RX_PHYSICAL, 0x161, 0x01, 0x04, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x000, MOST_OP_STATUS, U2M_FBLOCKS_HI, 8),
          "Hi-Line: NetBlock 000 Get -> [22.02 22.20 23.01 06.86]");
    rx_to(0x22, 0x02, 0x000, MOST_OP_GET, NULL, 0);
    check(sent_long(&l) && l.fkt == 0 && l.len == sizeof U2M_FKT_HI && !memcmp(l.data, U2M_FKT_HI, l.len), "Hi-Line: 22.02 FktIDs, usb2most's 33 bytes");
    rx_to(0x06, 0x86, 0x000, MOST_OP_GET, NULL, 0);
    check(sent_long(&l) && l.len == sizeof U2M_FKT_DIAG_HI && !memcmp(l.data, U2M_FKT_DIAG_HI, l.len), "Hi-Line: 06.86 FktIDs, usb2most's 38 bytes, not ascending");
    rx_to(0x22, 0x05, 0x000, MOST_OP_GET, NULL, 0);
    check(s_nout == 0, "Hi-Line: 22.05 (Premium's) -> no answer");
    startup_common(0x02);
    say_nothing(0x22, 0x02, 0x441, dly, sizeof dly);
    static const uint8_t rows[] = { 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x18,
                                    0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x2C, 0x2D };
    for (int i = 0; i < (int)sizeof rows; i++) {
        uint8_t b[6] = { rows[i], 0x00 };
        memcpy(b + 2, U2M_EQ_HI + 4 * (rows[i] - 1), 4);
        say_nothing(0x22, 0x02, 0x461, b, 6);
    }
    check(!s_loud, "Hi-Line: a head unit's whole start-up, 40 messages, usb2most's bytes -> none answered");
    const uint8_t sn[] = { 0xF1, 0x8C };
    rx_to(0x06, 0x86, 0xD22, MOST_OP_SETGET, sn, 2);
    check(sent_long(&l) && l.fkt == 0xD22 && l.op == MOST_OP_STATUS && l.len == 18, "Hi-Line: ... and the serial number it asks last -> Status, 18 bytes");
    const uint8_t on[] = { 0x01, 0x00, 0xE8, 0x9D, 0xE8, 0xE8, 0xE8 };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x02, 0x46D, MOST_OP_SET, 0, on, 7);
    check(s_nout == 0, "Hi-Line: the volume on 22.02 -> no answer");
}

static void sinks_and_errors(void)
{
    const uint8_t con[] = { 0x01, 0x00, 0x00, 0x01, 0x02, 0x03 }, one[] = { 0x01 };
    s_sound = -1;
    ask(0x111, MOST_OP_STARTRESULT, con, 6);
    check(answered(0x111, MOST_OP_RESULT, one, 1), "Connect StartResult [1, delay 0, channels 0..3] -> Result [01]");
    check(routed(1, con + 2, 4) && s_sound == 1, "  ... channels 00..03 to the stereo sink, REM and the DAC on");
    const uint8_t con2[] = { 0x02, 0x00, 0x10, 0x11 }, two[] = { 0x02 };
    ask(0x111, MOST_OP_STARTRESULT, con2, 4);
    check(answered(0x111, MOST_OP_RESULT, two, 1) && routed(2, con2 + 2, 2), "Connect [2, channels 10 11] -> mono 0");
    ask(0x112, MOST_OP_STARTRESULT, two, 1);
    bool off2 = routed(2, NULL, 0) && s_sound == 1;
    ask(0x112, MOST_OP_STARTRESULT, one, 1);
    check(answered(0x112, MOST_OP_RESULT, one, 1), "DisConnect StartResult [1] -> Result [01]");
    check(off2 && routed(1, NULL, 0) && s_sound == 0, "  ... both sinks off; REM follows only when none is left");
    /* the chip refuses the route: Error 41h instead of a Result, the sink off and REM with it; the next Connect
       routes it afresh */
    const uint8_t e41[] = { MOST_ERR_UNAVAIL };
    s_route_fail = true;
    ask(0x111, MOST_OP_STARTRESULT, con, 6);
    s_route_fail = false;
    check(answered(0x111, MOST_OP_ERROR, e41, 1) && routed(1, NULL, 0) && s_sound == 0,
          "Connect, the chip refused the route -> Error 41, the sink off, REM off");
    ask(0x111, MOST_OP_STARTRESULT, con, 6);
    check(answered(0x111, MOST_OP_RESULT, one, 1) && routed(1, con + 2, 4) && s_sound == 1,
          "  ... the next Connect routes it afresh");
    ask(0x112, MOST_OP_STARTRESULT, one, 1);
    const uint8_t bad[] = { 0x07 }, con7[] = { 0x07, 0x00, 0x20, 0x21 };
    int before = s_routes;
    ask(0x112, MOST_OP_STARTRESULT, bad, 1);
    bool r7 = answered(0x112, MOST_OP_RESULT, bad, 1);
    ask(0x111, MOST_OP_STARTRESULT, con7, 4);
    check(r7 && answered(0x111, MOST_OP_RESULT, bad, 1) && s_routes == before,
          "DisConnect and Connect of sink 7, not here -> Result [07], nothing routed, as usb2most");
    /* usb2most gives a pre-facelift amplifier [c+1 c c+5 c+4] for video (8037A2C): a list need not ascend */
    const uint8_t unord[] = { 0x01, 0x00, 0x09, 0x08, 0x0D, 0x0C }, far[] = { 0x01, 0x00, 0x3C, 0x3D, 0x3E, 0x3F },
                  e6c[] = { MOST_ERR_PARAM, 0x03, 0x3C };
    ask(0x111, MOST_OP_STARTRESULT, unord, 6);
    check(answered(0x111, MOST_OP_RESULT, one, 1) && routed(1, unord + 2, 4), "Connect [1, 0 , 09 08 0D 0C] -> Result, routed in that order");
    ask(0x112, MOST_OP_STARTRESULT, one, 1);
    ask(0x111, MOST_OP_STARTRESULT, far, 6);
    check(answered(0x111, MOST_OP_ERROR, e6c, 3), "Connect, channel 60 and up -> Error [06 03 3C]");

    const uint8_t e3[] = { MOST_ERR_FKT }, e1[] = { MOST_ERR_FBLOCK };
    ask(0x400, MOST_OP_GET, NULL, 0);
    check(answered(0x400, MOST_OP_ERROR, e3, 1), "Volume 400 Get (the XF amplifier has none) -> Error 03");
    rx(MOST_RX_GROUPCAST, 0x161, 0x22, 0x05, 0x400, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 0, "the same to the group: silence");
    rx(MOST_RX_LOGICAL, 0x161, 0x31, 0x01, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x31, 0x01, 0x000, MOST_OP_ERROR, e1, 1), "FBlock 31 to us -> Error 01");
    rx(MOST_RX_BROADCAST, 0x161, 0x31, 0x01, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 0, "the same as a broadcast: silence");
    const uint8_t st[] = { 0x00, 0x44 };
    rx(MOST_RX_LOGICAL, 0x161, 0xF5, 0x01, 0x303, MOST_OP_STATUS, 0, st, 2);
    check(s_nout == 0, "Status from the gateway (the subscription): no answer");
}

static void arrays(void)
{
    /* SpeakerDelay, Pos {0} = all outputs, 9 delays in us: the start-up groups front 2 ms, rear 5.6 ms, centre 0.5 ms,
       pillars 6.4 ms (stock_amps.md 2) - 20 bytes, two segments each way */
    const uint8_t dl[] = { 0x00, 0x00, 0x07, 0xD0, 0x07, 0xD0, 0x15, 0xE0, 0x15, 0xE0, 0x01, 0xF4,
                           0x19, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 }, pos[] = { 0x00, 0x00 };
    rx_long(0x441, MOST_OP_SET, dl, sizeof dl);
    bool quiet = s_nout == 0;
    static most_long_t l;
    ask(0x441, MOST_OP_GET, pos, 2);
    check(quiet && s_nout == 2 && s_out[0].tel_id == MOST_TEL_FIRST && s_out[1].tel_id == MOST_TEL_LAST &&
          sent_long(&l) && l.op == MOST_OP_STATUS && l.len == sizeof dl && !memcmp(l.data, dl, sizeof dl),
          "SpeakerDelay Set in 2 segments, Get [00 00] -> Status in 2 segments, the same 20 bytes");

    /* a node registering for everything gets every value there is at once, and no error for the others */
    const uint8_t all[] = { 0x00, 0x01, 0x6E }, b6[] = { 0x06 };
    rx(MOST_RX_LOGICAL, 0x16E, 0x22, 0x05, 0x001, MOST_OP_SET, 0, all, 3);
    bool none_err = s_nout > 4, bass_six = false;
    for (int i = 0; i < s_nout; i++) {
        none_err &= s_out[i].op != MOST_OP_ERROR;
        bass_six |= sent(i, 0x16E, 0x22, 0x05, 0x202, MOST_OP_STATUS, b6, 1);
    }
    check(none_err && bass_six, "Notification Set all from 016E -> the values there are, Bass among them, no errors");

    /* ClearFunction after SetAll: 016E stays registered for the other 13 properties (MOST 2.5, p. 103) */
    EXT_RAM_BSS_ATTR static most_long_t l2;
    const uint8_t clr[] = { 0x03, 0x01, 0x6E, 0x02, 0x02 }, dev[] = { 0x01, 0x6E };
    rx(MOST_RX_LOGICAL, 0x16E, 0x22, 0x05, 0x001, MOST_OP_SET, 0, clr, 5);
    bool quiet2 = s_nout == 0;
    rx(MOST_RX_LOGICAL, 0x16E, 0x22, 0x05, 0x002, MOST_OP_GET, 0, dev, 2);
    bool listed = quiet2 && sent_long(&l2) && l2.op == MOST_OP_STATUS && l2.len == 2 + 2 * 13, bass = false;
    for (int i = 2; listed && i + 1 < l2.len; i += 2) bass |= l2.data[i] == 0x02 && l2.data[i + 1] == 0x02;
    check(listed && !bass, "SetAll, ClearFunction Bass -> NotificationCheck lists the 13 other properties");

    /* a speaker the head unit never set keeps -99 dB: the row's level is the loudest of the ones it did set */
    const uint8_t sp3[] = { 0x03, 0x03, 0xE2 }, all3[] = { 0x03, 0x00 }, lv3[] = { 0x00, 0x00, 0x9D, 0x9D, 0xE2, 0x9D, 0x9D };
    ask(0x46D, MOST_OP_SET, sp3, 3);
    ask(0x46D, MOST_OP_GET, all3, 2);
    check(answered(0x46D, MOST_OP_STATUS, lv3, 7), "SpeakerLevel of speaker 3 of row 3 first -> the others at -99 dB");
}

/* segments: two senders' messages interleaved both arrive whole; a next segment with no first gets Segmentation
   Error [0C 01] back (MOST 2.5, table 2-5) */
static void segmentation(void)
{
    EXT_RAM_BSS_ATTR static most_long_t la, lb, back;
    EXT_RAM_BSS_ATTR static most_msg_t ta[4], tb[4];
    la = (most_long_t){ .type = MOST_RX_LOGICAL, .src = 0x161, .fblock = 0x22, .inst = 0x05, .fkt = 0x441,
                        .op = MOST_OP_SET, .len = 20 };
    lb = la;
    lb.src = 0x16E;
    for (int i = 0; i < 20; i++) { la.data[i] = (uint8_t)(0x10 + i); lb.data[i] = (uint8_t)(0x40 + i); }
    la.data[0] = 0x01; la.data[1] = 0x00;       /* Pos {1}, {2}: two entries */
    lb.data[0] = 0x02; lb.data[1] = 0x00;
    int na = most_split(&la, ta, 4), nb = most_split(&lb, tb, 4);
    for (int i = 0; i < na; i++) ta[i].src = 0x161;
    for (int i = 0; i < nb; i++) tb[i].src = 0x16E;
    const uint8_t clear_all[] = { 0x02, 0x01, 0x6E };  /* 016E off the matrix (arrays()), so the Sets stay silent */
    rx(MOST_RX_LOGICAL, 0x16E, 0x22, 0x05, 0x001, MOST_OP_SET, 0, clear_all, 3);
    s_nout = 0;
    amp_on_msg(&ta[0]);
    amp_on_msg(&tb[0]);
    amp_on_msg(&ta[1]);
    amp_on_msg(&tb[1]);
    bool quiet = s_nout == 0;
    const uint8_t pa[] = { 0x01, 0x00 }, pb[] = { 0x02, 0x00 };
    ask(0x441, MOST_OP_GET, pa, 2);
    bool a_ok = sent_long(&back) && back.len == 20 && !memcmp(back.data, la.data, 20);
    ask(0x441, MOST_OP_GET, pb, 2);
    bool b_ok = sent_long(&back) && back.len == 20 && !memcmp(back.data, lb.data, 20);
    check(na == 2 && nb == 2 && quiet && a_ok && b_ok, "two senders' SpeakerDelay in segments, interleaved: both whole");
    const uint8_t e0c[] = { MOST_ERR_SEGMENT, 0x01 };
    s_nout = 0;
    amp_on_msg(&ta[1]);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x05, 0x441, MOST_OP_ERROR, e0c, 2),
          "a last segment with no first -> Segmentation Error [0C 01] to the sender");
}

/* a request to another FBlock of ours from the gateway; the answer put together */
static bool asked_long(uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, const uint8_t *d, int n, most_long_t *l)
{
    rx(MOST_RX_LOGICAL, 0x161, fb, inst, fkt, op, 0, d, n);
    return sent_long(l) && l->dst == 0x161 && l->fblock == fb && l->inst == inst && l->fkt == fkt;
}

/* the pre-facelift amplifier's other FBlocks, their function lists byte for byte usb2most's, and the Diagnosis answers */
static void fblocks(void)
{
    static most_long_t l;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x20, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x20, 0x000, MOST_OP_STATUS, U2M_FKT_2220, sizeof U2M_FKT_2220),
          "22.20 FktIDs, as usb2most's");
    rx(MOST_RX_LOGICAL, 0x161, 0x23, 0x01, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x23, 0x01, 0x000, MOST_OP_STATUS, U2M_FKT_HAMP, sizeof U2M_FKT_HAMP),
          "HeadphoneAmplifier 23.01 FktIDs, as usb2most's");
    check(asked_long(0x06, 0x86, 0x000, MOST_OP_GET, NULL, 0, &l) && l.op == MOST_OP_STATUS &&
          l.len == sizeof U2M_FKT_DIAG_PREM && !memcmp(l.data, U2M_FKT_DIAG_PREM, sizeof U2M_FKT_DIAG_PREM),
          "Diagnosis 06.86 FktIDs: 38 bytes, as usb2most's for Premium");

    const uint8_t pn_q[] = { 0xF1, 0x12 }, sn_q[] = { 0xF1, 0x8C }, no_q[] = { 0xF1, 0x99 }, not_f1[] = { 0xF2, 0x12 };
    uint8_t pn[2 + 24] = { 0xF1, 0x12 };
    memcpy(pn + 2, "6H52-18C808-DD", 14);
    check(asked_long(0x06, 0x86, 0xD22, MOST_OP_SETGET, pn_q, 2, &l) && l.op == MOST_OP_STATUS && l.len == sizeof pn &&
          !memcmp(l.data, pn, sizeof pn), "D22 [F1 12] -> Status [F1 12, 6H52-18C808-DD, zeros to 24]");
    check(asked_long(0x06, 0x86, 0xD22, MOST_OP_SETGET, sn_q, 2, &l) && l.len == 18 && !memcmp(l.data + 2, "000000", 6),
          "D22 [F1 8C] -> the serial number's 16 bytes, zeros until the car's is read");
    const uint8_t e6[] = { MOST_ERR_PARAM, 0x01, 0xF1, 0x99 };
    rx(MOST_RX_LOGICAL, 0x161, 0x06, 0x86, 0xD22, MOST_OP_SETGET, 0, no_q, 2);
    check(s_nout == 1 && sent(0, 0x161, 0x06, 0x86, 0xD22, MOST_OP_ERROR, e6, 4), "D22 [F1 99] -> Error [06 01 F1 99], as usb2most");
    rx(MOST_RX_LOGICAL, 0x161, 0x06, 0x86, 0xD22, MOST_OP_SETGET, 0, not_f1, 2);
    check(s_nout == 0, "D22 [F2 12] -> no answer, as usb2most");

    /* the DTC service: usb2most's fixed answers, out of its firmware */
    const uint8_t q0[] = { 0x02, 0xAF }, q1[] = { 0x04, 0x9D, 0x84, 0x13, 0x55 }, q2[] = { 0x06, 0x9D, 0x84, 0x13, 0x00 },
                  q3[] = { 0x04, 0xF0, 0x00, 0x87, 0x01 }, q4[] = { 0x06, 0xF0, 0x00, 0x87, 0x01 }, qx[] = { 0x05, 0xAF };
    check(asked_long(0x06, 0x86, 0xD19, MOST_OP_SETGET, q0, 2, &l) && l.len == sizeof U2M_DTC_R0 && !memcmp(l.data, U2M_DTC_R0, l.len),
          "D19 [02 AF] -> usb2most's 38 bytes");
    check(asked_long(0x06, 0x86, 0xD19, MOST_OP_SETGET, q1, 5, &l) && l.len == sizeof U2M_DTC_R1 && !memcmp(l.data, U2M_DTC_R1, l.len),
          "D19 [04 9D 84 13 xx] -> usb2most's 28 bytes");
    check(asked_long(0x06, 0x86, 0xD19, MOST_OP_SETGET, q2, 5, &l) && l.len == sizeof U2M_DTC_R2 && !memcmp(l.data, U2M_DTC_R2, l.len),
          "D19 [06 9D 84 13 xx] -> usb2most's 21 bytes");
    check(asked_long(0x06, 0x86, 0xD19, MOST_OP_SETGET, q3, 5, &l) && l.len == sizeof U2M_DTC_R3 && !memcmp(l.data, U2M_DTC_R3, l.len),
          "D19 [04 F0 00 87 xx] -> usb2most's 28 bytes");
    check(asked_long(0x06, 0x86, 0xD19, MOST_OP_SETGET, q4, 5, &l) && l.len == sizeof U2M_DTC_R4 && !memcmp(l.data, U2M_DTC_R4, l.len),
          "D19 [06 F0 00 87 xx] -> usb2most's 21 bytes");
    rx(MOST_RX_LOGICAL, 0x161, 0x06, 0x86, 0xD19, MOST_OP_SETGET, 0, qx, 2);
    bool quiet = s_nout == 0;
    rx(MOST_RX_LOGICAL, 0x161, 0x06, 0x86, 0xD10, MOST_OP_GET, 0, NULL, 0);
    check(quiet && s_nout == 0, "D19 of another request, D10 -> no answer, as usb2most");

    const uint8_t con[] = { 0x01, 0x00, 0x10, 0x11 }, one[] = { 0x01 };
    rx(MOST_RX_LOGICAL, 0x161, 0x23, 0x01, 0x111, MOST_OP_STARTRESULT, 0, con, 4);
    check(s_nout == 1 && sent(0, 0x161, 0x23, 0x01, 0x111, MOST_OP_RESULT, one, 1),
          "HeadphoneAmplifier Connect -> Result [01], nothing routed");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x07, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 0, "22.07 (no such instance) -> no answer, as usb2most");
    /* LimiterSettings 427h is in both 22.05 and 22.20: a value set in one is not the other's */
    const uint8_t lim[] = { 0x00, 0x01, 0xAA }, pos[] = { 0x00, 0x01 }, e7l[] = { MOST_ERR_NOTAVAIL, 0x01, 0x00 };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x20, 0x427, MOST_OP_SET, 0, lim, 3);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x20, 0x427, MOST_OP_GET, 0, pos, 2);
    bool kept = s_nout == 1 && sent(0, 0x161, 0x22, 0x20, 0x427, MOST_OP_STATUS, lim, 3);
    ask(0x427, MOST_OP_GET, pos, 2);
    check(kept && answered(0x427, MOST_OP_ERROR, e7l, 3), "22.20 427 Set and Get kept apart from 22.05's 427");
}

/* AUD3: its FBlocks and functions out of usb2most's firmware, no Mute, Balance kept, its sinks and its source (stage 4),
   what it asks the master */
static void aud3(void)
{
    static most_long_t l;
    rx(MOST_RX_PHYSICAL, 0x161, 0x01, 0x04, 0x000, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x01, 0x04, 0x000, MOST_OP_STATUS, U2M_FBLOCKS_AUD3, 6),
          "AUD3: NetBlock -> [22.0A 06.86 23.01]");
    check(asked_long(0x22, 0x0A, 0x000, MOST_OP_GET, NULL, 0, &l) && l.len == sizeof U2M_FKT_AUD3 && !memcmp(l.data, U2M_FKT_AUD3, l.len),
          "AUD3 22.0A FktIDs: 29 bytes, as usb2most's");
    check(asked_long(0x23, 0x01, 0x000, MOST_OP_GET, NULL, 0, &l) && l.len == sizeof U2M_FKT_AUD3_HAMP && !memcmp(l.data, U2M_FKT_AUD3_HAMP, l.len),
          "AUD3 23.01 FktIDs: 14 bytes, as usb2most's");
    check(asked_long(0x06, 0x86, 0x000, MOST_OP_GET, NULL, 0, &l) && l.len == sizeof U2M_FKT_AUD3_DIAG && !memcmp(l.data, U2M_FKT_AUD3_DIAG, l.len),
          "AUD3 06.86 FktIDs: 26 bytes, not ascending, as usb2most's");
    const uint8_t on[] = { 0x01, 0x01 }, e3[] = { MOST_ERR_FKT }, bal[] = { 0xFD };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x113, MOST_OP_SETGET, 0, on, 2);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x113, MOST_OP_ERROR, e3, 1), "AUD3 Mute -> Error 03, it has none");
    dsp_init(48000);    /* what the checks before left muted goes */
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x200, MOST_OP_SET, 0, bal, 1);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x200, MOST_OP_GET, 0, NULL, 0);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x200, MOST_OP_STATUS, bal, 1), "AUD3 Balance Set -3, Get -> -3");
    /* and it is heard: -3 steps to the right, the left output 6 dB down; AUD3's levels are the loudest for both sides */
    const uint8_t lv0[] = { 0x01, 0x00, 0x00, 0xF6 };
    double gl, gr;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, lv0, 4);
    lr_gains(1, &gl, &gr);
    check(fabs(gl - 0.5012) < 0.002 && gr == 1, "AUD3 Balance -3 and a level of 0 -> the left output -6 dB, the right 0");
    dsp_init(48000);
    /* its sinks as usb2most's Connect handler takes them: main and video share the stereo sink, the last connected
       has it; 4 call, 6 radar, 7 navigation mono; 3 and 5 answered, not routed */
    const uint8_t main4[] = { 0x01, 0x00, 0x04, 0x05, 0x06, 0x07 }, vid6[] = { 0x02, 0x00, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                                                            0x0D };
    const uint8_t s1[] = { 0x01 }, s2[] = { 0x02 }, s3[] = { 0x03 };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x111, MOST_OP_STARTRESULT, 0, main4, 6);
    check(sent(0, 0x161, 0x22, 0x0A, 0x111, MOST_OP_RESULT, s1, 1) && routed(1, main4 + 2, 4),
          "AUD3 Connect main [1, 4 channels] -> the stereo sink, as the pre-facelift's");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x111, MOST_OP_STARTRESULT, 0, vid6, 8);
    check(sent(0, 0x161, 0x22, 0x0A, 0x111, MOST_OP_RESULT, s2, 1) && routed(1, vid6 + 2, 6),
          "AUD3 Connect video [2, 6 channels] -> takes the stereo sink (its first 4 are the stereo)");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x112, MOST_OP_STARTRESULT, 0, s2, 1);
    check(routed(1, main4 + 2, 4), "AUD3 DisConnect video -> the stereo sink back to main, still connected");
    const uint8_t radar[] = { 0x06, 0x00, 0x20, 0x21 }, chime[] = { 0x03, 0x00, 0x22, 0x23 };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x111, MOST_OP_STARTRESULT, 0, radar, 4);
    check(routed(3, radar + 2, 2), "AUD3 Connect radar [6, 2 channels] -> mono 1");
    int before = s_routes;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x111, MOST_OP_STARTRESULT, 0, chime, 4);
    check(sent(0, 0x161, 0x22, 0x0A, 0x111, MOST_OP_RESULT, s3, 1) && s_routes == before,
          "AUD3 Connect [3] -> Result, nothing routed");

    /* the source: 2 channels from the timing master, Result [SourceNr, SrcDelay, channels]; again the same ones */
    const uint8_t src[] = { 0x01 }, res[] = { 0x01, 0x05, 0x30, 0x31 }, nogo[] = { MOST_ERR_FUNC, 0x01, 0x02 };
    s_allocs = s_deallocs = 0;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x101, MOST_OP_STARTRESULT, 0, src, 1);
    bool first = s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x101, MOST_OP_RESULT, res, 4);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x101, MOST_OP_STARTRESULT, 0, src, 1);
    check(first && s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x101, MOST_OP_RESULT, res, 4) && s_allocs == 1,
          "AUD3 Allocate [01] -> Result [01, delay 5, channels 30 31], a second one the same channels");
    s_alloc_valid = false;                  /* the master started afresh: the ring's table no longer has them */
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x101, MOST_OP_STARTRESULT, 0, src, 1);
    s_alloc_valid = true;
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x101, MOST_OP_RESULT, res, 4) && s_allocs == 2,
          "AUD3 Allocate after the master started afresh -> channels asked for again, not the stale ones");
    s_dealloc_ok = false;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x102, MOST_OP_STARTRESULT, 0, src, 1);
    s_dealloc_ok = true;
    bool kept = s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x102, MOST_OP_RESULT, src, 1) && s_deallocs == 1;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x101, MOST_OP_STARTRESULT, 0, src, 1);
    check(kept && s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x101, MOST_OP_RESULT, res, 4) && s_allocs == 2,
          "AUD3 DeAllocate the master refused -> Result, the channels kept: the next Allocate has them unasked");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x102, MOST_OP_STARTRESULT, 0, src, 1);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x102, MOST_OP_RESULT, src, 1) && s_deallocs == 2,
          "AUD3 DeAllocate [01] -> Result [01], the channels given back");
    s_alloc_ok = false;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x101, MOST_OP_STARTRESULT, 0, src, 1);
    s_alloc_ok = true;
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x101, MOST_OP_ERROR, nogo, 3),
          "AUD3 Allocate, the master has no channels -> Error [20 01 02]");
    /* what it asks the master, whoever sent the OK: three messages (8038720-80387C6). On a car with AUD3 the master is the
       screen 016E, not the 0161 of the older cars (SDD: FCDIM) */
    const uint8_t ok[] = { 0x01 }, sub1[] = { 0x01, 0x01, 0x86, 0x02, 0x00 }, sub2[] = { 0x01, 0x01, 0x86, 0x03, 0x03 };
    rx(MOST_RX_BROADCAST, 0x16E, 0x02, 0x01, 0xA00, MOST_OP_STATUS, 0, ok, 1);
    check(s_nout == 3 && sent(0, 0x16E, 0xF5, 0x01, 0x305, MOST_OP_GET, U2M_GW_GET, 3) &&
          sent(1, 0x16E, 0xF5, 0x01, 0x001, MOST_OP_SET, sub1, 5) && sent(2, 0x16E, 0xF5, 0x01, 0x001, MOST_OP_SET, sub2, 5),
          "AUD3: network OK from 016E -> to 016E: F5.01 305 Get [26 28 24], Notification 0200, Notification 0303");
    const uint8_t e41[] = { 0x41, 0x01 };
    rx(MOST_RX_LOGICAL, 0x16E, 0xF5, 0x01, 0x001, MOST_OP_ERROR, 0, e41, 2);
    check(s_nout == 0, "AUD3: the master's Error [41 01] to the registration, as usb2most's own master gives -> no reaction");

    /* a head unit's start-up as usb2most's driver sends it to a real AUD3 (method +40, decode_aud3.md sect. 7) */
    const uint8_t c01[] = { 0x01, 0x01, 0x61, 0x0C, 0x01 }, e10[] = { 0x01, 0x01, 0x61, 0x0E, 0x10 },
                  d00[] = { 0x01, 0x01, 0x61, 0x0D, 0x00 }, f353[] = { 0x01, 0x01, 0x61, 0x03, 0x53 },
                  st1[] = { 0x01, 0x00, 0x02, 0x00, 0x06, 0x00, 0x01 }, st0[] = { 0x00, 0x00, 0x02, 0x00, 0x06, 0x00, 0x01 },
                  m0[] = { 0x02, 0x00, 0x00 }, b0[] = { 0x00 }, vol0[] = { 0x01, 0x00, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D, 0x9D };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x001, MOST_OP_SET, 0, c01, 5);
    bool q = s_nout == 0;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x001, MOST_OP_SET, 0, f353, 5);
    check(q && s_nout == 0, "AUD3: Notification for 0C01, 0353, in no list -> no answer, as usb2most");
    rx(MOST_RX_LOGICAL, 0x161, 0x06, 0x0A, 0x001, MOST_OP_SET, 0, e10, 5);
    check(s_nout == 1 && sent(0, 0x161, 0x06, 0x0A, 0xE10, MOST_OP_STATUS, NULL, 0),
          "AUD3: Notification for E10 on Diagnosis, under InstID 0A as the start-up asks -> an empty E10 Status");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x001, MOST_OP_SET, 0, d00, 5);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0xD00, MOST_OP_STATUS, st1, 7),
          "AUD3: Notification for D00 -> Status [01 00 02 00 06 00 01]");
    q = true;
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x20, 0x466, MOST_OP_SET, 0, U2M_MUTE_ROWS, 7);
    q &= s_nout == 0;
    for (int i = 1; i < 4; i++) {
        rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x466, MOST_OP_SET, 0, U2M_MUTE_ROWS + 7 * i, 7);
        q &= s_nout == 0;
    }
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, vol0, 9);
    q &= s_nout == 0;
    static const uint16_t tone[] = { 0x202, 0x203, 0x204, 0x200, 0xE09, 0xD01, 0xE53 };
    for (int i = 0; i < (int)(sizeof tone / sizeof tone[0]); i++) {
        rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, tone[i], MOST_OP_SET, 0, b0, 1);
        q &= s_nout == 0;
    }
    check(q, "AUD3: 22.20 466 (no such FBlock here), 466, 46D, Bass, Treble, Fader, Balance, E09, D01, E53 -> none answered");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0xE05, MOST_OP_SET, 0, m0, 3);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0xD00, MOST_OP_STATUS, st0, 7), "AUD3: E05 [02 00 00] -> D00 Status [00 00 02 00 06 00 01]");
    const uint8_t m5[] = { 0x02, 0x00, 0x05 }, m13[] = { 0x02, 0x00, 0x0D }, st4[] = { 0x04, 0x00, 0x02, 0x00, 0x06, 0x00, 0x01 },
                  st2[] = { 0x02, 0x00, 0x02, 0x00, 0x06, 0x00, 0x01 };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0xE05, MOST_OP_SET, 0, m5, 3);
    q = s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0xD00, MOST_OP_STATUS, st4, 7);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0xE05, MOST_OP_SET, 0, m13, 3);
    check(q && s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0xD00, MOST_OP_STATUS, st2, 7), "AUD3: E05 mode 5 -> D00 mode 4, mode 0D -> mode 2");

    /* the rows: SpeakerLevel numbers them apart from Connect: 1 main, 2 call (sink 4), 3 navigation (sink 7), 4 radar
       (sink 6). The processing's sinks: 1 stereo, 2 call, 3 radar, 4 navigation */
    const uint8_t r1[] ={ 0x01, 0x00, 0xF6, 0x9D, 0xF6, 0xF6, 0xF6, 0xF6, 0xF6 }, r2[] = { 0x02, 0x00, 0xE4, 0x9D, 0xE4, 0x9D, 0x9D, 0x9D, 0x9D },
                  r3[] = { 0x03, 0x00, 0xF0, 0x9D, 0xF0, 0x9D, 0x9D, 0x9D, 0x9D }, r4[] = { 0x04, 0x00, 0x9D, 0x9D, 0x9D, 0xE2, 0xE2, 0x9D, 0x9D };
    dsp_init(48000);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, r1, 9);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, r2, 9);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, r3, 9);
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_SET, 0, r4, 9);
    lr_gains(1, &gl, &gr);
    check(fabs(gl - 0.3162) < 0.002 && gr == gl, "AUD3: row 1 -10 dB -> the music at -10 dB, the other rows do not touch it");
    lr_gains(2, &gl, &gr);
    check(fabs(gl - 0.0398) < 0.001, "AUD3: row 2 -28 dB -> the call's sink (4) at -28 dB");
    lr_gains(3, &gl, &gr);
    check(fabs(gl - 0.0316) < 0.001, "AUD3: row 4 -30 dB -> the radar's sink (6) at -30 dB");
    lr_gains(4, &gl, &gr);
    check(fabs(gl - 0.1585) < 0.002, "AUD3: row 3 -16 dB -> the navigation's sink (7) at -16 dB");
    dsp_init(48000);
    const uint8_t pos[] = { 0x00, 0x00 }, g2[] = { 0x02, 0x00 }, st_r2[] = { 0x00, 0x00, 0xE4, 0x9D, 0xE4, 0x9D, 0x9D, 0x9D, 0x9D };
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_GET, 0, g2, 2);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0x46D, MOST_OP_STATUS, st_r2, 9), "AUD3: SpeakerLevel Get [02 00] -> [00 00, 7 levels]");
    rx_to(0x22, 0x0A, 0xE08, MOST_OP_GET, pos, 2);
    uint8_t e08[14];
    memcpy(e08, U2M_AUD3_E08_INIT, 14);
    e08[0] = e08[1] = 0;
    check(sent_long(&l) && l.len == 14 && !memcmp(l.data, e08, 14), "AUD3: E08 Get -> usb2most's starting value, first two bytes zeroed");

    /* a fresh start: the rows until set, usb2most's table */
    amp_init(&AMP_AUD3, capture);
    uint8_t all[2 + 28] = { 0x00, 0x00 };
    memcpy(all + 2, U2M_AUD3_LVL_INIT, 28);
    rx_to(0x22, 0x0A, 0x46D, MOST_OP_GET, pos, 2);
    check(sent_long(&l) && l.len == sizeof all && !memcmp(l.data, all, sizeof all), "AUD3: SpeakerLevel Get [00 00] at the start -> 4 x 7 at -99 dB");
    rx(MOST_RX_LOGICAL, 0x161, 0x22, 0x0A, 0xE05, MOST_OP_GET, 0, pos, 2);
    check(s_nout == 1 && sent(0, 0x161, 0x22, 0x0A, 0xE05, MOST_OP_STATUS, U2M_AUD3_E05_INIT, 7), "AUD3: E05 Get at the start -> usb2most's starting value");
}

int amp_selftest(void)
{
    const amp_profile_t *keep_p = amp_profile();
    amp_send_t keep_s = amp_sender();
    bool keep_on = amp_active();
    amp_set_active(false);      /* the ring's messages are dropped while the test owns the emulation (the console
                                   refuses the test while it is on) */
    const amp_io_t *keep_io = amp_set_io(&s_cap_io);    /* no routes, no REM, no allocation: the captures instead;
                                                           the sound processing it does drive, reset at the end */
    s_fail = s_pass = 0;
    printf("selftest: message layer\n");
    buffers();
    segments();
    printf("selftest: %s emulation\n", AMP_PRE_PREMIUM.name);
    amp_init(&AMP_PRE_PREMIUM, capture);
    start_up();
    netblock();
    notification();
    mute();
    levels();
    e0x();
    sinks_and_errors();
    arrays();
    segmentation();
    fblocks();
    amp_init(&AMP_PRE_PREMIUM, capture);
    sides();
    amp_init(&AMP_PRE_PREMIUM, capture);
    startup_premium();
    printf("selftest: %s emulation\n", AMP_PRE_HILINE.name);
    amp_init(&AMP_PRE_HILINE, capture);
    hiline();
    printf("selftest: %s emulation\n", AMP_AUD3.name);
    amp_init(&AMP_AUD3, capture);
    aud3();
    amp_init(keep_p ? keep_p : &AMP_PRE_HILINE, keep_s);   /* the state the test made is dropped */
    amp_set_io(keep_io);
    if (keep_io) {                                      /* and with it any sink of before: no routes, REM may go */
        if (keep_io->route) for (int d = 1; d <= DSP_SINKS; d++) keep_io->route(d, NULL, 0);
        if (keep_io->sound) keep_io->sound(false);
    }
    dsp_init(48000);                                    /* and what it told the sound processing */
    amp_set_active(keep_on);
    printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail;
}

/* ---- the phone's path ---- */

/* the converter alone: a sine at f_in through the filter at a fixed ratio, against the exact sine at the output
   positions. Full scale -1 dB in 16 bits: the input's own rounding allows about 97 dB */
static double src_snr(int f_in, double hz, double ppm)
{
    const double a = 0.891;
    int16_t *in = malloc(sizeof *in * 2 * 4096);    /* from the heap: no static room with Bluetooth on */
    if (!in || src_design(f_in, PHONE_OUT_RATE)) {
        free(in);
        return 0;
    }
    for (int i = 0; i < 4096; i++) {
        int16_t v = (int16_t)lrint(a * 32767 * sin(2 * M_PI * hz * i / f_in));
        in[2 * i] = v;
        in[2 * i + 1] = (int16_t)-v;        /* the right channel inverted: both are checked */
    }
    double step = (double)f_in / PHONE_OUT_RATE * (1 + ppm * 1e-6), sig = 0, err = 0;
    for (int j = 0;; j++) {
        double x = 100 + j * step;
        int i = (int)x;
        if (i + SRC_TAPS / 2 >= 4096) break;
        uint32_t frac = (uint32_t)((x - i) * 4294967296.0);
        double e = a * 32767 / 32768 * sin(2 * M_PI * hz * (i + frac / 4294967296.0) / f_in);
        float y[2];
        src_frame(&in[2 * (i - SRC_TAPS / 2 + 1)], frac, y);
        sig += 2 * e * e;
        err += (y[0] - e) * (y[0] - e) + (y[1] + e) * (y[1] + e);
    }
    free(in);
    return 10 * log10(sig / err);
}

/* the rate control in simulated time: the phone's clock 300 ppm fast, bursts of 512 frames with +-10 ms jitter and a
   60 ms hold-up every 10 s (then the backlog at once), the ring pulling 192 frames every 4 ms as the I2S task does.
   Only the clocks run, no sound is computed. Judged from 200 s to 330 s, when the start is long over: the correction
   must average the clocks' difference, wander little with the jitter and the hold-ups, and step by no more than a
   couple of ppm from one block to the next - a bigger step is audible as a fast wobble on high notes long before the
   slow wander is; the fill must average its target */
static void lock_test(void)
{
    EXT_RAM_BSS_ATTR static int16_t chunk[512 * 2];
    const double fp = 44100 * (1 + 300e-6);
    double t_in = 0, t_out = 0, c_sum = 0, c_min = 1e9, c_max = -1e9, c_prev = 0, c_step = 0, f_sum = 0;
    uint32_t seed = 12345, k = 0, under0 = 0;
    int n = 0;
    bool started = false;
    phone_stats_t st;
    phone_start(PHONE_VIRTUAL, 44100);
    while (t_out < 330) {
        if (t_in <= t_out) {
            phone_feed(PHONE_VIRTUAL, chunk, 512);
            double nom = ++k * 512 / fp, hold = fmod(nom, 10);
            seed = seed * 1664525u + 1013904223u;
            t_in = hold < 0.060 ? nom - hold + 0.060 : nom + ((seed >> 8) / 16777216.0 - 0.5) * 0.020;
            continue;
        }
        phone_pull(NULL, 192);
        t_out += 192.0 / PHONE_OUT_RATE;
        phone_stats(&st);
        if (st.playing && !started) {
            started = true;
            under0 = st.underruns;
        }
        if (t_out >= 200) {
            c_sum += st.corr_ppm;
            f_sum += st.fill_ms;
            c_min = fmin(c_min, st.corr_ppm);
            c_max = fmax(c_max, st.corr_ppm);
            if (n) c_step = fmax(c_step, fabs(st.corr_ppm - c_prev));
            n++;
        }
        c_prev = st.corr_ppm;
    }
    char what[128];
    snprintf(what, sizeof what, "rate control, phone +300 ppm, 200-330 s: correction %+.1f ppm on average, wander %.0f "
             "ppm, step %.2f ppm", c_sum / n, c_max - c_min, c_step);
    check(started && fabs(c_sum / n - 300) < 5 && c_max - c_min < 250 && c_step < 2, what);
    snprintf(what, sizeof what, "fill %.1f ms on average (target 100), underruns after the start %" PRIu32
             ", overruns %" PRIu32, f_sum / n, st.underruns - under0, st.overruns);
    check(fabs(f_sum / n - 100) < 1 && st.underruns == under0 && !st.overruns, what);
    phone_stop(PHONE_VIRTUAL);
}

int phone_selftest(void)
{
    if (phone_active()) {
        printf("phone test: a stream is on, stop it first\n");
        return 1;
    }
    static const struct { int f; double ppm; } r[] = { { 44100, 0 }, { 48000, 300 } };
    static const double hz[] = { 1000, 10000, 15000 };
    char what[112];
    s_fail = s_pass = 0;
    printf("selftest: phone, sample-rate converter\n");
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++) {
            double snr = src_snr(r[i].f, hz[k], r[i].ppm);
            snprintf(what, sizeof what, "%d Hz %+.0f ppm -> 48000 Hz, %.0f Hz at -1 dBFS: SNR %.1f dB", r[i].f,
                     r[i].ppm, hz[k], snr);
            check(snr > 90, what);
        }
    printf("selftest: phone, rate control\n");
    lock_test();
    printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail;
}

/* ---- the sound processing ---- */

/* sink 1 (and sink 2, if given) through dsp_process in blocks of 192 frames, as the I2S task runs it */
static void dsp_run(const int32_t *in, const int32_t *two, int32_t *out, int n)
{
    for (int i = 0; i < n; i += 192) {
        int k = n - i < 192 ? n - i : 192;
        const int32_t *mono[3] = { two ? two + i : NULL, NULL, NULL };
        dsp_process(in + 2 * i, mono, out + 2 * i, k);
    }
}

/* output over input in dB for frames [a, b), left channel */
static double gain_db(const int32_t *in, const int32_t *out, int a, int b)
{
    double si = 0, so = 0;
    for (int i = a; i < b; i++) {
        si += (double)in[2 * i] * in[2 * i];
        so += (double)out[2 * i] * out[2 * i];
    }
    return 10 * log10(so / si);
}

static void sine(int32_t *x, int n, double hz, double amp)
{
    for (int i = 0; i < n; i++) x[2 * i] = x[2 * i + 1] = (int32_t)lrint(amp * 2147483648.0 * sin(2 * M_PI * hz * i / 48000));
}

int dsp_selftest(void)
{
    enum { N = 9600 };
    int32_t *in = malloc(sizeof *in * 2 * N), *out = malloc(sizeof *out * 2 * N), *two = malloc(sizeof *two * N);
    char what[128];
    if (!in || !out || !two) {
        printf("dsp test: no memory\n");
        free(in);
        free(out);
        free(two);
        return 1;
    }
    s_fail = s_pass = 0;
    printf("selftest: sound processing\n");

    /* neutral: 0 dB, unmuted, tone flat, sink 2 present but silent - every 16-bit sample comes out as it went in */
    dsp_init(48000);
    dsp_level(1, 0);
    memset(in, 0, sizeof *in * 2 * N);
    memset(two, 0, sizeof *two * N);
    dsp_run(in, two, out, 960);                     /* the level's glide */
    uint32_t seed = 7;
    for (int i = 0; i < 2 * N; i++) {
        seed = seed * 1664525u + 1013904223u;
        in[i] = (int32_t)(seed & 0xFFFF0000u);      /* a random 16-bit sample, left-justified */
    }
    dsp_run(in, two, out, N);
    int diff = 0;
    for (int i = 0; i < 2 * N; i++) diff += out[i] != in[i];
    snprintf(what, sizeof what, "neutral: %d random 16-bit frames bit for bit, %d differ", N, diff / 2);
    check(!diff, what);

    /* a level */
    dsp_level(1, -20);
    sine(in, N, 1000, 0.25);
    dsp_run(in, NULL, out, N);
    double g = gain_db(in, out, 960, N);
    snprintf(what, sizeof what, "level -20 dB: %.4f dB", g);
    check(fabs(g + 20) < 0.001, what);

    /* a linear mute of 100 ms: half way at 50 ms, silence from 100 ms on */
    dsp_level(1, 0);
    dsp_run(in, NULL, out, 960);
    dsp_mute_params(1, 100, 100, DSP_LINEAR, DSP_LINEAR);
    dsp_mute(1, true);
    for (int i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = 0x40000000;
    dsp_run(in, NULL, out, N);
    double half = (double)out[2 * 2400] / in[2 * 2400];
    bool silent = true;
    for (int i = 4800; i < N; i++) silent &= !out[2 * i] && !out[2 * i + 1];
    snprintf(what, sizeof what, "mute, linear 100 ms: %.3f at 50 ms, silent from 100 ms: %s", half, silent ? "yes" : "no");
    check(fabs(half - 0.5) < 0.01 && silent, what);
    /* and out again, degressive over 100 ms: fast first, 0.4375 at a quarter */
    dsp_mute_params(1, 100, 100, DSP_LINEAR, DSP_DEGRESSIVE);
    dsp_mute(1, false);
    dsp_run(in, NULL, out, N);
    double q = (double)out[2 * 1200] / in[2 * 1200], back = (double)out[2 * 6000] / in[2 * 6000];
    snprintf(what, sizeof what, "unmute, degressive 100 ms: %.3f at 25 ms, %.3f after", q, back);
    check(fabs(q - 0.4375) < 0.01 && back == 1.0, what);

    /* tone: 3 steps of Bass and of Treble (+6 dB). The shelves (RBJ, slope 1, 100 Hz and 10 kHz, as a state-variable
       filter) give +5.95 dB at 30 Hz, +5.76 dB at 16 kHz and 0.00 dB at 1 kHz, computed from their coefficients */
    static const struct { int bass, treble; double hz, lo, hi; } t[] = {
        { 3, 0, 30, 5.80, 6.10 }, { 3, 0, 1000, -0.05, 0.05 }, { 0, 3, 16000, 5.61, 5.91 }, { 0, 3, 1000, -0.05, 0.05 },
    };
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
        dsp_init(48000);
        dsp_level(1, 0);
        dsp_tone(t[k].bass, t[k].treble);
        sine(in, N, t[k].hz, 0.1);
        dsp_run(in, NULL, out, N);
        g = gain_db(in, out, 4800, N);
        snprintf(what, sizeof what, "%s +6 dB at %.0f Hz: %+.2f dB", t[k].bass ? "bass" : "treble", t[k].hz, g);
        check(g >= t[k].lo && g <= t[k].hi, what);
    }

    /* sink 2 into both channels at its own level, over a silent sink 1 */
    dsp_init(48000);
    dsp_level(2, -6);
    memset(in, 0, sizeof *in * 2 * N);
    for (int i = 0; i < N; i++) two[i] = (int32_t)lrint(0.3 * 2147483648.0 * sin(2 * M_PI * 440 * i / 48000));
    dsp_run(in, two, out, N);
    double worst = 0;
    for (int i = 960; i < N; i++) {
        double want = two[i] * pow(10, -6 / 20.0);
        worst = fmax(worst, fmax(fabs(out[2 * i] - want), fabs(out[2 * i + 1] - want)) / 2147483648.0);
    }
    snprintf(what, sizeof what, "sink 2 at -6 dB into left and right: worst error %.1e of full scale", worst);
    check(worst < 1e-6, what);

    /* balance -5, to the right: the left side 10 dB down once the glide is over, the right as it was */
    dsp_init(48000);
    dsp_level(1, 0);
    dsp_balance(-5);
    sine(in, N, 1000, 0.25);
    dsp_run(in, NULL, out, N);
    double sl = 0, sr = 0, si = 0;
    for (int i = 4800; i < N; i++) {
        si += (double)in[2 * i] * in[2 * i];
        sl += (double)out[2 * i] * out[2 * i];
        sr += (double)out[2 * i + 1] * out[2 * i + 1];
    }
    double dl = 10 * log10(sl / si), dr = 10 * log10(sr / si);
    snprintf(what, sizeof what, "balance -5 (to the right): left %+.2f dB, right %+.2f dB", dl, dr);
    check(fabs(dl + 10) < 0.01 && fabs(dr) < 1e-6, what);

    /* the output gate: closed over 100 ms, half way at 50 ms, silent from 100 ms; opened at once, bit for bit */
    dsp_init(48000);
    dsp_level(1, 0);
    for (int i = 0; i < N; i++) in[2 * i] = in[2 * i + 1] = 0x40000000;
    dsp_run(in, NULL, out, 960);
    dsp_gate(false, 100);
    dsp_run(in, NULL, out, N);
    half = (double)out[2 * 2400] / in[2 * 2400];
    silent = true;
    for (int i = 4800; i < N; i++) silent &= !out[2 * i] && !out[2 * i + 1];
    dsp_gate(true, 0);
    dsp_run(in, NULL, out, 192);
    bool open = out[2 * 100] == in[2 * 100] && out[2 * 100 + 1] == in[2 * 100 + 1];
    snprintf(what, sizeof what, "gate: %.3f at 50 ms, silent from 100 ms: %s, open at once: %s", half,
             silent ? "yes" : "no", open ? "yes" : "no");
    check(fabs(half - 0.5) < 0.01 && silent && open, what);

    dsp_init(48000);
    free(in);
    free(out);
    free(two);
    printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail;
}
