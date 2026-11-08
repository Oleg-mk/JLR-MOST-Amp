/* OS8104A driver: register access over I2C0, reset with the power-on interrupt, master and slave start, INT events.
   Sequences follow DS8104AFP4 chapter 16; deviations are noted where they are made */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "board.h"
#include "os8104.h"

static const char *TAG = "os8104";

/* 200 kHz: above it the chip may stretch SCL at every byte (DS8104AFP4 18.5.7, note 1); R8, R9 are the pull-ups */
#define I2C_HZ       200000
#define I2C_TOUT_MS  50
/* Mode 5 (DS8104AFP4 7.3.5): ports 1..3 I2S as in 7.2.1.1, port 0 S/PDIF; FSY and SCK out, SX outputs on, SR1/SX1
   not transparent */
#define SDC1_RUN     (SDC1_EDG | SDC1_DEL | SDC1_IO | SDC1_SPD | SDC1_MT | SDC1_TCE)

typedef struct { uint8_t reg, val; } regval_t;

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;    /* one user of the chip at a time: console, starts, the INT task */
static SemaphoreHandle_t s_int;     /* INT falling edge, taken by the reset sequence */
static TaskHandle_t s_task;
static uint8_t s_ver[3];
static bool s_ready;                /* the power-on interrupt was seen and cleared */
static QueueHandle_t s_rxq;         /* received telegrams, for the amp task */
static SemaphoreHandle_t s_txlock;  /* one message in mXCMB at a time */
static SemaphoreHandle_t s_txdone;  /* MTX seen, s_xts holds bXTS */
static volatile uint8_t s_xts;
static uint32_t s_rx_dropped;
static bool s_rel_pending;          /* a telegram was queued but mRCMB's release did not go through: not again */
static volatile bool s_lock_masked; /* a lock loss is masked in bXSR until the ring locks again (6.2.4) */
static bool s_page_lost;            /* the page register could not be set back to 0 (os8104_alloc_valid): until it
                                       is, every access would hit page 3, so each one first tries again */

#define INT_POLL_MS  50             /* the INT task looks at the pin itself this often, edge or not */

static esp_err_t system_msg(uint8_t type, uint8_t arg, uint8_t ans[10]);

/* page 0 again; call with s_lock held */
static esp_err_t page0(void)
{
    const uint8_t b[2] = { OS_PAGE, 0 };
    esp_err_t e = i2c_master_transmit(s_dev, b, sizeof b, I2C_TOUT_MS);
    s_page_lost = e != ESP_OK;
    return e;
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    if (s_page_lost && page0() != ESP_OK) return ESP_FAIL;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, I2C_TOUT_MS);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t b[1 + 64];
    if (n > 64) return ESP_ERR_INVALID_SIZE;
    if (s_page_lost && page0() != ESP_OK) return ESP_FAIL;
    b[0] = reg;     /* MAP, then data; MAP increments after each byte (DS8104AFP4 5.1.1) */
    memcpy(b + 1, buf, n);
    return i2c_master_transmit(s_dev, b, n + 1, I2C_TOUT_MS);
}

static esp_err_t wr8(uint8_t reg, uint8_t v) { return wr(reg, &v, 1); }

