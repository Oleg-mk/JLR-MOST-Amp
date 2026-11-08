/* see learn.h. The amplifier is found by its position: the read adapter puts it right after this node downstream
   (light from the car's plug through this node into the amplifier), and the node before is asked if that one has no
   AudioAmplifier. What it answers is kept as it came, in the NVS partition LEARN_PART (main.c initialises it), so the
   emulation repeats it byte for byte */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include "learn.h"

static const char *TAG = "learn";

#define NB_FBLOCK   0x01            /* NetBlock */
#define NM_FBLOCK   0x02            /* NetworkMaster */
#define NM_CONFIG   0xA00           /* its Configuration, Status [01h] = network OK */
#define AMP_FBLOCK  0x22            /* AudioAmplifier */
#define DIAG_FBLOCK 0x06            /* Diagnosis */
#define F_DID       0xD22
#define REPLY_MS    500
#define TRIES       3
#define NET_WAIT_MS 5000            /* no Configuration OK seen: the amplifier is asked anyway this long after the lock */
#define VERSION     2               /* 2: function lists of up to 96 bytes */
#define F_CONNECT   0x111           /* in the AudioAmplifier with the sinks: the main one */

/* the Diagnosis answers asked for: serial number, assembly, hardware, software, calibration (usb2most's set) */
static const uint16_t s_dids[LEARN_DID] = { 0xF18C, 0xF112, 0xF191, 0xF188, 0xF124 };

static QueueHandle_t s_q;
static amp_send_t s_send;
static void (*s_set_node)(uint16_t node);
static volatile learn_state_t s_state;
static volatile int s_pos = -1, s_count;
static volatile bool s_net_ok;
/* the buffers are cold - read mode, start-up, a control message now and then: in PSRAM, internal RAM is short */
EXT_RAM_BSS_ATTR static learn_id_t s_id;
EXT_RAM_BSS_ATTR static most_rx_seg_t s_seg;

void learn_on_msg(const most_msg_t *m) { if (s_q) xQueueSend(s_q, m, 0); }
void learn_ring(uint8_t pos, uint8_t count) { s_count = count; s_pos = pos; }
learn_state_t learn_state(void) { return s_state; }
bool learn_active(void) { return s_state != LEARN_OFF; }
void learn_stop(void) { s_state = LEARN_OFF; }

/* what the ring asks this node meanwhile: the master's NetBlock FBlockIDs gets an empty list - a slave with no active
   FBlocks answers so (MOST 2.5, 3.3.4) - and its Configuration OK is noted */
static void serve(const most_msg_t *m)
{
    if (m->fblock == NM_FBLOCK && m->fkt == NM_CONFIG && m->op == MOST_OP_STATUS && m->len && m->data[0] == 0x01)
        s_net_ok = true;
    if (m->fblock == NB_FBLOCK && m->fkt == 0x000 && m->op == MOST_OP_GET && m->type != MOST_RX_BROADCAST &&
        m->type != MOST_RX_GROUPCAST) {
        most_msg_t r = { .dst = m->src, .fblock = NB_FBLOCK, .inst = m->inst, .fkt = 0x000, .op = MOST_OP_STATUS };
        s_send(&r);
    }
}

/* the answer to fkt of fb.inst from dev (0: any), serving the rest meanwhile: 1 Status in *out, -1 Error, 0 none in
   time. The InstID counts too: 22.02 and 22.20 answer the same FktIDs, and a late answer to one must not pass for the
   other */
static int await(uint16_t dev, uint8_t fb, uint8_t inst, uint16_t fkt, most_long_t *out, int ms)
{
    TickType_t t0 = xTaskGetTickCount(), span = pdMS_TO_TICKS(ms);
    most_msg_t m;
    for (TickType_t left = span; left; ) {
        if (xQueueReceive(s_q, &m, left) != pdTRUE) return 0;
        TickType_t gone = xTaskGetTickCount() - t0;
        left = gone < span ? span - gone : 0;
        serve(&m);
        if (m.fblock != fb || m.inst != inst || m.fkt != fkt || (dev && m.src != dev)) continue;
        if (m.op != MOST_OP_STATUS && m.op != MOST_OP_ERROR) continue;
        if (most_reassemble(&s_seg, &m, out)) return out->op == MOST_OP_STATUS ? 1 : -1;
    }
    return 0;
}

