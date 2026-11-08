/* Stage 2 log, stage 3 amplifier emulation, stage 4 the sinks' sound: a Connect's channels go to the sound processing
   (amp_io_t: the OS8104A's routes, REM and the DAC), AUD3's source gets channels of its own. What the node answers,
   and how, checked function by function in docs/usb2most/check_amp.md:
   - usb2most, which works in the cars as these amplifiers (docs/usb2most/decode_prefacelift.md, decode_aud3.md: what its
     code does, read from main-v24.bin): the bytes on the wire wherever it has them, its silence where a known request
     gets none;
   - a working JLR node (JLR-HU: FBlocks.ts, findings/startUpFlow.json; stock_amps.md source HU): the XF amplifier
     subscribes to 303h of FBlock F5h in the gateway;
   - MOST Specification 2.5 for what neither shows: errors and their ErrorInfo, none to a group or broadcast (p. 37,
     table 2-5), Notification (2.3.12); the function formats FBlock AudioAmplifier 2V4-3, GeneralFBlock 2.5.1.
   Inference, marked in check_amp.md, for the first run in a ring: the amplifier's own range of Bass and Treble, the
   meaning of the E0xh fields, row 3 of AUD3 being the navigation */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "amp.h"
#include "dsp.h"
#include "learn.h"
#include "music.h"
#include "os8104.h"
#include "power.h"

static const char *TAG = "amp";

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

/* The profiles. The FBlocks, their functions and the part numbers are usb2most's (docs/usb2most: main-v24.bin, FBlocks
   at 2E9FE and 2EC2C, functions at 259C0-25B33, 277F4 and 2790F-27946, part numbers at 25A2C), the node and the
   gateway's 303h from JLR-HU on the XF (stock_amps.md 5). The node: in JLR the Diagnosis FBlock's InstID is the low
   byte of the node address (the XF's amplifier 186h with Diagnosis 86h; usb2most's audio unit 180h with 80h), and
   every amplifier set has Diagnosis 86h. Group 322h as usb2most reports it, the chip's register 22h.

   Pre-facelift: one amplifier set up for the car by its scene file. Hi-Line's main AudioAmplifier is 22.02 with E08h of
   JLR's own functions, Premium's 22.05 with E04h, E05h, E07h, E08h (usb2most picks by the type, 80322FC; SG1's photo
   shows 22.02 on 6H52-18C808-CD; JLR-HU saw the Premium list on an XF). Both are the same to a head unit otherwise */
static const uint16_t pre_amp[] = { 0x000, 0x001, 0x002, 0x111, 0x112, 0x113, 0x202, 0x203, 0x427, 0x430, 0x431,
                                    0x441, 0x461, 0x466, 0x46D, 0xE04, 0xE05, 0xE07, 0xE08 };
static const uint16_t pre_hi_amp[] = { 0x000, 0x001, 0x002, 0x111, 0x112, 0x113, 0x202, 0x203, 0x427, 0x430, 0x431,
                                       0x441, 0x461, 0x466, 0x46D, 0xE08 };
/* as usb2most answers for Hi-Line (25AA6): the list, then four bytes of zeros */
static const uint8_t pre_hi_amp_raw[] = { 0x00, 0x31, 0x11, 0x11, 0x42, 0x02, 0x20, 0x44, 0x27, 0x42, 0x84, 0x30, 0x43, 0x24,
                                          0x41, 0x44, 0x24, 0x61, 0x46, 0x24, 0x66, 0x46, 0x74, 0x6D, 0x46, 0xEE, 0x08, 0xE0,
                                          0x90, 0x00, 0x00, 0x00, 0x00 };
static const uint16_t pre_amp2[] = { 0x000, 0x001, 0x002, 0x427, 0x466, 0xE09 };                 /* 22.20 */
static const uint16_t pre_hamp[] = { 0x000, 0x001, 0x002, 0x111, 0x112, 0x113, 0x466, 0xE00 };   /* HeadphoneAmplifier */
static const uint16_t diag[] = { 0x000, 0x001, 0x002, 0xD10, 0xD11, 0xD14, 0xD19, 0xD22, 0xD27, 0xD2E, 0xD31, 0xD34,
                                 0xD36, 0xD37, 0xD3E, 0xD85, 0xE10 };
/* Hi-Line's Diagnosis list as usb2most answers it (25AE7): D35h, D36h twice, not ascending */
static const uint8_t diag_hi_raw[] = { 0x00, 0x3D, 0x10, 0xD1, 0x2D, 0x14, 0xD1, 0x5D, 0x19, 0xD1, 0xAD, 0x22, 0xD2, 0x3D,
                                       0x27, 0xD2, 0x8D, 0x2E, 0xD2, 0xFD, 0x31, 0xD3, 0x2D, 0x34, 0xD3, 0x5D, 0x36, 0xD3,
                                       0x5D, 0x36, 0xD3, 0xFD, 0x85, 0xD8, 0x6E, 0x10, 0xE1, 0x10 };
static const amp_fb_t pre_hi_fb[] = {
    { 0x22, 0x02, pre_hi_amp, N(pre_hi_amp), pre_hi_amp_raw, sizeof pre_hi_amp_raw },
    { 0x22, 0x20, pre_amp2, N(pre_amp2), NULL, 0 }, { 0x23, 0x01, pre_hamp, N(pre_hamp), NULL, 0 },
    { 0x06, 0x86, diag, N(diag), diag_hi_raw, sizeof diag_hi_raw },
};
static const amp_fb_t pre_prem_fb[] = {
    { 0x22, 0x05, pre_amp, N(pre_amp), NULL, 0 }, { 0x22, 0x20, pre_amp2, N(pre_amp2), NULL, 0 },
    { 0x23, 0x01, pre_hamp, N(pre_hamp), NULL, 0 }, { 0x06, 0x86, diag, N(diag), NULL, 0 },
};
static const uint16_t pre_subs[] = { 0x303 };
/* F18Ch the serial number: zeros until the car's own is read (stage 7); usb2most keeps a real amplifier's there */
#define DID(id, field, text) { id, field, (const uint8_t *)text, sizeof text - 1 }
static const amp_did_t pre_hi_dids[] = {
    DID(0xF18C, 16, "000000000000"), DID(0xF112, 24, "6H52-18C808-CD"), DID(0xF191, 24, "6H52-14C524-CB"),
    DID(0xF188, 24, "6H52-14C526-CD"), DID(0xF124, 24, "6H52-14C527-CB"),
};
static const amp_did_t pre_prem_dids[] = {
    DID(0xF18C, 16, "000000000000"), DID(0xF112, 24, "6H52-18C808-DD"), DID(0xF191, 24, "6H52-14C524-CB"),
    DID(0xF188, 24, "6H52-14C526-CD"), DID(0xF124, 24, "6H52-14C527-CB"),
};
/* the sinks (stock_amps.md 3, mObridge's table for JLR): 1 the music, stereo 16-bit in 4 channels; 2 phone, navigation
   and voice, 3 parking sensors, 4 chimes, mono. Their rows in SpeakerLevel have the same numbers: usb2most connects its
   second source to sink 4 and sets its level in row 4 */
static const amp_sink_t pre_sinks[] = { { 1, 1, 4, 1 }, { 2, 2, 2, 2 }, { 3, 3, 2, 3 }, { 4, 4, 2, 4 } };
const amp_profile_t AMP_PRE_HILINE = {
    .name = "pre-facelift Hi-Line", .node = 0x186, .group = 0x22, .fb = pre_hi_fb, .nfb = N(pre_hi_fb), .nlvl = 5,
    .gw_fblock = 0xF5, .gw_inst = 0x01, .subs = pre_subs, .nsubs = N(pre_subs),
    .dids = pre_hi_dids, .ndids = N(pre_hi_dids), .sinks = pre_sinks, .nsinks = N(pre_sinks),
};
const amp_profile_t AMP_PRE_PREMIUM = {
    .name = "pre-facelift Premium", .node = 0x186, .group = 0x22, .fb = pre_prem_fb, .nfb = N(pre_prem_fb), .nlvl = 5,
    .gw_fblock = 0xF5, .gw_inst = 0x01, .subs = pre_subs, .nsubs = N(pre_subs),
    .dids = pre_prem_dids, .ndids = N(pre_prem_dids), .sinks = pre_sinks, .nsinks = N(pre_sinks),
};

/* AUD3-8, -12, -16 (Meridian): alike on MOST (stock_amps.md 1). Balance and Fader instead of Mute; Allocate and
   DeAllocate, D00h, D01h. Of usb2most's two lists the one with E05h: its AUD3-16; SG3's photo shows E05h on an AUD3-12
   too. Its part numbers usb2most answers as the pre-facelift ones, and that works */
static const uint16_t aud3_amp[] = { 0x000, 0x001, 0x002, 0x101, 0x102, 0x111, 0x112, 0x200, 0x202, 0x203, 0x204,
                                     0x466, 0x46D, 0xD00, 0xD01, 0xE05, 0xE08, 0xE09 };
static const uint16_t aud3_hamp[] = { 0x000, 0x001, 0x002, 0x111, 0x112, 0x466, 0xE00, 0xE1C, 0xE1D, 0xE1E };
static const uint16_t aud3_diag[] = { 0x000, 0x001, 0x002, 0xD10, 0xD11, 0xD14, 0xD19, 0xD22, 0xD2E, 0xD31, 0xD3E, 0xD85 };
/* AUD3's Diagnosis list as usb2most answers it (278F5), not ascending */
static const uint8_t aud3_diag_raw[] = { 0x00, 0x3D, 0x10, 0xD1, 0x2D, 0x22, 0xD2, 0x3D, 0x31, 0xD3, 0x2D, 0x3E, 0xD3, 0xFD,
                                         0x2E, 0xD2, 0xFD, 0x14, 0xD1, 0x5D, 0x19, 0xD1, 0xAD, 0x85, 0xD8, 0x60 };