static esp_err_t wr_seq(const regval_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = wr8(s[i].reg, s[i].val);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "write %02Xh = %02Xh: %s", s[i].reg, s[i].val, esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

/* SX1 and SX3 send zeros until the head unit connects a sink (stage 4) */
static esp_err_t rx_routes_zero(void)
{
    esp_err_t e = ESP_OK;
    for (int k = 0; k < 8 && e == ESP_OK; k++) {
        e = wr8(os8104_sx(1, k), OS_MRA_ZERO);
        if (e == ESP_OK) e = wr8(os8104_sx(3, k), OS_MRA_ZERO);
    }
    return e;
}

/* S/PDIF out of SX0 from the ESP32 stream on SR1 (7.4.5, figures 7-7 and 7-8). Per subframe the SX0 bytes are the LS,
   middle and MS byte of a 24-bit sample, then V U C P in bits 0..3 with the next preamble above. So the top 24 bits of
   each 32-bit slot go out, VUC = 000 comes from MRA F8h, and the chip adds parity and the preambles; all C bits are 0
   (7.4.5). SX0 locations follow chapter 12 (47h + 8k): figure 7-8 labels them like the SR0 references (40h + 8k) */
static esp_err_t spdif_routes(void)
{
    static const int8_t sr1[8] = { 2, 1, 0, -1, 6, 5, 4, -1 };     /* SR1 byte for each SX0 byte; -1: zero */
    esp_err_t e = ESP_OK;
    for (int k = 0; k < 8 && e == ESP_OK; k++)
        e = wr8(os8104_sx(0, k), sr1[k] < 0 ? OS_MRA_ZERO : os8104_sr(1, sr1[k]));
    return e;
}

/* the public calls before a successful os8104_init() */
#define CHECK_INIT() do { if (!s_lock) return ESP_ERR_INVALID_STATE; } while (0)

/* call with s_lock held */
static esp_err_t reset_locked(void)
{
    s_ready = false;
    s_lock_masked = false;
    s_rel_pending = false;
    s_page_lost = false;            /* a reset lands on page 0 */
    /* RS low for more than 1 ms (DS8104AFP4 16.1), counting the time the pin takes to discharge C14. After release
       C14 charges through R12, so RS rises ~12 ms later; SCL is high then (the bus is idle), which selects I2C */
    gpio_set_level(PIN_MOST_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    xSemaphoreTake(s_int, 0);       /* an edge from before the reset is not the power-on interrupt (18.5.2, note 2) */
    gpio_set_level(PIN_MOST_RST, 1);
    if (xSemaphoreTake(s_int, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "reset: no power-on interrupt");
        return ESP_ERR_TIMEOUT;
    }
    /* SCK out before the ring locks, so it is never floating then (16.1); R16 holds it low until here */
    esp_err_t e = wr8(OS_bSDC1, SDC1_IO);
    if (e == ESP_OK) e = rd(OS_VERSION, s_ver, sizeof s_ver);
    if (e == ESP_OK) e = wr8(OS_bMSGC, MSGC_RERR);     /* clears the power-on interrupt */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "reset: %s", esp_err_to_name(e));
        return e;
    }
    s_ready = true;
    ESP_LOGI(TAG, "reset: version %02X, %02X.%02X; All-Bypass", s_ver[0], s_ver[1], s_ver[2]);
    return ESP_OK;
}