static bool ask(uint16_t dst, uint8_t fb, uint8_t inst, uint16_t fkt, uint8_t op, const uint8_t *d, int n,
                uint16_t from, most_long_t *out)
{
    for (int t = 0; t < TRIES; t++) {
        most_msg_t m = { .dst = dst, .fblock = fb, .inst = inst, .fkt = fkt, .op = op, .len = (uint8_t)n };
        if (n) memcpy(m.data, d, n);
        memset(&s_seg, 0, sizeof s_seg);
        if (!s_send(&m)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int r = await(from, fb, inst, fkt, out, REPLY_MS);
        if (r) return r > 0;
    }
    return false;
}

static bool save(const learn_id_t *id)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(LEARN_PART, "amp", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "learned", id, sizeof *id) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool load(learn_id_t *id)
{
    nvs_handle_t h;
    size_t n = sizeof *id;
    if (nvs_open_from_partition(LEARN_PART, "amp", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_blob(h, "learned", id, &n) == ESP_OK && n == sizeof *id && id->ver == VERSION;
    nvs_close(h);
    return ok;
}

static void fail(const char *why)
{
    ESP_LOGE(TAG, "%s: nothing kept", why);
    s_state = LEARN_FAILED;
}

/* the AudioAmplifier with the sinks, Connect in its list - 22.05, 22.02 or 22.0A, not the second 22.20; -1 none */
static int main_amp(const learn_id_t *id)
{
    for (int i = 0; i < id->nfb; i++) {
        if (id->fb[i].fblock != AMP_FBLOCK || !id->fb[i].len) continue;
        uint16_t f[64];
        int n = most_fktids_decode(id->fb[i].fkts, id->fb[i].len, f, 64);
        for (int k = 0; k < n; k++) if (f[k] == F_CONNECT) return i;
    }
    return -1;
}

static bool learn(void)
{
    EXT_RAM_BSS_ATTR static most_long_t r;
    most_msg_t m;
    while (s_pos < 0)                                       /* the ring first */
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) serve(&m);
    ESP_LOGI(TAG, "ring: this node at position %d of %d", s_pos, s_count);
    if (s_set_node) s_set_node((uint16_t)(0x100 + s_pos));  /* dynamic: clear of JLR's fixed addresses (stock_amps 5) */
    TickType_t t0 = xTaskGetTickCount();
    while (!s_net_ok && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(NET_WAIT_MS))
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) serve(&m);
    ESP_LOGI(TAG, "network %s", s_net_ok ? "OK" : "not reported OK: asking anyway");

    /* the amplifier: after this node, else before it */
    memset(&s_id, 0, sizeof s_id);
    s_id.ver = VERSION;
    int cand[2] = { (s_pos + 1) % s_count, (s_pos + s_count - 1) % s_count };
    for (int k = 0; k < 2 && !s_id.nfb; k++) {
        if (!ask((uint16_t)(0x400 + cand[k]), NB_FBLOCK, (uint8_t)cand[k], 0x000, MOST_OP_GET, NULL, 0, 0, &r)) continue;
        bool amp = false;
        for (int i = 0; i + 1 < r.len; i += 2) amp |= r.data[i] == AMP_FBLOCK;
        ESP_LOGI(TAG, "position %d, node %03X: %d FBlocks%s", cand[k], r.src, r.len / 2, amp ? ", an amplifier" : "");
        if (!amp) continue;
        s_id.node = r.src;
        for (int i = 0; i + 1 < r.len && s_id.nfb < LEARN_FB; i += 2) {
            s_id.fb[s_id.nfb].fblock = r.data[i];
            s_id.fb[s_id.nfb++].inst = r.data[i + 1];
        }
    }
    if (!s_id.nfb) { fail("no amplifier next to this node"); return false; }

    /* every FBlock's functions, then the Diagnosis answers */
    uint8_t diag = 0;
    bool has_diag = false;
    for (int i = 0; i < s_id.nfb; i++) {
        uint8_t fb = s_id.fb[i].fblock, inst = s_id.fb[i].inst;
        if (fb == DIAG_FBLOCK) { diag = inst; has_diag = true; }
        if (!ask(s_id.node, fb, inst, 0x000, MOST_OP_GET, NULL, 0, s_id.node, &r) || r.len > sizeof s_id.fb[i].fkts) {
            ESP_LOGW(TAG, "%02X.%02X: no function list", fb, inst);
            continue;
        }
        s_id.fb[i].len = r.len;
        memcpy(s_id.fb[i].fkts, r.data, r.len);
        ESP_LOGI(TAG, "%02X.%02X: function list of %u bytes", fb, inst, r.len);
    }
    /* every FBlock with its list, or the emulation would lack it - Diagnosis above all, which the head unit asks for
       the serial number: a gap is a failed read, shown as such, not a "done" the car would refuse */
    for (int i = 0; i < s_id.nfb; i++)
        if (!s_id.fb[i].len) {
            char why[40];
            snprintf(why, sizeof why, "%02X.%02X gave no function list", s_id.fb[i].fblock, s_id.fb[i].inst);
            fail(why);
            return false;
        }
    if (main_amp(&s_id) < 0) { fail("no AudioAmplifier with sinks"); return false; }
    if (!has_diag) { fail("no Diagnosis FBlock"); return false; }
    for (int i = 0; has_diag && i < LEARN_DID; i++) {
        uint8_t q[2] = { s_dids[i] >> 8, s_dids[i] & 0xFF };
        if (!ask(s_id.node, DIAG_FBLOCK, diag, F_DID, MOST_OP_SETGET, q, 2, s_id.node, &r) || r.len < 2 ||
            r.len > sizeof s_id.did[0].data || r.data[0] != q[0] || r.data[1] != q[1]) {
            ESP_LOGW(TAG, "Diagnosis %04X: no answer", s_dids[i]);
            continue;
        }
        s_id.did[s_id.ndid].len = r.len;
        memcpy(s_id.did[s_id.ndid++].data, r.data, r.len);
        ESP_LOGI(TAG, "Diagnosis %04X: %.*s", s_dids[i], r.len - 2, (const char *)r.data + 2);
    }
    bool serial = false;
    for (int i = 0; i < s_id.ndid; i++) serial |= s_id.did[i].data[0] == 0xF1 && s_id.did[i].data[1] == 0x8C;
    if (!serial) { fail("the serial number (F18C) did not come"); return false; }
    if (!save(&s_id)) { fail("NVS refused"); return false; }
    ESP_LOGI(TAG, "kept: node %03X, %d FBlocks, %d Diagnosis answers, the serial number among them", s_id.node,
             s_id.nfb, s_id.ndid);
    s_state = LEARN_DONE;
    return true;
}

static void task(void *arg)
{
    learn();
    most_msg_t m;
    for (;;)                                                /* still a node of the ring: answer the master */
        if (xQueueReceive(s_q, &m, portMAX_DELAY) == pdTRUE) serve(&m);
}

void learn_start(amp_send_t send, void (*set_node)(uint16_t node))
{
    if (s_q) return;
    s_send = send;
    s_set_node = set_node;
    s_q = xQueueCreate(16, sizeof(most_msg_t));
    s_state = LEARN_RUNNING;
    ESP_LOGI(TAG, "read mode: waiting for the ring");
    xTaskCreate(task, "learn", 4096, NULL, 7, NULL);   /* as the amp task: the ring's answers first */
}

/* ---- the learned amplifier as the emulation's profile ---- */

EXT_RAM_BSS_ATTR static learn_id_t s_kept;
static amp_fb_t s_fb[LEARN_FB];
EXT_RAM_BSS_ATTR static uint16_t s_fk[LEARN_FB][64];
static amp_did_t s_did[LEARN_DID];
static amp_profile_t s_prof;
static char s_name[48];

const amp_profile_t *learn_profile(const amp_profile_t *base)
{
    if (!load(&s_kept)) return NULL;
    bool sane = s_kept.nfb > 0 && s_kept.nfb <= LEARN_FB && s_kept.ndid <= LEARN_DID;
    for (int i = 0; sane && i < s_kept.nfb; i++) sane = s_kept.fb[i].len <= sizeof s_kept.fb[i].fkts;
    for (int i = 0; sane && i < s_kept.ndid; i++)
        sane = s_kept.did[i].len >= 2 && s_kept.did[i].len <= sizeof s_kept.did[i].data;
    int a = sane ? main_amp(&s_kept) : -1;
    if (a < 0) {
        ESP_LOGE(TAG, "the learned amplifier in NVS is %s: the type's profile instead",
                 sane ? "without an AudioAmplifier with sinks" : "damaged");
        return NULL;
    }
    /* the sinks and the gateway's subscriptions follow the learned amplifier's kind, not SW1: AUD3 is 22.0A */
    if (s_kept.fb[a].inst == 0x0A) base = &AMP_AUD3;
    else if (base == &AMP_AUD3) base = &AMP_PRE_HILINE;
    s_prof = *base;
    snprintf(s_name, sizeof s_name, "%s, learned", base->name);
    s_prof.name = s_name;
    s_prof.node = s_kept.node;
    /* the main AudioAmplifier first: the emulation's sinks are fb[0]'s */
    int n = 0;
    for (int j = -1; j < s_kept.nfb; j++) {
        int i = j < 0 ? a : j;
        if ((j >= 0 && i == a) || !s_kept.fb[i].len) continue;
        /* the list as it came for the head unit, the decoded one for what is answered: JLR's lists do not always come
           out the same when coded again */
        s_fb[n] = (amp_fb_t){ s_kept.fb[i].fblock, s_kept.fb[i].inst, s_fk[n],
                              most_fktids_decode(s_kept.fb[i].fkts, s_kept.fb[i].len, s_fk[n], 64),
                              s_kept.fb[i].fkts, s_kept.fb[i].len };
        n++;
    }
    s_prof.fb = s_fb;
    s_prof.nfb = n;
    for (int i = 0; i < s_kept.ndid; i++)
        s_did[i] = (amp_did_t){ (uint16_t)(s_kept.did[i].data[0] << 8 | s_kept.did[i].data[1]),
                                (uint8_t)(s_kept.did[i].len - 2), s_kept.did[i].data + 2,
                                (uint8_t)(s_kept.did[i].len - 2) };
    s_prof.dids = s_did;
    s_prof.ndids = s_kept.ndid;
    return &s_prof;
}

void learn_forget(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(LEARN_PART, "amp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "learned");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "the learned amplifier forgotten: the type's profile from the next start");
}

void learn_show(void)
{
    static const char *st[] = { "off", "reading", "done", "failed" };
    EXT_RAM_BSS_ATTR static learn_id_t shown;      /* s_kept stays the profile's */
    printf("read mode: %s\n", st[s_state]);
    learn_id_t *id = &shown;
    if (!load(id)) { printf("no learned amplifier\n"); return; }
    printf("learned amplifier: node %03X\n", id->node);
    for (int i = 0; i < id->nfb; i++) {
        uint16_t f[64];
        printf("  %02X.%02X: %d functions\n", id->fb[i].fblock, id->fb[i].inst,
               id->fb[i].len ? most_fktids_decode(id->fb[i].fkts, id->fb[i].len, f, 64) : 0);
    }
    for (int i = 0; i < id->ndid; i++)
        printf("  DID %02X%02X: %.*s\n", id->did[i].data[0], id->did[i].data[1], id->did[i].len - 2,
               (const char *)id->did[i].data + 2);
}