static const amp_fb_t aud3_fb[] = {
    { 0x22, 0x0A, aud3_amp, N(aud3_amp), NULL, 0 }, { 0x06, 0x86, aud3_diag, N(aud3_diag), aud3_diag_raw, sizeof aud3_diag_raw },
    { 0x23, 0x01, aud3_hamp, N(aud3_hamp), NULL, 0 },
};
/* at Configuration OK, to whoever sent it (8038720-80387C6): 305h Get [26 28 24], then the two notifications */
static const uint8_t aud3_gw_get[] = { 0x26, 0x28, 0x24 };
static const uint16_t aud3_subs[] = { 0x200, 0x303 };
/* AUD3's sinks as usb2most's Connect handler takes them (main-v24.bin 80378C8): 1 main, stereo 16-bit in 4 channels
   like the pre-facelift's; 2 video in 6, of which usb2most routes the first 4, the stereo; 4 call, 6 radar (the
   parking sensors), 7 navigation, mono in 2. Sinks 3 and 5 it answers and does not route. SpeakerLevel and
   MuteParameters number them differently, rows 1..4: 1 main, 2 call, 4 radar (8037E66-8037EAA); 3 is what is left,
   the navigation (inference) */
static const amp_sink_t aud3_sinks[] = { { 1, 1, 4, 1 }, { 2, 1, 6, 1 }, { 3, 0, 0, 0 }, { 4, 2, 2, 2 }, { 5, 0, 0, 0 },
                                         { 6, 3, 2, 4 }, { 7, 4, 2, 3 } };
const amp_profile_t AMP_AUD3 = {
    .name = "AUD3", .node = 0x186, .group = 0x22, .fb = aud3_fb, .nfb = N(aud3_fb), .nlvl = 7,
    .gw_fblock = 0xF5, .gw_inst = 0x01, .gw_get = aud3_gw_get, .ngw_get = sizeof aud3_gw_get,
    .subs = aud3_subs, .nsubs = N(aud3_subs),
    .dids = pre_hi_dids, .ndids = N(pre_hi_dids), .sinks = aud3_sinks, .nsinks = N(aud3_sinks),
};

const amp_profile_t *amp_profile_of(unsigned type)
{
    static const amp_profile_t *const by_type[] = { &AMP_PRE_HILINE, &AMP_PRE_PREMIUM, &AMP_AUD3 };
    return type < (unsigned)N(by_type) ? by_type[type] : &AMP_PRE_HILINE;
}

#define NB_FBLOCK   0x01    /* NetBlock */
#define NM_FBLOCK   0x02    /* NetworkMaster */
#define NM_CONFIG   0xA00   /* its Configuration, Status [01h] = network OK (startUpFlow.json) */
#define DIAG_FBLOCK 0x06    /* Diagnosis */
#define AMP_FBLOCK  0x22    /* AudioAmplifier */
#define F_FKTIDS    0x000
#define F_NOTIFY    0x001
#define F_NOTIFYCHK 0x002
#define F_ALLOC     0x101
#define F_DEALLOC   0x102
#define F_DID       0xD22   /* Diagnosis: SetGet [DID] -> Status [DID, value] (usb2most) */
#define F_CONNECT   0x111
#define F_DISCONN   0x112
#define F_MUTE      0x113
#define F_BALANCE   0x200   /* AUD3: -10..+10, a negative value moves the sound to the right (stock_amps.md 5). The
                               pre-facelift has none: its balance is in SpeakerLevel, speaker by speaker */
#define F_BASS      0x202
#define F_TREBLE    0x203
#define F_MUTEPAR   0x466
#define F_LEVEL     0x46D
#define F_E04       0xE04   /* JLR's own arrays: usb2most keeps them with these starting values, and answers Get */
#define F_E05       0xE05
#define F_E07       0xE07
#define F_E08       0xE08
#define F_D00       0xD00   /* AUD3: a Status only (8038878): the mode, then six fixed bytes */
#define F_DTC       0xD19   /* Diagnosis, fixed answers to fixed requests */
#define F_DIAG_NOTE 0xE10   /* Diagnosis: what AUD3 answers a registration for, though it is not in its list */
#define NSINK       7       /* the highest SinkNr of a profile (AUD3's navigation); each names its own (amp_sink_t) */
#define NROW        4       /* rows of SpeakerLevel and MuteParameters */
#define NSPK        7       /* levels per row: fl, c, fr, rl, rr (the pre-facelift), then sl, sr (AUD3) */
#define MAX_CH      60      /* a channel list: each 0..59, in the order of the sample's bytes */
#define TONE_MAX    6       /* where Increment and Decrement stop: the range the screen uses (stock_amps.md 5) */
#define LEVEL_MIN   (-127)  /* SpeakerLevel: -127 dB mutes, 0 dB is full */
#define LEVEL_OFF   (-99)   /* what a row holds until set, and what usb2most sends for a speaker it switches off */
#define POS_ROW     0       /* SpeakerLevel: [row, speaker (0 = all), dB...], as usb2most stores (8034140, 8037D74) */
#define POS_SPK     1       /* and sends them (80330E8, 8036C48), and as the catalogue's Pos {x sink, y speaker} */
#define ALL_FKTS    0xFFFF

typedef struct { bool on; uint8_t delay, nch; uint8_t ch[MAX_CH]; } sink_t;
typedef struct { uint8_t fb; uint16_t fkt, key; uint8_t len; uint8_t data[MOST_LONG]; } raw_t;
typedef struct { uint8_t fb; uint16_t dev, fkt; } sub_t;

static const amp_profile_t *P;
static const amp_fb_t *FB;              /* the FBlock the message being answered is for; FB = P->fb is the main one */
static amp_send_t S;
EXT_RAM_BSS_ATTR static most_rx_seg_t s_seg;    /* a slot per sender: 1 kB, in PSRAM */
static SemaphoreHandle_t s_mx;          /* the emulation's state: the amp task, the console, the selftest */
static uint8_t s_xts_last = 0x10;       /* bXTS of the last telegram sent (amp_tx); 00h: no node took it */
static sink_t s_sink[NSINK + 1];
static bool s_mute[NSINK + 1];
static bool s_mp_ok[NROW + 1];
static uint8_t s_mp[NROW + 1][6];       /* MuteParameters after the row: TimeIn, TimeOut (ms), ShapeIn, ShapeOut */
static int8_t s_tone[2];                /* Bass, Treble */
static int8_t s_lvl[NROW + 1][NSPK];    /* SpeakerLevel in dB, [row][speaker - 1] */
static uint8_t s_nlvl[NROW + 1];        /* speakers of the row with a level */
static uint8_t s_lset[NROW + 1];        /* those the head unit did set, a bit each: a level it sent counts, the starting
                                           value is no level at all */
/* JLR's own arrays as usb2most keeps them: starting values from flash (8045xxx, decode_prefacelift.md sect. 9), a Set of
   the whole length replaces them, Get answers them */
static const uint8_t E04_INIT[4] = { 0x00, 0x00, 0x00, 0x01 }, E05_INIT[7] = { 0x00, 0x00, 0x06, 0x00, 0x14, 0x03, 0x00 },
                     E07_INIT[6] = { 0x00, 0x00, 0xFD, 0xFD, 0xF6, 0xF6 },
                     E08_INIT[14] = { 0x00, 0x50, 0x00, 0x00, 0x07, 0x01, 0x01, 0x00, 0x00, 0x13, 0x88, 0x05, 0x01, 0x01 };
static uint8_t s_e04[4], s_e05[7], s_e07[6], s_e08[14];
/* the other arrays per Pos, E0xh whole; in PSRAM, internal RAM is short. The head unit's start-up upload of the
   equaliser alone is a Pos per output and band (stock_amps.md 2) */
EXT_RAM_BSS_ATTR static raw_t s_raw[128];
static int s_nraw;
static sub_t s_sub[48];
static int s_nsub;
static bool s_subscribed;
static const amp_io_t *IO;
static uint16_t s_master;               /* who sent the Configuration OK: the node the amplifier subscribed to */
static uint8_t s_owner[DSP_SINKS + 1];  /* the SinkNr each sink of the processing hears: AUD3's main and video share
                                           the stereo one, the one connected last has it */
static bool s_any;                      /* a routed sink is connected */
static uint8_t s_src_ch[8], s_src_n, s_src_delay;   /* the channels allocated for us as a source (AUD3) */

void amp_init(const amp_profile_t *p, amp_send_t send)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    P = p;
    FB = p->fb;
    S = send;
    memset(&s_seg, 0, sizeof s_seg);
    memset(s_sink, 0, sizeof s_sink);
    memset(s_mute, 0, sizeof s_mute);
    memset(s_mp_ok, 0, sizeof s_mp_ok);
    memset(s_tone, 0, sizeof s_tone);
    memset(s_lvl, LEVEL_OFF, sizeof s_lvl);    /* what Get answers until a level comes (9Dh, as usb2most) */
    memset(s_nlvl, 0, sizeof s_nlvl);
    memset(s_lset, 0, sizeof s_lset);
    memcpy(s_e04, E04_INIT, sizeof s_e04);
    memcpy(s_e05, E05_INIT, sizeof s_e05);
    memcpy(s_e07, E07_INIT, sizeof s_e07);
    memcpy(s_e08, E08_INIT, sizeof s_e08);
    memset(s_owner, 0, sizeof s_owner);
    for (int i = p->nsinks - 1; i >= 0; i--) if (p->sinks[i].dsp) s_owner[p->sinks[i].dsp] = p->sinks[i].sink;
    s_any = false;
    s_src_n = 0;
    s_nraw = s_nsub = 0;
    s_subscribed = false;
}