esp_err_t os8104_reset(void)
{
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = reset_locked();
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_start_master(void)
{
    static const regval_t seq[] = {
        /* E2h as in 16.2: timing master, transmitter on, an active node (table 6-10) */
        { OS_bXCR, XCR_MTR | XCR_OE | XCR_LB | XCR_ABY },
        /* PLL on the 12.288 MHz crystal (256Fs), RMCK 12.288 MHz for the PCM5122 */
        { OS_bCM1, CM1_RD256 | CM1_XTL256 | CM1_MXXTAL },
        { OS_bXSR2, XSR2_DFE },                         /* deep RX FIFO in the timing master (6.3) */
        { OS_bSDC1, SDC1_RUN },
        { OS_bSDC2, SDC2_64FS },
        { OS_bSDC3, 0 },                                /* source ports on, inputs not muted (7.2.3) */
        { OS_bXSR, XSR_MSL | XSR_ME },                  /* 50h: only lock errors count; captures cleared */
        /* 8 synchronous quadlets = 32 channels, 7 left for packets; the allocation table starts with De-Allocate All
           once the loop has locked (below) */
        { OS_bSBC, 8 },
        { OS_bIE, IE_IERR | IE_IALC | IE_IMRX | IE_IMTX },
        { OS_bMSGC, MSGC_RALL | MSGC_RBE },
    };
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = reset_locked();
    uint8_t cm3 = 0;
    if (e == ESP_OK) e = wr8(seq[0].reg, seq[0].val);
    /* the Frequency Regulator is for slaves (9.5); the master's PLL follows the crystal only */
    if (e == ESP_OK) e = rd(OS_bCM3, &cm3, 1);
    if (e == ESP_OK) e = wr8(OS_bCM3, cm3 & (CM3_ASR | CM3_ACD));
    if (e == ESP_OK) e = wr_seq(seq + 1, sizeof seq / sizeof seq[0] - 1);
    if (e == ESP_OK) e = spdif_routes();
    if (e == ESP_OK) e = rx_routes_zero();
    xSemaphoreGive(s_lock);
    if (e != ESP_OK) return e;
    ESP_LOGI(TAG, "timing master on the crystal, S/PDIF from SR1");
    /* a timing master's application sends De-Allocate All to itself after a reset or a bSBC change (13.5.2.5, 14.2),
       so a bench test of the allocation starts from an empty table; it needs the loop locked, a second at most */
    bool light, locked = false;
    for (int i = 0; i < 20 && !locked; i++) {
        if (os8104_poll(&light, &locked) != ESP_OK || !locked) vTaskDelay(pdMS_TO_TICKS(50));
    }
    uint8_t a[10];
    if (!locked || system_msg(0x04, 0x7F, a) != ESP_OK || a[0] != 0x01)
        ESP_LOGW(TAG, "De-Allocate All not done: %s", locked ? "no answer" : "the loop is not locked");
    return ESP_OK;
}

esp_err_t os8104_start_slave(uint16_t node, uint8_t group)
{
    const regval_t seq[] = {
        { OS_bNAH, node >> 8 }, { OS_bNAL, node & 0xFF }, { OS_bGA, group },
        /* the PLL follows RX; the Frequency Regulator (on after an Enhanced reset) falls back to the crystal while RX
           is absent or off frequency, so RMCK, FSY and SCK keep running (9.5) */
        { OS_bCM1, CM1_RD256 | CM1_XTL256 | CM1_MXXTAL },
        /* 42h: transmitter on, an active node - with LB clear the same Active Transmitter as 16.3's 62h (table 6-10).
           ABY must stay set: the node delay bNDR counts only then, and without it the node gets no position. Not yet
           tried in a car */
        { OS_bXCR, XCR_OE | XCR_ABY },
        { OS_bSDC1, SDC1_RUN },
        { OS_bSDC2, SDC2_64FS },
        { OS_bSDC3, 0 },
        { OS_bXSR, XSR_MSL | XSR_ME },
        { OS_bIE, IE_IERR | IE_IALC | IE_IMRX | IE_IMTX },
        { OS_bMSGC, MSGC_RALL | MSGC_RBE },
    };
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = reset_locked();
    if (e == ESP_OK) e = wr_seq(seq, sizeof seq / sizeof seq[0]);
    /* the Frequency Regulator's crystal must go once the PLL has re-locked to RX (9.5): ACD has the chip do it by
       itself; FREN and ASR stay as the reset left them */
    uint8_t cm3 = 0;
    if (e == ESP_OK) e = rd(OS_bCM3, &cm3, 1);
    if (e == ESP_OK) e = wr8(OS_bCM3, (cm3 & (CM3_FREN | CM3_ASR)) | CM3_ACD);
    if (e == ESP_OK) e = spdif_routes();
    if (e == ESP_OK) e = rx_routes_zero();
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) ESP_LOGI(TAG, "timing slave, node %03Xh, group %02Xh, S/PDIF from SR1", node, group);
    return e;
}

static void (*s_ring_cb)(uint8_t pos, uint8_t count);

void os8104_on_ring(void (*cb)(uint8_t pos, uint8_t count)) { s_ring_cb = cb; }

static void (*s_light_cb)(bool on);

void os8104_on_light(void (*cb)(bool on)) { s_light_cb = cb; }

esp_err_t os8104_set_node(uint16_t node, uint8_t group)
{
    const regval_t seq[] = { { OS_bNAH, node >> 8 }, { OS_bNAL, node & 0xFF }, { OS_bGA, group } };
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = wr_seq(seq, sizeof seq / sizeof seq[0]);
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) ESP_LOGI(TAG, "node %03Xh, group %02Xh", node, group);
    return e;
}

