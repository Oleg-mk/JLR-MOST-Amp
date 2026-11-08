/* MOST25 control message layer: OS8104A buffer layout, log lines, segments (see most.h) */
#include <stdio.h>
#include <string.h>
#include "most.h"

void most_decode(const uint8_t r[MOST_RCMB_LEN], most_msg_t *m)
{
    memset(m, 0, sizeof *m);
    m->type = r[0];
    m->src = (uint16_t)(r[1] << 8 | r[2]);
    m->fblock = r[3];
    m->inst = r[4];
    m->fkt = (uint16_t)(r[5] << 4 | r[6] >> 4);
    m->op = r[6] & 0x0F;
    m->tel_id = r[7] >> 4;
    m->len = r[7] & 0x0F;
    if (m->len > MOST_DATA) m->len = MOST_DATA;
    memcpy(m->data, r + 8, MOST_DATA);
}

void most_encode(const most_msg_t *m, uint8_t prio, uint8_t r[MOST_XCMB_LEN])
{
    memset(r, 0, MOST_XCMB_LEN);
    r[0] = prio;
    r[1] = m->type;
    r[2] = m->dst >> 8;
    r[3] = m->dst & 0xFF;
    r[4] = m->fblock;
    r[5] = m->inst;
    r[6] = (m->fkt >> 4) & 0xFF;
    r[7] = (uint8_t)((m->fkt & 0x0F) << 4 | (m->op & 0x0F));
    r[8] = (uint8_t)(m->tel_id << 4 | (m->len & 0x0F));
    memcpy(r + 9, m->data, m->len <= MOST_DATA ? m->len : MOST_DATA);
}

/* names for the log; 0x1xx..0x4xx methods of the amplifier answer with Result where a property says Status */
const char *most_op_name(uint16_t fkt, uint8_t op)
{
    bool method = fkt == 0x111 || fkt == 0x112 || (fkt >= 0x101 && fkt <= 0x103);
    static const char *prop[16] = { "Set", "Get", "SetGet", "Inc", "Dec", "GetIf", "op6", "op7", "op8", "ErrAck",
                                    "opA", "opB", "Status", "opD", "Interface", "Error" };
    static const char *meth[16] = { "Start", "Abort", "StartResult", "op3", "op4", "GetIf", "StartResultAck",
                                    "AbortAck", "StartAck", "ErrorAck", "ProcessingAck", "Processing", "Result",
                                    "ResultAck", "Interface", "Error" };
    return (method ? meth : prop)[op & 0x0F];
}

int most_format(const most_msg_t *m, bool rx, char *buf, size_t n)
{
    static const char *how[4] = { "log", "phys", "bcast", "group" };
    int k = rx ? snprintf(buf, n, "rx %04X %-5s ", m->src, how[m->type & 3]) : snprintf(buf, n, "tx > %04X  ", m->dst);
    k += snprintf(buf + k, n > (size_t)k ? n - k : 0, "%02X.%02X %03X %-11s", m->fblock, m->inst, m->fkt,
                  most_op_name(m->fkt, m->op));
    if (m->tel_id) k += snprintf(buf + k, n > (size_t)k ? n - k : 0, " seg%u", m->tel_id);
    k += snprintf(buf + k, n > (size_t)k ? n - k : 0, " [");
    for (int i = 0; i < m->len && i < MOST_DATA; i++)
        k += snprintf(buf + k, n > (size_t)k ? n - k : 0, i ? " %02X" : "%02X", m->data[i]);
    k += snprintf(buf + k, n > (size_t)k ? n - k : 0, "]");
    return k;
}

static void to_long(const most_msg_t *m, most_long_t *l)
{
    l->type = m->type;
    l->src = m->src;
    l->dst = m->dst;
    l->fblock = m->fblock;
    l->inst = m->inst;
    l->fkt = m->fkt;
    l->op = m->op;
    l->len = 0;
}

/* JLR-HU common.ts: segment 1 opens with a counter in data[0], each next one carries counter + 1; the payload is
   data[1..]. A sender's segment out of order, or a new message of its own, drops the one it had under way; other
   senders' telegrams leave it alone */
static void seg_error(most_rx_seg_t *s, uint8_t code, const most_msg_t *m)
{
    s->err = code;
    to_long(m, &s->err_msg);
}