const amp_io_t *amp_set_io(const amp_io_t *io)
{
    const amp_io_t *was = IO;
    IO = io;
    return was;
}

const amp_profile_t *amp_profile(void) { return P; }
amp_send_t amp_sender(void) { return S; }
/* the network was found NotOK (MOST 2.5, p. 105): our notification matrix is deleted, and so is the gateway's with
   our entry in it - we register again at the next OK. The source's channels are checked at the next Allocate */
static void network_not_ok(void)
{
    if (s_nsub || s_subscribed) ESP_LOGI(TAG, "network NotOK: notification matrix cleared");
    s_nsub = 0;
    s_subscribed = false;
}

static bool has_fkt(uint16_t f)
{
    for (int i = 0; i < FB->nfkts; i++) if (FB->fkts[i] == f) return true;
    return false;
}

static uint8_t fb_index(void) { return (uint8_t)(FB - P->fb); }

/* properties are notified; on the XF list everything from 200h up is one, of the rest only Mute */
static bool is_prop(uint16_t f) { return f == F_MUTE || f >= 0x200; }

/* the profile's sink with this SinkNr, NULL if it has none */
static const amp_sink_t *sink_of(int n)
{
    for (int i = 0; i < P->nsinks; i++) if (P->sinks[i].sink == n) return &P->sinks[i];
    return NULL;
}

/* the processing's sink that SinkNr s drives now, 0 if none */
static int dsp_of(int s)
{
    const amp_sink_t *k = sink_of(s);
    return k && k->dsp && s_owner[k->dsp] == s ? k->dsp : 0;
}

/* the speakers by SpeakerLevel index, as usb2most lays the levels out (80330E8, 8036C48: fl, c, fr, rl, rr, then sl, sr
   on AUD3; the subwoofer goes by E09h, not in the array): which side each is on, 0 the centre */
static const int8_t s_side[NSPK] = { -1, 0, 1, -1, 1, -1, 1 };

static bool main_has(uint16_t f)
{
    for (int i = 0; i < P->fb[0].nfkts; i++) if (P->fb[0].fkts[i] == f) return true;
    return false;
}

/* the row's level for the left and the right output. An amplifier without Balance (the pre-facelift) gets it as the
   levels of the speakers on each side: the loudest of a side is that output's level, -127 dB being silence as for any
   level. The loudest of all stands in for a side the head unit never set (a mono sink with one level), and for both
   when both sides are silent (a sink played through the centre alone). One with Balance (AUD3) takes the loudest for
   both: its balance comes as 200h, and the levels are the same on both sides. A level above 0 dB is 0 dB here: a
   head unit's centre can be above its door speakers at full volume (usb2most does not clamp that one, 8033604), and the
   output must not go over */
static void row_levels(int row, float *l, float *r)
{
    int8_t top = LEVEL_MIN, side[2] = { LEVEL_MIN, LEVEL_MIN };
    bool set[2] = { false, false };
    for (int k = 0; k < s_nlvl[row]; k++) {
        if (!(s_lset[row] >> k & 1)) continue;
        int8_t v = s_lvl[row][k];
        if (v > 0) v = 0;
        if (v > top) top = v;
        if (!s_side[k]) continue;
        int c = s_side[k] > 0;
        set[c] = true;
        if (v > side[c]) side[c] = v;
    }
    bool sides = !main_has(F_BALANCE) && (side[0] > LEVEL_MIN || side[1] > LEVEL_MIN);
    *l = sides && set[0] ? side[0] : top;
    *r = sides && set[1] ? side[1] : top;
}

static void push_level(int row, int k)
{
    float l, r;
    row_levels(row, &l, &r);
    dsp_level_lr(k, l, r);
}

/* what SinkNr s holds - mute parameters, level, mute - to the processing's sink it now drives */
static void push(int s)
{
    int k = dsp_of(s);
    if (!k) return;
    int row = sink_of(s)->row;
    if (row && s_mp_ok[row])
        dsp_mute_params(k, s_mp[row][0] << 8 | s_mp[row][1], s_mp[row][2] << 8 | s_mp[row][3], s_mp[row][4], s_mp[row][5]);
    if (row && s_nlvl[row]) push_level(row, k);
    dsp_mute(k, s_mute[s]);
}

/* stage 4: the processing's sink of SinkNr s onto the ring channels its Connect brought, or off. A stereo sink two
   SinkNrs share (AUD3's main and video) follows the one connected last, back to the other while that stays. False:
   the chip did not take the route */
static bool route(int s, bool on)
{
    const amp_sink_t *k = sink_of(s);
    bool ok = true;
    if (k && k->dsp && (on || s_owner[k->dsp] == s)) {
        int d = k->dsp;
        if (on) s_owner[d] = (uint8_t)s;
        else
            for (int i = 0; i < P->nsinks && !on; i++)
                if (P->sinks[i].dsp == d && P->sinks[i].sink != s && s_sink[P->sinks[i].sink].on) {
                    s = P->sinks[i].sink;
                    s_owner[d] = (uint8_t)s;
                    on = true;
                }
        if (IO && IO->route) ok = IO->route(d, on ? s_sink[s].ch : NULL, on ? s_sink[s].nch : 0);
        if (on && ok) push(s);
        if (d == 1) music_hu_changed();
    }
    bool any = false;
    for (int i = 0; i < P->nsinks; i++) any |= P->sinks[i].dsp && s_sink[P->sinks[i].sink].on;
    if (any != s_any && IO && IO->sound) IO->sound(any);
    s_any = any;
    return ok;
}

/* one message out, in segments when longer than a telegram; false when a telegram was not delivered */
static bool send_msg(uint16_t dst, uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    static most_long_t r;
    static most_msg_t t[(MOST_LONG + 9) / 10];      /* segments to a group carry 10 bytes */
    r = (most_long_t){ .dst = dst, .fblock = fb, .inst = inst, .fkt = fkt, .op = op, .len = (uint8_t)n };
    if (n) memcpy(r.data, d, n);
    int k = most_split(&r, t, sizeof t / sizeof t[0]);
    if (!k) ESP_LOGW(TAG, "%03X: %d bytes do not fit", fkt, n);
    for (int i = 0; i < k; i++) if (!S || !S(&t[i])) return false;
    return k > 0;
}

static bool emit(uint16_t dev, uint16_t fkt, uint8_t op, const uint8_t *d, int n)
{
    return send_msg(dev, FB->fblock, FB->inst, fkt, op, d, n);
}

/* an answer keeps the request's FBlock and InstID; one to "any instance" (00h, FFh) names the real one */
static void reply(const most_long_t *q, uint8_t op, const uint8_t *d, int n)
{
    uint8_t inst = q->fblock == FB->fblock && (q->inst == 0x00 || q->inst == 0xFF) ? FB->inst : q->inst;
    send_msg(q->src, q->fblock, inst, q->fkt, op, d, n);
}

static void status(const most_long_t *q, const uint8_t *d, int n) { reply(q, MOST_OP_STATUS, d, n); }

/* Error [code, ErrorInfo] to the sender, never to a group or broadcast request. ErrorInfo: 04h the OPType, 06h and 07h
   the number of the parameter (1, 2...) and its value */
static void error_info(const most_long_t *q, uint8_t code, const uint8_t *info, int n)
{
    uint8_t d[12] = { code };
    if (n) memcpy(d + 1, info, n);
    ESP_LOGI(TAG, "%02X.%02X %03X %s from %04X: error %02X", q->fblock, q->inst, q->fkt, most_op_name(q->fkt, q->op),
             q->src, code);
    if (q->type == MOST_RX_LOGICAL || q->type == MOST_RX_PHYSICAL) reply(q, MOST_OP_ERROR, d, 1 + n);
}

static void error(const most_long_t *q, uint8_t code) { error_info(q, code, NULL, 0); }
static void error_op(const most_long_t *q) { error_info(q, MOST_ERR_OPTYPE, &q->op, 1); }

static void error_par(const most_long_t *q, uint8_t code, uint8_t nr, uint8_t value)
{
    uint8_t i[2] = { nr, value };
    error_info(q, code, i, 2);
}

static void subscribe_clear(uint16_t dev, uint16_t f);

/* a changed value to every node registered for the function. A node that is gone (no node took the telegram, bXTS
   00h) loses all its entries in this FBlock (MOST 2.5, p. 104) */
static void notify(uint16_t fkt, const uint8_t *d, int n)
{
    uint16_t gone[8];
    int ngone = 0;
    for (int i = 0; i < s_nsub; i++)
        if (s_sub[i].fb == fb_index() && (s_sub[i].fkt == fkt || s_sub[i].fkt == ALL_FKTS) &&
            !emit(s_sub[i].dev, fkt, MOST_OP_STATUS, d, n)) {
            ESP_LOGW(TAG, "notification %03X to %04X not delivered (bXTS %02X)", fkt, s_sub[i].dev, s_xts_last);
            if (s_xts_last == 0x00 && ngone < (int)(sizeof gone / sizeof gone[0])) gone[ngone++] = s_sub[i].dev;
        }
    for (int i = 0; i < ngone; i++) subscribe_clear(gone[i], ALL_FKTS);
}

/* one entry per FBlock, node and function: "all" replaces the node's single entries and covers later ones */
static bool subscribe_add(uint16_t dev, uint16_t f)
{
    uint8_t fb = fb_index();
    if (f == ALL_FKTS) subscribe_clear(dev, ALL_FKTS);
    for (int i = 0; i < s_nsub; i++)
        if (s_sub[i].fb == fb && s_sub[i].dev == dev && (s_sub[i].fkt == f || s_sub[i].fkt == ALL_FKTS)) return true;
    if (s_nsub >= (int)(sizeof s_sub / sizeof s_sub[0])) return false;
    s_sub[s_nsub++] = (sub_t){ fb, dev, f };
    return true;
}