esp_err_t os8104_read(uint8_t reg, uint8_t *buf, size_t n)
{
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = rd(reg, buf, n);
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_write(uint8_t reg, const uint8_t *buf, size_t n)
{
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = wr(reg, buf, n);
    xSemaphoreGive(s_lock);
    return e;
}

const uint8_t *os8104_version(void) { return s_ver; }

QueueHandle_t os8104_rx_queue(void) { return s_rxq; }

/* 13.4: fill mXCMB, set STX, wait for MTX; the chip retries by itself (bXRTY 6 tries, bXTIM 11 x 333 us at 48 kHz),
   so 200 ms covers every retry. The first mXCMB write overwrites the version bytes, read at reset already */
esp_err_t os8104_send(const most_msg_t *m, uint8_t *xts)
{
    CHECK_INIT();
    uint8_t raw[MOST_XCMB_LEN];
    most_encode(m, 0x01, raw);
    xSemaphoreTake(s_txlock, portMAX_DELAY);
    xSemaphoreTake(s_txdone, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = s_ready ? wr(OS_mXCMB, raw, sizeof raw) : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) e = wr8(OS_bMSGC, MSGC_STX);
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) e = xSemaphoreTake(s_txdone, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    uint8_t x = e == ESP_OK ? s_xts : 0;
    xSemaphoreGive(s_txlock);
    if (xts) *xts = x;
    if (e == ESP_OK && (x & 0xF0) != 0x10) e = ESP_FAIL;
    return e;
}

esp_err_t os8104_loop_routes(bool ring)
{
    CHECK_INIT();
    uint8_t tx[8];
    for (int k = 0; k < 8; k++) tx[k] = os8104_sr(1, k);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* ring: SR1 -> frame bytes 0..7 -> RX bytes 0..7 -> SX1, SX3. Direct: SR1 -> SX1, SX3; SX byte 0 takes no SR byte
       7, as 12.8 requires */
    esp_err_t e = ring ? wr(0x00, tx, sizeof tx) : ESP_OK;
    for (uint8_t k = 0; k < 8 && e == ESP_OK; k++) {
        e = wr8(os8104_sx(1, k), ring ? k : tx[k]);
        if (e == ESP_OK) e = wr8(os8104_sx(3, k), ring ? k : tx[k]);
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_loop_off(void)
{
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    for (uint8_t k = 0; k < 8 && e == ESP_OK; k++) e = wr8(k, k);     /* frame byte k from RX byte k (14.3) */
    if (e == ESP_OK) e = rx_routes_zero();
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_spdif(bool on)
{
    CHECK_INIT();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (on) e = spdif_routes();
    else for (int k = 0; k < 8 && e == ESP_OK; k++) e = wr8(os8104_sx(0, k), OS_MRA_ZERO);
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_route_stereo(const uint8_t ch[4])
{
    CHECK_INIT();
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* per slot: the sample's MSB and LSB, then two zero bytes (MRT at 64Fs, 12.2) */
    const uint8_t z = OS_MRA_ZERO, src[8] = { ch ? ch[0] : z, ch ? ch[1] : z, z, z, ch ? ch[2] : z, ch ? ch[3] : z, z, z };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    for (int k = 0; k < 8 && e == ESP_OK; k++) e = wr8(os8104_sx(1, k), src[k]);
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t os8104_route_mono(int k, const uint8_t ch[2])
{
    CHECK_INIT();
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (k < 0 || k > 2) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = wr8(os8104_sx(3, 2 * k), ch ? ch[0] : OS_MRA_ZERO);
    if (e == ESP_OK) e = wr8(os8104_sx(3, 2 * k + 1), ch ? ch[1] : OS_MRA_ZERO);
    xSemaphoreGive(s_lock);
    return e;
}

/* a system message to the timing master (13.5.2): mXCMB holds priority, type, the target 0400h (node position 0),
   a reserved byte, the argument, then zeros to D4h as tables 13-20 and 13-22 ask - not the last telegram's data; the
   master's answer comes back into mXCMB from C7h: Answer1, Answer2, P0..P7 */
static esp_err_t system_msg(uint8_t type, uint8_t arg, uint8_t ans[10])
{
    const uint8_t x[MOST_XCMB_LEN] = { 0x01, type, 0x04, 0x00, 0x00, arg };
    xSemaphoreTake(s_txlock, portMAX_DELAY);
    xSemaphoreTake(s_txdone, 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = s_ready ? wr(OS_mXCMB, x, sizeof x) : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) e = wr8(OS_bMSGC, MSGC_STX);
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) e = xSemaphoreTake(s_txdone, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
    if (e == ESP_OK && (s_xts & 0xF0) != 0x10) e = ESP_FAIL;
    if (e == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        e = rd(OS_mXCMB + 7, ans, 10);
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_txlock);
    return e;
}

/* Answer1 of a resource message: 01h granted, 02h the master busy - asked again, 20 times (MOST 2.5, 3.7.2.2, p. 193;
   DS8104AFP4 figure 13-7) - 03h denied (Answer2: the channels free), 04h wrong count, 05h wrong target */
#define BUSY_TRIES 20

static esp_err_t resource_msg(uint8_t type, uint8_t arg, uint8_t a[10])
{
    esp_err_t e = ESP_OK;
    for (int t = 0; t < BUSY_TRIES; t++) {
        memset(a, 0, 10);
        e = system_msg(type, arg, a);
        if (e != ESP_OK || a[0] != 0x02) break;
        esp_task_wdt_reset();               /* the amp task's watchdog, if this runs in it */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return e;
}

int os8104_alloc(int n, uint8_t ch[8], uint8_t *delay)
{
    if (!s_lock || n < 1 || n > 8) return 0;
    uint8_t a[10];
    esp_err_t e = resource_msg(0x03, (uint8_t)n, a);
    if (e != ESP_OK || a[0] != 0x01) {
        ESP_LOGW(TAG, "allocate %d channels: %s, answer %02Xh, %u free", n, esp_err_to_name(e), a[0], a[1]);
        return 0;
    }
    /* the channels' MRT locations are left as they are, passing the ring through: the source has nothing to send
       (usb2most routes nothing into them either), and a stale allocation can then never overwrite another's data */
    int got = 0;
    for (int i = 0; i < n && a[2 + i] <= 0x3B; i++) ch[got++] = a[2 + i];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (rd(OS_bNDR, delay, 1) != ESP_OK) *delay = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "allocated %d channels from %02Xh (the label), node delay %u", got, ch[0], *delay);
    return got;
}

esp_err_t os8104_dealloc(const uint8_t *ch, int n)
{
    CHECK_INIT();
    if (n < 1) return ESP_OK;
    uint8_t a[10];
    esp_err_t e = resource_msg(0x04, ch[0], a);
    if (e == ESP_OK && a[0] != 0x01) e = ESP_FAIL;
    if (e != ESP_OK) ESP_LOGW(TAG, "de-allocate %02Xh: %s, answer %02Xh", ch[0], esp_err_to_name(e), a[0]);
    else ESP_LOGI(TAG, "de-allocated %d channels of %02Xh", n, ch[0]);
    return e;
}

bool os8104_alloc_valid(const uint8_t *ch, int n)
{
    if (!s_lock || !s_ready || n < 1) return false;
    /* this node's copy of the allocation table, mCRA 0380h..03BBh on page 3 (14.1.1): each channel of ours holds its
       Connection Label (MSB masked), 70h a free one. The only page switch of the driver, under s_lock */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = wr8(OS_PAGE, 3) == ESP_OK;
    for (int i = 0; ok && i < n; i++) {
        uint8_t v = 0;
        ok = ch[i] <= 0x3B && rd(0x80 + ch[i], &v, 1) == ESP_OK && (v & 0x7F) == ch[0];
    }
    /* back to page 0 whatever happened above: a write that fails leaves the chip on page 3, so every access tries
       again until one goes through (rd, wr) */
    if (page0() != ESP_OK) ESP_LOGE(TAG, "page 0 not restored: tried again before every access");
    xSemaphoreGive(s_lock);
    return ok;
}

esp_err_t os8104_poll(bool *light, bool *locked)
{
    CHECK_INIT();
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t cm2 = 0, x = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = rd(OS_bCM2, &cm2, 1);
    if (e == ESP_OK) {
        *light = cm2 & CM2_NAC;
        *locked = !(cm2 & CM2_LOK);
        /* locked again: the lock loss masked after the last one counts again (int_task) */
        if (*locked && s_lock_masked && rd(OS_bXSR, &x, 1) == ESP_OK && wr8(OS_bXSR, x & (XSR_MSL | XSR_ME)) == ESP_OK)
            s_lock_masked = false;
    }
    xSemaphoreGive(s_lock);
    return e;
}

/* INT: log and clear each event. Several can be pending; INT goes high for >= 1 us between them (11.3). The pin is
   looked at every INT_POLL_MS besides the edges: an event left uncleared after a failed I2C transfer holds INT low,
   and with no further edge the node would hear nothing more */
static void int_task(void *arg)
{
    esp_task_wdt_add(NULL);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(INT_POLL_MS));
        esp_task_wdt_reset();
        if (gpio_get_level(PIN_MOST_INT)) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; s_ready && i < 8 && !gpio_get_level(PIN_MOST_INT); i++) {
            uint8_t st, clr = 0;
            if (rd(OS_bMSGS, &st, 1) != ESP_OK) break;
            if (st & MSGS_ERR) {
                uint8_t x = 0, cm2 = 0;
                rd(OS_bXSR, &x, 1);
                rd(OS_bCM2, &cm2, 1);
                ESP_LOGW(TAG, "error: bXSR %02Xh%s%s, PLL %s, RX %s", x, x & XSR_EXL ? ", lock lost" : "",
                         x & XSR_ESL ? ", S/PDIF lock lost" : "", cm2 & CM2_LOK ? "unlocked" : "locked",
                         cm2 & CM2_NAC ? "active" : "dark");
                /* a lock loss that lasts would raise ERR again at once (6.2.4): it is masked until the ring locks
                   again (os8104_poll); the write also clears the captures */
                uint8_t mask = x & (XSR_MSL | XSR_MXL | XSR_ME);
                if (x & XSR_EXL) {
                    mask |= XSR_MXL;
                    s_lock_masked = true;
                }
                wr8(OS_bXSR, mask);
                if (s_light_cb && !(cm2 & CM2_NAC)) s_light_cb(false);
                clr |= MSGC_RERR;
            }
            if (st & MSGS_ALC) {
                uint8_t npr = 0, mpr = 0, ndr = 0, mdr = 0;
                rd(OS_bNPR, &npr, 1);
                rd(OS_bMPR, &mpr, 1);
                rd(OS_bNDR, &ndr, 1);
                rd(OS_bMDR, &mdr, 1);
                ESP_LOGI(TAG, "ring: position %u of %u nodes, delay %u of %u", npr, mpr, ndr, mdr);
                if (s_ring_cb && mpr) s_ring_cb(npr, mpr);
                if (s_light_cb) s_light_cb(true);
                clr |= MSGC_RALC;
            }
            if (st & MSGS_MTX) {
                uint8_t x = 0;
                rd(OS_bXTS, &x, 1);
                s_xts = st & MSGS_TXR ? 0x10 : x;
                xSemaphoreGive(s_txdone);
                clr |= MSGC_RMTX;
            }
            if (st & MSGS_MRX) {
                /* read mRCMB, then release it with RBE: until then the chip takes no further message (13.3). One
                   whose release failed was queued already and is only released again */
                uint8_t raw[MOST_RCMB_LEN];
                if (!s_rel_pending && rd(OS_mRCMB, raw, sizeof raw) == ESP_OK) {
                    most_msg_t m;
                    most_decode(raw, &m);
                    if (xQueueSend(s_rxq, &m, 0) != pdTRUE && !(s_rx_dropped++ % 16))
                        ESP_LOGW(TAG, "receive queue full, %lu telegrams dropped", (unsigned long)s_rx_dropped);
                }
                clr |= MSGC_RMRX | MSGC_RBE;
            }
            if (!clr) break;
            esp_err_t w = wr8(OS_bMSGC, clr);
            s_rel_pending = w != ESP_OK && (clr & MSGC_RBE);
            if (w != ESP_OK) break;         /* the next look, INT_POLL_MS on, writes it again */
        }
        xSemaphoreGive(s_lock);
    }
}

static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_int, &woken);
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

esp_err_t os8104_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_MOST_SDA,
        .scl_io_num = PIN_MOST_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "I2C0 bus");
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_OS8104A,
        .scl_speed_hz = I2C_HZ,
        .scl_wait_us = 10000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_dev), TAG, "OS8104A device");
    s_lock = xSemaphoreCreateMutex();
    s_int = xSemaphoreCreateBinary();
    s_rxq = xQueueCreate(32, sizeof(most_msg_t));
    s_txlock = xSemaphoreCreateMutex();
    s_txdone = xSemaphoreCreateBinary();
    xTaskCreate(int_task, "os8104", 3072, NULL, 8, &s_task);     /* above the amp task and the phone's network */
    gpio_set_intr_type(PIN_MOST_INT, GPIO_INTR_NEGEDGE);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;    /* already installed is fine */
    return gpio_isr_handler_add(PIN_MOST_INT, int_isr, NULL);
}