bool most_reassemble(most_rx_seg_t *s, const most_msg_t *m, most_long_t *out)
{
    s->err = 0;
    int k = -1;                                     /* the sender's slot, if it has a message under way */
    for (int i = 0; i < MOST_SEG_SLOTS; i++) if (s->slot[i].busy && s->slot[i].src == m->src) k = i;
    if (m->tel_id == MOST_TEL_SINGLE || m->tel_id == MOST_TEL_FIRST) {
        if (k >= 0) {                               /* its unfinished message is dropped and reported */
            s->slot[k].busy = false;
            s->err = 0x07;
            s->err_msg = s->slot[k].msg;
            s->err_msg.len = 0;
        }
    }
    if (m->tel_id == MOST_TEL_SINGLE) {
        to_long(m, out);
        out->len = m->len;
        memcpy(out->data, m->data, m->len);
        return true;
    }
    if (m->len < 1) return false;
    int n = m->len - 1;
    if (m->tel_id == MOST_TEL_FIRST) {
        for (int i = 0; i < MOST_SEG_SLOTS && k < 0; i++) if (!s->slot[i].busy) k = i;
        if (k < 0) { seg_error(s, 0x04, m); return false; }
        s->slot[k].busy = true;
        s->slot[k].src = m->src; s->slot[k].fkt = m->fkt; s->slot[k].fblock = m->fblock; s->slot[k].inst = m->inst;
        s->slot[k].op = m->op;
        s->slot[k].next = m->data[0] + 1;
        to_long(m, &s->slot[k].msg);
        memcpy(s->slot[k].msg.data, m->data + 1, n);
        s->slot[k].msg.len = (uint8_t)n;
        return false;
    }
    if (k < 0) { seg_error(s, 0x01, m); return false; }
    if (s->slot[k].fkt != m->fkt || s->slot[k].fblock != m->fblock || s->slot[k].inst != m->inst ||
        s->slot[k].op != m->op || m->data[0] != s->slot[k].next) {
        s->slot[k].busy = false;
        seg_error(s, 0x03, m);
        return false;
    }
    if (s->slot[k].msg.len + n > MOST_LONG) {
        s->slot[k].busy = false;
        seg_error(s, 0x02, m);
        return false;
    }
    memcpy(s->slot[k].msg.data + s->slot[k].msg.len, m->data + 1, n);
    s->slot[k].msg.len += n;
    s->slot[k].next++;
    if (m->tel_id != MOST_TEL_LAST) return false;
    s->slot[k].busy = false;
    *out = s->slot[k].msg;
    return true;
}

int most_split(const most_long_t *in, most_msg_t *out, int max)
{
    most_msg_t t = { .type = in->type, .src = in->src, .dst = in->dst, .fblock = in->fblock, .inst = in->inst,
                     .fkt = in->fkt, .op = in->op };
    /* groupcast and broadcast carry 16 data bytes, not 17 (13.1.3): one data byte less per telegram */
    int room = (in->dst >= 0x0300 && in->dst <= 0x03FF) ? MOST_DATA - 1 : MOST_DATA;
    if (in->len <= room) {
        if (max < 1) return 0;
        t.len = in->len;
        memcpy(t.data, in->data, in->len);
        out[0] = t;
        return 1;
    }
    int per = room - 1, segs = (in->len + per - 1) / per;
    if (segs > max) return 0;
    for (int i = 0, off = 0; i < segs; i++, off += per) {
        int n = in->len - off < per ? in->len - off : per;
        t.tel_id = i == 0 ? MOST_TEL_FIRST : i == segs - 1 ? MOST_TEL_LAST : MOST_TEL_NEXT;
        t.data[0] = (uint8_t)i;
        memcpy(t.data + 1, in->data + off, n);
        t.len = (uint8_t)(n + 1);
        out[i] = t;
    }
    return segs;
}

/* MOST Specification 2.5, 2.3.9: the bit field of the FBlock's functions, run-length coded. Listed is every FktID where
   the bit changes, the bit starting at 1 on 000h: its example (002 004 006 022 024 A00 A02) stands for 000-001,
   004-005, 022-023 and A00-A01. JLR's amplifiers answer so: usb2most keeps their lists (ref/usb2most). 12 bits each,
   high nibble first, a stuffing nibble 0 to end on a whole byte */
static bool put12(uint8_t *out, int max, int *nib, uint16_t v)
{
    for (int sh = 8; sh >= 0; sh -= 4, (*nib)++) {
        if (*nib / 2 >= max) return false;
        uint8_t x = (v >> sh) & 0xF;
        if (*nib & 1) out[*nib / 2] |= x;
        else out[*nib / 2] = (uint8_t)(x << 4);
    }
    return true;
}

int most_fktids(const uint16_t *f, int n, uint8_t *out, int max)
{
    int nib = 0;
    for (int i = 0, start = 0; i < n; i++) {
        if (i + 1 < n && f[i + 1] == f[i] + 1) continue;    /* inside a run: f[start..i] */
        if (start == 0 && f[0] > 0 && !put12(out, max, &nib, 0)) return 0;    /* no 000h: the bit drops at once */
        if ((start > 0 || f[0] > 0) && !put12(out, max, &nib, f[start])) return 0;
        if (f[i] < 0xFFF && !put12(out, max, &nib, (uint16_t)(f[i] + 1))) return 0;
        start = i + 1;
    }
    return (nib + 1) / 2;
}

static uint8_t nibble(const uint8_t *b, int p) { return (uint8_t)((b[p / 2] >> (p & 1 ? 0 : 4)) & 0xF); }

int most_fktids_decode(const uint8_t *b, int n, uint16_t *out, int max)
{
    /* the bit flips in the order of the list, which need not be ascending: JLR's Diagnosis lists are not (usb2most 25AE7,
       278F5). A 000h after the start is the padding some lists end in (25AA6) */
    int k = 0, nv = 2 * n / 3;              /* a last lone nibble is the stuffing */
    uint16_t from = 0;
    bool on = true;
    for (int i = 0; i < nv; i++) {
        uint16_t v = (uint16_t)(nibble(b, 3 * i) << 8 | nibble(b, 3 * i + 1) << 4 | nibble(b, 3 * i + 2));
        if (i && !v) break;
        if (on) for (uint16_t f = from; f < v && k < max; f++) out[k++] = f;
        from = v;
        on = !on;
    }
    if (on && nv) for (uint16_t f = from; f <= 0xFFF && k < max; f++) out[k++] = f;
    return k;
}