static void subscribe_clear(uint16_t dev, uint16_t f)
{
    for (int i = 0; i < s_nsub;)
        if (s_sub[i].fb == fb_index() && s_sub[i].dev == dev && (f == ALL_FKTS || s_sub[i].fkt == f))
            s_sub[i] = s_sub[--s_nsub];
        else i++;
}

/* ClearFunction: a node registered with SetAll is deleted for these functions only, so its "all" entry turns into
   one entry per remaining property (MOST 2.5, p. 103) */
static void subscribe_clear_fkts(uint16_t dev, const uint16_t *f, int n)
{
    bool all = false;
    for (int i = 0; i < s_nsub; i++) all |= s_sub[i].fb == fb_index() && s_sub[i].dev == dev && s_sub[i].fkt == ALL_FKTS;
    if (!all) {
        for (int j = 0; j < n; j++) subscribe_clear(dev, f[j]);
        return;
    }
    subscribe_clear(dev, ALL_FKTS);
    for (int i = 0; i < FB->nfkts; i++) {
        bool cleared = false;
        for (int j = 0; j < n; j++) cleared |= f[j] == FB->fkts[i];
        if (is_prop(FB->fkts[i]) && !cleared) subscribe_add(dev, FB->fkts[i]);
    }
}

static raw_t *raw(uint16_t f, uint16_t key, bool make)
{
    uint8_t fb = fb_index();
    for (int i = 0; i < s_nraw; i++)
        if (s_raw[i].fb == fb && s_raw[i].fkt == f && s_raw[i].key == key) return &s_raw[i];
    if (!make || s_nraw >= (int)(sizeof s_raw / sizeof s_raw[0])) return NULL;
    raw_t *p = &s_raw[s_nraw++];
    memset(p, 0, sizeof *p);
    p->fb = fb;
    p->fkt = f;
    p->key = key;
    return p;
}

/* one of JLR's own arrays by its FktID: the bytes and the length; NULL none */
static uint8_t *e0x_table(uint16_t f, int *n)
{
    switch (f) {
    case F_E04: *n = sizeof s_e04; return s_e04;
    case F_E05: *n = sizeof s_e05; return s_e05;
    case F_E07: *n = sizeof s_e07; return s_e07;
    case F_E08: *n = sizeof s_e08; return s_e08;
    default: return NULL;
    }
}

/* AUD3's D00h Status: the mode, then six fixed bytes whose meaning is not known (8038878) */
static void d00_status(uint16_t dev, uint8_t mode)
{
    const uint8_t d[7] = { mode, 0x00, 0x02, 0x00, 0x06, 0x00, 0x01 };
    send_msg(dev, FB->fblock, FB->inst, F_D00, MOST_OP_STATUS, d, sizeof d);
}

/* the value of a function to a node that has just registered for it: Status when there is one (2.3.12). No value, or a
   function not here: nothing. That is what usb2most does, and the head units' start-up registers for functions no
   amplifier list has (0C01h, 0353h on AUD3, decode_aud3.md sect. 7) */
static void report(uint16_t dev, uint16_t f)
{
    uint8_t d[2 + NSPK];
    int nt;
    const uint8_t *t;
    bool main = FB == P->fb;            /* the sinks, the tone and the levels are the main AudioAmplifier's */
    if (main && f == F_D00) {
        d00_status(dev, 1);
    } else if (main && f == F_MUTE) {
        for (int i = 0; i < P->nsinks; i++) {
            int s = P->sinks[i].sink;
            d[0] = (uint8_t)s;
            d[1] = s_mute[s];
            emit(dev, f, MOST_OP_STATUS, d, 2);
        }
    } else if (main && (f == F_BASS || f == F_TREBLE)) {
        d[0] = (uint8_t)s_tone[f - F_BASS];
        emit(dev, f, MOST_OP_STATUS, d, 1);
    } else if (main && f == F_MUTEPAR) {
        for (int r = 1; r <= NROW; r++)
            if (s_mp_ok[r]) {
                d[0] = (uint8_t)r;
                memcpy(d + 1, s_mp[r], 6);
                emit(dev, f, MOST_OP_STATUS, d, 7);
            }
    } else if (main && f == F_LEVEL) {
        for (int r = 1; r <= NROW; r++)
            if (s_nlvl[r]) {
                d[POS_ROW] = (uint8_t)r;
                d[POS_SPK] = 0;
                memcpy(d + 2, s_lvl[r], P->nlvl);
                emit(dev, f, MOST_OP_STATUS, d, 2 + P->nlvl);
            }
    } else if (main && (t = e0x_table(f, &nt))) {
        emit(dev, f, MOST_OP_STATUS, t, nt);
    } else {
        for (int i = 0; i < s_nraw; i++)
            if (s_raw[i].fb == fb_index() && s_raw[i].fkt == f) emit(dev, f, MOST_OP_STATUS, s_raw[i].data, s_raw[i].len);
    }
}

/* Set [control, DeviceID, FktIDs as 16-bit words]: 0 all functions, 1 these (at most 4), 2 clear all, 3 clear these.
   A registered node gets the values at once, again on a repeated registration; what cannot be registered is passed
   over in silence (see report). Get [FktID] -> Status [FktID, DeviceIDs] */
static void on_notification(const most_long_t *m)
{
    const uint8_t *d = m->data;
    if (m->op == MOST_OP_GET) {
        if (m->len != 2) { error(m, MOST_ERR_LENGTH); return; }
        uint16_t f = (uint16_t)(d[0] << 8 | d[1]);
        if (!has_fkt(f) || !is_prop(f)) {
            uint8_t i[3] = { 0x01, d[0], d[1] };
            error_info(m, MOST_ERR_NOTAVAIL, i, 3);
            return;
        }
        uint8_t st[2 + 2 * 48] = { d[0], d[1] };
        int n = 2;
        for (int i = 0; i < s_nsub; i++)
            if (s_sub[i].fb == fb_index() && (s_sub[i].fkt == f || s_sub[i].fkt == ALL_FKTS)) {
                st[n++] = s_sub[i].dev >> 8;
                st[n++] = s_sub[i].dev & 0xFF;
            }
        status(m, st, n);
        return;
    }
    if (m->op != MOST_OP_SET) { error_op(m); return; }
    if (m->len < 3 || m->len > 11 || (m->len - 3) % 2) { error(m, MOST_ERR_LENGTH); return; }
    uint8_t ctl = d[0];
    uint16_t dev = (uint16_t)(d[1] << 8 | d[2]);
    switch (ctl) {
    case 0: {
        const uint8_t full = 0x21;          /* no more registration possible (MOST 2.5, p. 104) */
        if (!subscribe_add(dev, ALL_FKTS)) { error_info(m, MOST_ERR_FUNC, &full, 1); return; }
        ESP_LOGI(TAG, "notification: %04X for all", dev);
        for (int i = 0; i < FB->nfkts; i++) if (is_prop(FB->fkts[i])) report(dev, FB->fkts[i]);
        return;
    }
    case 1:
        for (int i = 3; i < m->len; i += 2) {
            uint16_t f = (uint16_t)(d[i] << 8 | d[i + 1]);
            /* AUD3's Diagnosis answers this one, an empty Status to the sender, though its list does not have it
               (8037224); the pre-facelift's says nothing */
            if (FB->fblock == DIAG_FBLOCK && f == F_DIAG_NOTE && main_has(F_BALANCE)) {
                send_msg(m->src, DIAG_FBLOCK, m->inst, F_DIAG_NOTE, MOST_OP_STATUS, NULL, 0);
                continue;
            }
            if (!has_fkt(f) || !is_prop(f) || !subscribe_add(dev, f)) {
                ESP_LOGI(TAG, "notification: %04X for %03X: not here", dev, f);
                continue;
            }
            ESP_LOGI(TAG, "notification: %04X for %03X", dev, f);
            report(dev, f);
        }
        return;
    case 2:
        subscribe_clear(dev, ALL_FKTS);
        ESP_LOGI(TAG, "notification: %04X cleared", dev);
        return;
    case 3: {
        uint16_t f[4];
        int n = 0;
        for (int i = 3; i < m->len && n < 4; i += 2) f[n++] = (uint16_t)(d[i] << 8 | d[i + 1]);
        subscribe_clear_fkts(dev, f, n);
        return;
    }
    default:
        error_par(m, MOST_ERR_PARAM, 1, ctl);
    }
}

/* 002h NotificationCheck: Get [DeviceID] -> Status [DeviceID, the functions it is registered for here] (GFB25 2.1.3),
   so a node can see whether its entries survived */
static void on_notify_check(const most_long_t *m)
{
    if (m->op != MOST_OP_GET) { error_op(m); return; }
    if (m->len != 2) { error(m, MOST_ERR_LENGTH); return; }
    uint16_t dev = (uint16_t)(m->data[0] << 8 | m->data[1]);
    uint8_t st[2 + 2 * 64] = { m->data[0], m->data[1] };
    int n = 2;
    for (int i = 0; i < s_nsub && n + 2 <= (int)sizeof st; i++) {
        if (s_sub[i].fb != fb_index() || s_sub[i].dev != dev) continue;
        if (s_sub[i].fkt != ALL_FKTS) {
            st[n++] = s_sub[i].fkt >> 8;
            st[n++] = s_sub[i].fkt & 0xFF;
            continue;
        }
        for (int k = 0; k < FB->nfkts && n + 2 <= (int)sizeof st; k++)
            if (is_prop(FB->fkts[k])) {
                st[n++] = FB->fkts[k] >> 8;
                st[n++] = FB->fkts[k] & 0xFF;
            }
    }
    status(m, st, n);
}

/* Connect StartResult [SinkNr, SrcDelay, ChannelList] -> Result [SinkNr]; DisConnect [SinkNr] alike (GFB25,
   stock_amps.md 3, 4). A channel list is each 0..59, in the order of the sample's bytes and not always ascending:
   usb2most gives a pre-facelift amplifier [c+1, c, c+5, c+4] for video (8037A2C). All FFh means none. A SinkNr with
   nothing behind it still gets its Result, as from usb2most. Stage 4: the channels go to the sound processing (route()) */
static void on_connect(const most_long_t *m, bool connect)
{
    const uint8_t *d = m->data;
    if (m->op != MOST_OP_START && m->op != MOST_OP_STARTRESULT) { error_op(m); return; }
    if (connect ? m->len < 3 : m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
    uint8_t sink = d[0];
    if (!sink_of(sink)) {
        ESP_LOGI(TAG, "%s sink %u: not here", connect ? "connect" : "disconnect", sink);
        if (m->op == MOST_OP_STARTRESULT) reply(m, MOST_OP_RESULT, &sink, 1);
        return;
    }
    sink_t *s = &s_sink[sink];
    if (connect) {
        int n = m->len - 2;
        bool none = true;
        for (int i = 0; i < n; i++) none &= d[2 + i] == 0xFF;
        for (int i = 0; !none && i < n; i++)
            if (d[2 + i] >= MAX_CH) { error_par(m, MOST_ERR_PARAM, 3, d[2 + i]); return; }
        s->on = !none;
        s->delay = d[1];
        s->nch = none ? 0 : (uint8_t)n;
        memcpy(s->ch, d + 2, s->nch);
        char chs[3 * MAX_CH + 1] = "";
        for (int i = 0; i < s->nch; i++) snprintf(chs + 3 * i, 4, " %02X", s->ch[i]);
        ESP_LOGI(TAG, "connect sink %u, source delay %u, channels%s", sink, s->delay, s->nch ? chs : " none");
    } else {
        s->on = false;
        s->nch = 0;
        ESP_LOGI(TAG, "disconnect sink %u", sink);
    }
    if (!route(sink, s->on) && connect) {
        /* the chip did not take the route (I2C, tried twice): the sink stays off, so the next Connect routes it
           afresh, and the head unit hears that the sink is not available - not a Result for one that plays nothing */
        s->on = false;
        s->nch = 0;
        route(sink, false);
        ESP_LOGE(TAG, "connect sink %u: the route did not take", sink);
        error(m, MOST_ERR_UNAVAIL);
        return;
    }
    if (m->op == MOST_OP_STARTRESULT) reply(m, MOST_OP_RESULT, &sink, 1);  /* the sink is ready when the head unit
                                                                                gets the Result */
}

/* Allocate StartResult [SourceNr] -> Result [SourceNr, SrcDelay, ChannelList], DeAllocate [SourceNr] -> Result
   [SourceNr] (GFB25): AUD3 is a source too. usb2most takes 2 channels from the timing master for it and routes
   nothing into them (main-v24.bin 80385DA, 80246FC), and so does this: they pass the ring through, so channels that
   are no longer ours can never be overwritten. A repeated Allocate gets the same channels while the ring's allocation
   table still has them for us; after the master started afresh, new ones. Any SourceNr is taken, as usb2most does.
   No channels: Error [20h, SourceNr, the channels wanted] - an allocation is never partial (MOST 2.5, p. 184) */
#define SRC_CH 2

static void on_alloc(const most_long_t *m, bool alloc)
{
    if (m->op != MOST_OP_START && m->op != MOST_OP_STARTRESULT) { error_op(m); return; }
    if (m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
    uint8_t src = m->data[0];
    if (alloc) {
        if (s_src_n && IO && IO->alloc_ok && !IO->alloc_ok(s_src_ch, s_src_n)) {
            ESP_LOGI(TAG, "source: channels %02X %02X are not ours any more, allocating afresh", s_src_ch[0],
                     s_src_ch[1]);
            s_src_n = 0;
        }
        if (!s_src_n && IO && IO->alloc) {
            int n = IO->alloc(SRC_CH, s_src_ch, &s_src_delay);
            if (n != SRC_CH && n && IO->dealloc) IO->dealloc(s_src_ch, n);
            s_src_n = n == SRC_CH ? SRC_CH : 0;
        }
        if (!s_src_n) {
            const uint8_t i[2] = { src, SRC_CH };
            error_info(m, MOST_ERR_FUNC, i, 2);
            return;
        }
        uint8_t r[2 + SRC_CH] = { src, s_src_delay };
        memcpy(r + 2, s_src_ch, SRC_CH);
        ESP_LOGI(TAG, "allocate source %u: channels %02X %02X", src, s_src_ch[0], s_src_ch[1]);
        if (m->op == MOST_OP_STARTRESULT) reply(m, MOST_OP_RESULT, r, sizeof r);
    } else {
        /* channels the master did not take back stay ours: the next Allocate finds them in the allocation table */
        if (s_src_n && IO && IO->dealloc && !IO->dealloc(s_src_ch, s_src_n))
            ESP_LOGW(TAG, "source: channels %02X %02X not given back, kept for the next Allocate", s_src_ch[0],
                     s_src_ch[1]);
        else s_src_n = 0;
        ESP_LOGI(TAG, "de-allocate source %u", src);
        if (m->op == MOST_OP_STARTRESULT) reply(m, MOST_OP_RESULT, &src, 1);
    }
}

/* 113h Mute: Get [SinkNr], SetGet [SinkNr, Status], Status [SinkNr, Status]; SinkNr 0 means all sinks, bit 0 of Status
   mutes (GFB25). Set is taken too: the intent is plain */
static void on_mute(const most_long_t *m)
{
    const uint8_t *d = m->data;
    if (m->op == MOST_OP_GET) {
        if (m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
        if (d[0] && !sink_of(d[0])) { error_par(m, MOST_ERR_PARAM, 1, d[0]); return; }
        bool all = true;
        for (int i = 0; i < P->nsinks; i++) all &= s_mute[P->sinks[i].sink];
        uint8_t st[2] = { d[0], d[0] ? s_mute[d[0]] : all };
        status(m, st, 2);
        return;
    }
    if (m->op != MOST_OP_SET && m->op != MOST_OP_SETGET) { error_op(m); return; }
    if (m->len != 2) { error(m, MOST_ERR_LENGTH); return; }
    if (d[0] && !sink_of(d[0])) { error_par(m, MOST_ERR_PARAM, 1, d[0]); return; }
    bool on = d[1] & 1;
    uint8_t st[2] = { d[0], on };
    if (m->op == MOST_OP_SETGET) status(m, st, 2);
    /* a notification per sink that changed, SinkNr 0 included (GFB25 2.1.29) */
    for (int i = 0; i < P->nsinks; i++) {
        int s = P->sinks[i].sink, k = dsp_of(s);
        if ((!d[0] || d[0] == s) && s_mute[s] != on) {
            s_mute[s] = on;
            if (k) dsp_mute(k, on);
            if (k == 1) music_hu_changed();
            uint8_t ns[2] = { (uint8_t)s, on };
            notify(F_MUTE, ns, 2);
        }
    }
}

/* 466h MuteParameters: Set [row, MuteTimeIn, MuteTimeOut (ms, 16 bits), MuteShapeIn, MuteShapeOut], Get [row], Status
   as Set (AudioAmplifier 2.1.30). usb2most stores rows 1..4 and passes over the rest in silence (8033AAC, 80376FC), and
   so does this; row 0 is taken as all rows, as in Mute; SetGet as Set + Get */
static void on_mute_params(const most_long_t *m)
{
    const uint8_t *d = m->data;
    if (m->op == MOST_OP_GET) {
        if (m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
        if (!d[0] || d[0] > NROW) { error_par(m, MOST_ERR_PARAM, 1, d[0]); return; }
        if (!s_mp_ok[d[0]]) { error_par(m, MOST_ERR_NOTAVAIL, 1, d[0]); return; }
        uint8_t st[7] = { d[0] };
        memcpy(st + 1, s_mp[d[0]], 6);
        status(m, st, 7);
        return;
    }
    if (m->op != MOST_OP_SET && m->op != MOST_OP_SETGET) { error_op(m); return; }
    if (m->len != 7 || d[0] > NROW) return;
    bool changed = false;
    for (int r = 1; r <= NROW; r++) {
        if (d[0] && d[0] != r) continue;
        changed |= !s_mp_ok[r] || memcmp(s_mp[r], d + 1, 6);
        memcpy(s_mp[r], d + 1, 6);
        s_mp_ok[r] = true;
        for (int i = 0; i < P->nsinks; i++) {
            int k = dsp_of(P->sinks[i].sink);
            if (P->sinks[i].row == r && k) dsp_mute_params(k, d[1] << 8 | d[2], d[3] << 8 | d[4], d[5], d[6]);
        }
    }
    if (m->op == MOST_OP_SETGET) status(m, d, 7);
    if (changed) notify(F_MUTEPAR, d, 7);
}

/* levels from speaker index first on into a row, and on to the sound */
static bool row_store(int row, int first, const uint8_t *v, int n)
{
    bool changed = false;
    for (int j = 0; j < n && first + j < NSPK; j++) {
        changed |= s_lvl[row][first + j] != (int8_t)v[j];
        s_lvl[row][first + j] = (int8_t)v[j];
        s_lset[row] |= (uint8_t)(1u << (first + j));
        if (s_nlvl[row] < first + j + 1) {
            s_nlvl[row] = (uint8_t)(first + j + 1);
            changed = true;
        }
    }
    for (int i = 0; i < P->nsinks; i++) {
        int k = dsp_of(P->sinks[i].sink);
        if (P->sinks[i].row == row && k) push_level(row, k);
    }
    return changed;
}

/* 46Dh SpeakerLevel, as usb2most takes it (8034140, 8037D74, 8033DE0, 8037768) and sends it (80330E8, 8036C48, 8037150),
   and as the catalogue's Pos {x, y} (AudioAmplifier 2.1.37): [row, speaker, dB...], signed, 0 dB full.
   - [r, 0, levels]: row r, 5 levels on the pre-facelift, 7 on AUD3.
   - [0, 0, levels]: all rows one after the other, 4 x 5 or 4 x 7.
   - [r, s, levels], s not 0: the catalogue's run from speaker s on. usb2most stores nothing for these: no head unit it
     meets sends one, or it would not play.
   - Get [r, 0]: Status [00 00, the row], Get [0, 0]: [00 00, all rows]; anything else an empty Status; a length other
     than 2 Error [06]. The row's number is not echoed, in both of usb2most's.
   A level out of the catalogue's range is taken as it comes: see row_levels */
static void on_level(const most_long_t *m)
{
    const uint8_t *d = m->data;
    if (m->op == MOST_OP_GET) {
        if (m->len != 2) { error(m, MOST_ERR_PARAM); return; }
        uint8_t st[2 + NROW * NSPK] = { 0, 0 };
        int n = 2, row = d[POS_ROW];
        if (d[POS_SPK] || row > NROW) { status(m, NULL, 0); return; }
        for (int r = row ? row : 1; r <= (row ? row : NROW); r++)
            for (int k = 0; k < P->nlvl; k++) st[n++] = (uint8_t)s_lvl[r][k];
        status(m, st, n);
        return;
    }
    if (m->op != MOST_OP_SET && m->op != MOST_OP_SETGET) { error_op(m); return; }
    if (m->len < 2) return;
    int row = d[POS_ROW], spk = d[POS_SPK], n = m->len - 2;
    bool changed = false;
    if (row > NROW) return;
    if (!row && !spk) {
        for (int r = 1; r <= NROW && (r - 1) * P->nlvl < n; r++) {
            int left = n - (r - 1) * P->nlvl;
            changed |= row_store(r, 0, d + 2 + (r - 1) * P->nlvl, left < P->nlvl ? left : P->nlvl);
        }
    } else if (row) {
        changed = row_store(row, spk ? spk - 1 : 0, d + 2, n);
    } else {
        return;                         /* every row's speaker s: nobody's message */
    }
    if (m->op == MOST_OP_SETGET) status(m, d, m->len);
    if (changed) notify(F_LEVEL, d, m->len);
}

/* JLR's own arrays E04h, E05h, E07h, E08h, as usb2most keeps them (8034110, 8035074, 80340E0, 8034008; 8034C4C, 8034B98,
   8034AE4, 8034A30; AUD3 8038994, 8037C3C, 8037FE0, 8037F2C). What their fields mean is not known.
   Set: E04h, E05h, E07h the whole array with its two first bytes, anything else passed over. E08h [00 00, 14 bytes] or
   [1 or 2, 00, 7 bytes] into its half, anything else Error [06]. On AUD3 E05h [x, x, mode] gets a D00h Status back, the
   mode with 5 and 7 as 4, 0Dh as 2 (8038994).
   Get [00 00]: Status with the array, its first two bytes zeroed; another Pos an empty Status; another length Error [06].
   E08h's table holds no Pos, so this loses the first two bytes of what was set: as in usb2most */
static void on_e0x(const most_long_t *m)
{
    const uint8_t *d = m->data;
    int n;
    uint8_t *t = e0x_table(m->fkt, &n);
    if (m->op == MOST_OP_GET) {
        if (m->len != 2) { error(m, MOST_ERR_PARAM); return; }
        if (d[0] || d[1]) { status(m, NULL, 0); return; }
        t[0] = t[1] = 0;
        status(m, t, n);
        return;
    }
    if (m->op != MOST_OP_SET && m->op != MOST_OP_SETGET) { error_op(m); return; }
    uint8_t before[sizeof s_e08];
    memcpy(before, t, n);
    if (m->fkt == F_E08) {
        if (m->len == 16 && !d[0] && !d[1]) memcpy(t, d + 2, 14);
        else if (m->len == 9 && (d[0] == 1 || d[0] == 2) && !d[1]) memcpy(t + 7 * (d[0] - 1), d + 2, 7);
        else { error(m, MOST_ERR_PARAM); return; }
    } else if (m->fkt == F_E05 && m->len == 3 && main_has(F_BALANCE)) {
        uint8_t mode = d[2];
        d00_status(m->src, mode == 5 || mode == 7 ? 4 : mode == 0x0D ? 2 : mode);
        return;
    } else if (m->len == n) {
        memcpy(t, d, n);
    } else {
        return;
    }
    if (m->op == MOST_OP_SETGET) status(m, t, n);
    if (memcmp(before, t, n)) notify(m->fkt, t, n);
}

/* 202h Bass, 203h Treble: one signed byte. Increment and Decrement by NSteps report like SetGet and stop at the end of
   the range; Set takes any value until the car log shows the amplifier's range */
static void on_tone(const most_long_t *m)
{
    int8_t *v = &s_tone[m->fkt - F_BASS], old = *v;
    switch (m->op) {
    case MOST_OP_GET:
        if (m->len) { error(m, MOST_ERR_LENGTH); return; }
        break;
    case MOST_OP_SET:
    case MOST_OP_SETGET:
        if (m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
        *v = (int8_t)m->data[0];
        break;
    case MOST_OP_INC:
    case MOST_OP_DEC: {
        if (m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
        if (!m->data[0]) { error_par(m, MOST_ERR_PARAM, 1, 0); return; }
        int t = *v + (m->op == MOST_OP_INC ? m->data[0] : -m->data[0]);
        if (m->op == MOST_OP_INC && t > TONE_MAX) t = old > TONE_MAX ? old : TONE_MAX;     /* a Set beyond stays */
        if (m->op == MOST_OP_DEC && t < -TONE_MAX) t = old < -TONE_MAX ? old : -TONE_MAX;
        *v = (int8_t)t;
        break;
    }
    default:
        error_op(m);
        return;
    }
    uint8_t b = (uint8_t)*v;
    if (m->op != MOST_OP_SET) status(m, &b, 1);
    if (*v != old) notify(m->fkt, &b, 1);
    dsp_tone(s_tone[0], s_tone[1]);
}

/* the other arrays (427h, 430h, 431h, 441h, 461h) are kept per Pos, their first two bytes [row, parameter], as they came:
   nothing here plays them, the sound has no crossover, delay or equaliser of the amplifier's. usb2most models them with
   starting values for its Android application; no answer of a head unit depends on that (check_amp.md) */
static bool is_array(uint16_t f) { return f == 0x427 || f == 0x430 || f == 0x431 || f == 0x441 || f == 0x461; }

static void on_raw(const most_long_t *m)
{
    bool arr = is_array(m->fkt);
    if (arr && m->len < 2) { error(m, MOST_ERR_LENGTH); return; }
    uint16_t key = arr ? (uint16_t)(m->data[0] << 8 | m->data[1]) : 0;
    switch (m->op) {
    case MOST_OP_SET:
    case MOST_OP_SETGET: {
        raw_t *p = raw(m->fkt, key, true);
        if (!p) {
            ESP_LOGW(TAG, "%03X: no room for one more value", m->fkt);
            error(m, MOST_ERR_UNAVAIL);
            return;
        }
        bool changed = p->len != m->len || memcmp(p->data, m->data, m->len);
        p->len = m->len;
        memcpy(p->data, m->data, m->len);
        if (m->op == MOST_OP_SETGET) status(m, p->data, p->len);
        if (changed) notify(m->fkt, p->data, p->len);
        /* Balance is kept as it came and goes to the sound too; Fader (204h) is kept only: every output is stereo */
        if (FB == P->fb && m->fkt == F_BALANCE && m->len >= 1) dsp_balance((int8_t)p->data[0]);
        return;
    }
    case MOST_OP_GET: {
        raw_t *p = raw(m->fkt, key, false);
        if (!p) { error_par(m, MOST_ERR_NOTAVAIL, 1, arr ? m->data[0] : 0); return; }
        status(m, p->data, p->len);
        return;
    }
    default:
        error_op(m);
    }
}

/* what the amplifier asks the network master at Configuration OK, the master being whoever sent that: on XF MY08-11 the
   ICM 161h, on later cars the FCDIM (SDD). A message each, in this order (8038720-80387C6). What comes back is only
   logged, as in usb2most, whose own master answers the two registrations with an error and the amplifier plays on */
static void subscribe_gateway(uint16_t master)
{
    bool ok = true;
    ESP_LOGI(TAG, "subscribing to %04X %02X.%02X", master, P->gw_fblock, P->gw_inst);
    if (P->gw_get) ok &= send_msg(master, P->gw_fblock, P->gw_inst, 0x305, MOST_OP_GET, P->gw_get, P->ngw_get);
    for (int i = 0; i < P->nsubs; i++) {
        const uint8_t d[5] = { 1, P->node >> 8, P->node & 0xFF, P->subs[i] >> 8, P->subs[i] & 0xFF };
        ok &= send_msg(master, P->gw_fblock, P->gw_inst, F_NOTIFY, MOST_OP_SET, d, sizeof d);
    }
    s_subscribed = ok;                  /* else at the next OK */
    s_master = master;
}

/* 0..8 ask something (Set .. StartAck); 9..F report (ErrorAck .. Error) and never get an answer, not even an error */
static bool is_request(uint8_t op) { return op <= 0x8; }

/* Diagnosis D22h: SetGet [DID] -> Status [DID, the value's field zero-padded], as usb2most answers the part numbers
   and the serial number (803485C, 80381D0). Another length or a first byte not F1h: no answer; F1h with another second
   byte: Error [06 01 F1 xx] */
static void on_did(const most_long_t *m)
{
    if (m->op != MOST_OP_SETGET && m->op != MOST_OP_GET) { error_op(m); return; }
    if (m->len != 2 || m->data[0] != 0xF1) return;
    uint16_t did = (uint16_t)(m->data[0] << 8 | m->data[1]);
    for (int i = 0; i < P->ndids; i++)
        if (P->dids[i].did == did) {
            const amp_did_t *v = &P->dids[i];
            uint8_t d[2 + 32] = { m->data[0], m->data[1] };
            int n = v->len < 32 ? v->len : 32, k = v->vlen < n ? v->vlen : n;
            memcpy(d + 2, v->value, k);
            ESP_LOGI(TAG, "Diagnosis DID %04X to %04X: %.*s", did, m->src, k, (const char *)v->value);
            status(m, d, 2 + n);
            return;
        }
    const uint8_t i[3] = { 0x01, 0xF1, m->data[1] };
    error_info(m, MOST_ERR_PARAM, i, 3);
}

/* Diagnosis D19h: fixed requests, fixed answers, as usb2most has them from a real amplifier (8033260, 8036DC8; the two
   drivers hold the same bytes): the DTC service, what the bytes mean is not known. Another request: no answer */
static const uint8_t DTC_Q0[] = { 0x02, 0xAF }, DTC_Q1[] = { 0x04, 0x9D, 0x84, 0x13 }, DTC_Q2[] = { 0x06, 0x9D, 0x84, 0x13 },
                     DTC_Q3[] = { 0x04, 0xF0, 0x00, 0x87 }, DTC_Q4[] = { 0x06, 0xF0, 0x00, 0x87 };
static const uint8_t DTC_R0[] = { 0x02, 0x7F, 0x9D, 0x84, 0x13, 0x69, 0xF0, 0x00, 0x87, 0x28, 0x9A, 0x03, 0x13, 0x28, 0x9A,
                                  0x04, 0x13, 0x28, 0x9A, 0x05, 0x13, 0x2D, 0x9A, 0x06, 0x13, 0x2D, 0x9A, 0x10, 0x13, 0x2D,
                                  0x9A, 0x11, 0x13, 0x2D, 0x9A, 0x03, 0x11, 0x68 };
static const uint8_t DTC_R1[] = { 0x04, 0x9D, 0x84, 0x13, 0x69, 0x10, 0x05, 0xDD, 0x00, 0x5E, 0x3C, 0x4F, 0xDD, 0xDD, 0x01,
                                  0x00, 0x02, 0x7A, 0xBD, 0xDD, 0x02, 0x38, 0xDD, 0x04, 0x3C, 0xDD, 0x06, 0xC7 };
static const uint8_t DTC_R2[] = { 0x06, 0x9D, 0x84, 0x13, 0x2C, 0x01, 0xDD, 0x02, 0x81, 0x03, 0xFF, 0x04, 0x8E, 0x05, 0x00,
                                  0x10, 0x80, 0x11, 0x7F, 0x12, 0x00 };
static const uint8_t DTC_R3[] = { 0x04, 0xF0, 0x00, 0x87, 0x28, 0x10, 0x05, 0xDD, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xDD, 0x01,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xDD, 0x02, 0xFF, 0xDD, 0x04, 0xFF, 0xDD, 0x06, 0xFF };
static const uint8_t DTC_R4[] = { 0x06, 0xF0, 0x00, 0x87, 0x28, 0x01, 0xFE, 0x02, 0xFD, 0x03, 0xFF, 0x04, 0x80, 0x05, 0x00,
                                  0x10, 0x80, 0x11, 0x7F, 0x12, 0x00 };

static void on_dtc(const most_long_t *m)
{
    static const struct { const uint8_t *q; uint8_t nq; const uint8_t *r; uint8_t nr; } a[] = {
        { DTC_Q0, sizeof DTC_Q0, DTC_R0, sizeof DTC_R0 }, { DTC_Q1, sizeof DTC_Q1, DTC_R1, sizeof DTC_R1 },
        { DTC_Q2, sizeof DTC_Q2, DTC_R2, sizeof DTC_R2 }, { DTC_Q3, sizeof DTC_Q3, DTC_R3, sizeof DTC_R3 },
        { DTC_Q4, sizeof DTC_Q4, DTC_R4, sizeof DTC_R4 },
    };
    if (m->op != MOST_OP_SETGET) { error_op(m); return; }
    for (int i = 0; i < N(a); i++)      /* len 2 the first, len 5 the others, whose fifth byte is not looked at */
        if (m->len == (a[i].nq == 2 ? 2 : 5) && !memcmp(m->data, a[i].q, a[i].nq)) {
            status(m, a[i].r, a[i].nr);
            return;
        }
    ESP_LOGI(TAG, "Diagnosis D19 from %04X: not a request known", m->src);
}

/* Connect and DisConnect on an FBlock with no output of ours (the HeadphoneAmplifier): answered, nothing routed */
static void on_connect_other(const most_long_t *m, bool connect)
{
    if (m->op != MOST_OP_START && m->op != MOST_OP_STARTRESULT) { error_op(m); return; }
    if (connect ? m->len < 3 : m->len != 1) { error(m, MOST_ERR_LENGTH); return; }
    ESP_LOGI(TAG, "%02X.%02X %s sink %u: no output for it", FB->fblock, FB->inst, connect ? "connect" : "disconnect",
             m->data[0]);
    if (m->op == MOST_OP_STARTRESULT) reply(m, MOST_OP_RESULT, m->data, 1);
}

static void on_fktids(const most_long_t *m)
{
    static uint8_t d[96];
    if (m->op != MOST_OP_GET) { error_op(m); return; }
    if (FB->raw) status(m, FB->raw, FB->nraw);
    else status(m, d, most_fktids(FB->fkts, FB->nfkts, d, sizeof d));
}

/* the other FBlocks: their functions as stored values; of the Diagnosis FBlock the DIDs and the DTC service, the rest
   of its list no answer, as in usb2most */
static void on_other(const most_long_t *m)
{
    switch (m->fkt) {
    case F_FKTIDS:    on_fktids(m); return;
    case F_NOTIFY:    on_notification(m); return;
    case F_NOTIFYCHK: on_notify_check(m); return;
    case F_CONNECT:   on_connect_other(m, true); return;
    case F_DISCONN:   on_connect_other(m, false); return;
    }
    if (FB->fblock != DIAG_FBLOCK) { on_raw(m); return; }
    if (m->fkt == F_DID) { on_did(m); return; }
    if (m->fkt == F_DTC) { on_dtc(m); return; }
    ESP_LOGI(TAG, "Diagnosis %03X %s from %04X: no answer", m->fkt, most_op_name(m->fkt, m->op), m->src);
}

/* NetBlock, as usb2most answers it (decode_prefacelift.md sect. 6, decode_aud3.md sect. 6; the two read the same table
   80458C4). The master asks FBlockIDs; the rest is here for what else may ask. ShutDown Query: its Control back; Execute
   is power.c's and gets no answer. 000h SetGet renames a duplicate InstID: none can happen here, this is the only
   amplifier in the ring, so it gets no answer. 401h, a registration, no answer either */
static void on_netblock(const most_long_t *m)
{
    uint8_t d[16];
    int n = 0;
    switch (m->fkt) {
    case 0x000:
        if (m->op != MOST_OP_GET) return;
        for (int i = 0; i < P->nfb && n + 2 <= (int)sizeof d; i++) {
            d[n++] = P->fb[i].fblock;
            d[n++] = P->fb[i].inst;
        }
        ESP_LOGI(TAG, "NetBlock FBlockIDs to %04X: %d FBlocks", m->src, P->nfb);
        status(m, d, n);
        return;
    case 0x001:                         /* DeviceInfo */
        if (m->op != MOST_OP_GET) return;
        d[0] = 0xFF, d[1] = 0xFF, d[2] = 0x00;
        status(m, d, 3);
        return;
    case 0x002:                         /* NodePositionAddress */
        if (m->op != MOST_OP_GET) return;
        d[0] = 0x40, d[1] = IO && IO->position ? (uint8_t)IO->position() : 0;
        status(m, d, 2);
        return;
    case 0x003:                         /* NodeAddress */
        if (m->op != MOST_OP_GET) return;
        d[0] = P->node >> 8, d[1] = P->node & 0xFF;
        status(m, d, 2);
        return;
    case 0x004:                         /* GroupAddress */
        if (m->op != MOST_OP_GET) return;
        d[0] = 0x03, d[1] = P->group;
        status(m, d, 2);
        return;
    case 0x006:                         /* ShutDown */
        if (m->op == MOST_OP_START && m->len == 1 && m->data[0] != 0x02) reply(m, MOST_OP_RESULT, m->data, 1);
        return;
    default:
        return;
    }
}

/* functions usb2most takes though they are not in the amplifier's list: E09h on the main FBlock of the pre-facelift,
   its other E0xh on Hi-Line, E53h on AUD3. A head unit that sends them meets no error, and only E0xh is answered */
static bool also_taken(uint16_t f)
{
    return main_has(F_BALANCE) ? f == 0xE53 : f == 0xE09 || f == F_E04 || f == F_E05 || f == F_E07;
}

static void on_message(const most_long_t *m)
{
    if (m->fblock == NM_FBLOCK) {
        if (m->fkt == NM_CONFIG && m->op == MOST_OP_STATUS && m->len) {
            if (m->data[0] == 0x01 && !s_subscribed) subscribe_gateway(m->src);
            else if (m->data[0] == 0x00) network_not_ok();
        }
        return;
    }
    if (!is_request(m->op)) return;     /* Status, Result, Error to us: the log has them */
    if (m->fblock == NB_FBLOCK) { on_netblock(m); return; }
    /* the FBlock by FBlockID and InstID; "any instance" (00h, FFh) is the first with that FBlockID. usb2most looks at the
       InstID on the AudioAmplifier only: its start-up asks AUD3's Diagnosis under 0Ah (80374B8), and an AudioAmplifier of
       another InstID - it sends 22.20 to AUD3, which has none - gets no answer */
    bool known = false, found = false;
    for (int i = 0; i < P->nfb && !found; i++)
        if (P->fb[i].fblock == m->fblock) {
            known = true;
            if (m->inst == P->fb[i].inst || m->inst == 0x00 || m->inst == 0xFF || m->fblock != AMP_FBLOCK) {
                FB = &P->fb[i];
                found = true;
            }
        }
    if (!found) {
        if (!known) error(m, MOST_ERR_FBLOCK);
        else ESP_LOGI(TAG, "%02X.%02X %03X from %04X: no such instance", m->fblock, m->inst, m->fkt, m->src);
        return;
    }
    if (FB != P->fb) {
        if (!has_fkt(m->fkt)) { error(m, MOST_ERR_FKT); return; }
        on_other(m);
        return;
    }
    if (!has_fkt(m->fkt) && !also_taken(m->fkt)) { error(m, MOST_ERR_FKT); return; }
    switch (m->fkt) {
    case F_FKTIDS:    on_fktids(m); return;
    case F_NOTIFY:    on_notification(m); return;
    case F_NOTIFYCHK: on_notify_check(m); return;
    case F_ALLOC:     on_alloc(m, true); return;
    case F_DEALLOC:   on_alloc(m, false); return;
    case F_CONNECT:   on_connect(m, true); return;
    case F_DISCONN:   on_connect(m, false); return;
    case F_MUTE:      on_mute(m); return;
    case F_MUTEPAR:   on_mute_params(m); return;
    case F_LEVEL:     on_level(m); return;
    case F_BASS:
    case F_TREBLE:    on_tone(m); return;
    case F_E04:
    case F_E05:
    case F_E07:
    case F_E08:       on_e0x(m); return;
    default:          on_raw(m); return;
    }
}

/* a segmented request that could not be put together: Segmentation Error [0Ch, ErrorInfo] back to its sender, never
   to a group or broadcast and never for a report (MOST 2.5, 2.3.11.1.1, point 8) */
static void seg_failed(void)
{
    const most_long_t *q = &s_seg.err_msg;
    if (!s_seg.err || !is_request(q->op) || (q->type != MOST_RX_LOGICAL && q->type != MOST_RX_PHYSICAL)) return;
    const uint8_t d[2] = { MOST_ERR_SEGMENT, s_seg.err };
    ESP_LOGW(TAG, "%02X.%02X %03X from %04X: segmentation error %02X", q->fblock, q->inst, q->fkt, q->src, s_seg.err);
    send_msg(q->src, q->fblock, q->inst, q->fkt, MOST_OP_ERROR, d, 2);
}

void amp_on_msg(const most_msg_t *t)
{
    EXT_RAM_BSS_ATTR static most_long_t m;
    if (!P) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool whole = most_reassemble(&s_seg, t, &m);
    seg_failed();
    if (whole) on_message(&m);
    FB = P->fb;
    xSemaphoreGive(s_mx);
}

void amp_reset_link(void)
{
    if (!P) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < P->nsinks; i++) {
        s_sink[P->sinks[i].sink].on = false;
        s_sink[P->sinks[i].sink].nch = 0;
    }
    if (IO && IO->route) for (int d = 1; d <= DSP_SINKS; d++) IO->route(d, NULL, 0);
    if (s_any && IO && IO->sound) IO->sound(false);
    s_any = false;
    s_src_n = 0;
    s_nsub = 0;
    s_subscribed = false;
    memset(&s_seg, 0, sizeof s_seg);
    music_hu_changed();
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "link reset: sinks, source and notifications forgotten");
}

void amp_show(void)
{
    if (!P) { printf("amplifier: no profile\n"); return; }
    printf("profile %s: node %03X, group %02X, FBlocks", P->name, P->node, P->group);
    for (int i = 0; i < P->nfb; i++) printf(" %02X.%02X", P->fb[i].fblock, P->fb[i].inst);
    printf("; emulation %s, log %s; %ssubscribed to %04X %02X.%02X\n", amp_active() ? "on" : "off",
           amp_log_on() ? "on" : "off", s_subscribed ? "" : "not ", s_master, P->gw_fblock, P->gw_inst);
    printf("bass %d, treble %d\n", s_tone[0], s_tone[1]);
    if (s_src_n) printf("source: channels %02X %02X, delay %u\n", s_src_ch[0], s_src_ch[1], s_src_delay);
    for (int j = 0; j < P->nsinks; j++) {
        int s = P->sinks[j].sink, k = P->sinks[j].dsp, r = P->sinks[j].row;
        printf("sink %d (%s, row %d): %s%s", s, k == 1 ? "stereo" : k ? "mono" : "not routed", r,
               s_mute[s] ? "muted, " : "", s_sink[s].on ? "connected, channels" : "disconnected");
        for (int i = 0; s_sink[s].on && i < s_sink[s].nch; i++) printf(" %02X", s_sink[s].ch[i]);
        printf("\n");
    }
    for (int r = 1; r <= NROW; r++) {
        printf("row %d:", r);
        if (s_nlvl[r]) printf(" levels dB");
        for (int k = 0; k < s_nlvl[r]; k++) printf(" %d", s_lvl[r][k]);
        if (s_mp_ok[r])
            printf(" mute in %u ms shape %u, out %u ms shape %u", s_mp[r][0] << 8 | s_mp[r][1], s_mp[r][4],
                   s_mp[r][2] << 8 | s_mp[r][3], s_mp[r][5]);
        printf("\n");
    }
    for (int i = 0; i < s_nraw; i++) {
        const amp_fb_t *f = &P->fb[s_raw[i].fb];
        printf("%02X.%02X %03X =", f->fblock, f->inst, s_raw[i].fkt);
        for (int k = 0; k < s_raw[i].len; k++) printf(" %02X", s_raw[i].data[k]);
        printf("\n");
    }
    for (int i = 0; i < s_nsub; i++) {
        const amp_fb_t *f = &P->fb[s_sub[i].fb];
        if (s_sub[i].fkt == ALL_FKTS) printf("notify %04X of %02X.%02X: all\n", s_sub[i].dev, f->fblock, f->inst);
        else printf("notify %04X of %02X.%02X: %03X\n", s_sub[i].dev, f->fblock, f->inst, s_sub[i].fkt);
    }
}

/* ---- firmware side ---- */

static volatile bool s_log = true, s_active = false;

void amp_set_log(bool on) { s_log = on; }
void amp_set_active(bool on) { s_active = on; }
bool amp_log_on(void) { return s_log; }
bool amp_active(void) { return s_active; }

/* The log goes through a queue to a task of its own at the lowest priority: the console's UART takes ~6 ms a line at
   115200 baud and has no transmit buffer, and the head unit's answers must not wait for it. A full queue drops lines
   and counts them */
#define LOG_LINES   64
#define LOG_LEN     160
EXT_RAM_BSS_ATTR static uint8_t s_logq_buf[LOG_LINES * LOG_LEN];
static StaticQueue_t s_logq_ctl;
static QueueHandle_t s_logq;
static volatile uint32_t s_log_drops;

static void log_line(const char *line)
{
    if (!s_logq || xQueueSend(s_logq, line, 0) != pdTRUE) s_log_drops++;
}

static void log_task(void *arg)
{
    char line[LOG_LEN];
    uint32_t told = 0;
    for (;;) {
        if (xQueueReceive(s_logq, line, portMAX_DELAY) != pdTRUE) continue;
        printf("%s\n", line);
        if (s_log_drops != told) {
            told = s_log_drops;
            printf("(log: %lu lines dropped so far)\n", (unsigned long)told);
        }
    }
}

bool amp_tx(const most_msg_t *m)
{
    uint8_t xts = 0;
    esp_err_t e = os8104_send(m, &xts);
    s_xts_last = e == ESP_OK ? 0x10 : xts;
    if (s_log) {
        char line[LOG_LEN];
        int k = most_format(m, false, line, sizeof line);
        if (k < (int)sizeof line)
            snprintf(line + k, sizeof line - k, "  -> %s (bXTS %02X)", e == ESP_OK ? "ok" : "failed", xts);
        log_line(line);
    }
    return e == ESP_OK;
}

/* the task watchdog follows this task: it waits for telegrams a second at most (docs/firmware.md, "Нотатки") */
static void task(void *arg)
{
    QueueHandle_t q = os8104_rx_queue();
    most_msg_t m;
    char line[LOG_LEN];
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(q, &m, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (s_log) {
            most_format(&m, true, line, sizeof line);
            log_line(line);
        }
        power_on_msg(&m);                           /* ShutDown, whatever else answers */
        if (learn_active()) learn_on_msg(&m);       /* the read mode: a node beside the stock amplifier */
        else if (s_active) amp_on_msg(&m);
    }
}

void amp_start(void)
{
    if (!os8104_rx_queue()) return;
    s_logq = xQueueCreateStatic(LOG_LINES, LOG_LEN, s_logq_buf, &s_logq_ctl);
    xTaskCreate(log_task, "amp_log", 3072, NULL, 1, NULL);
    /* above the phone's network tasks (raop.c: 5 and 6): the head unit's answers before the iPhone's packets */
    xTaskCreate(task, "amp", 4096, NULL, 7, NULL);
}
