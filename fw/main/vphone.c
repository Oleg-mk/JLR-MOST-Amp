/* QEMU only: QEMU has no Bluetooth radio, no I2S and no ring, so a script on the PC stands in for what is missing. It
   reaches the firmware over the emulated OpenCores Ethernet (wifi_qemu.c; QEMU's user network shows the PC as
   10.0.2.2). tools/vphone.py sends the phone's 16-bit frames in A2DP-like bursts and asks for 48 kHz frames at the
   ring's pace, both clocks simulated; tools/vamp.py sends the head unit's commands and the sinks' sound as the ring
   would bring them, and takes the sound engine's output; tools/vsource.py does both at once, with AirPlay too, for
   the switching between the head unit's music and the phones. They play the result on the PC's sound card. Messages,
   little-endian:
     to us:   [type, 0, 0, 0, n (32 bits)] then
              'R' a phone stream at n Hz; 'I' n phone frames (16-bit left, right); 'X' the phone's stream stops;
              'P' n frames wanted (n <= CHUNK), the phone's stream alone;
              'T' the amplifier's type n, as SW1 sets it (amp.h), or the amplifier read for it, afresh; ends the
                  read mode;
              'W' n = 1: the telegrams the amplifier or the read mode send come to the script as 'M';
              'L' the read mode (learn.h), this node at position n & FFh of n >> 8 nodes;
              'K' the ring's light, n = 1 lit (as a car's slave), 0 dark (power.h);
              'C' n bytes of a telegram to us: source (16 bits, high first), FBlock, InstID, FktID (16 bits, high
                  first), OPType with the TelID in the high nibble, length, data - to the amplifier, or in the read
                  mode to it;
              'S' n ring frames (n <= CHUNK), each sink 1 left, right, sinks 2, 3, 4, 16-bit, through the choice of
                  sink 1's music (music.c) and the sound engine;
              'Q' the end
     from us: on 'P' and 'S' [type, playing, underruns (16 bits), n (32 bits), fill ms, correction ppm (floats)], then
              n frames, 32-bit left, right. The type is 'O', or after a request to the virtual phone 'p' pause, 'g'
              play: the script, being the phone, stops or restarts its stream. On 'S' playing means sink 1 plays the
              phone. 'M' carries a telegram out (after 'W'): target (16 bits), FBlock, InstID, FktID (16 bits),
              OPType with the TelID in the high nibble, length, data */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "amp.h"
#include "dsp.h"
#include "learn.h"
#include "music.h"
#include "phone.h"
#include "power.h"
#include "vphone.h"

static const char *TAG = "vphone";

#define HOST    "10.0.2.2"
#define PORT    5555
#define CHUNK   480

typedef struct __attribute__((packed)) { uint8_t type, pad[3]; uint32_t n; } msg_in_t;
typedef struct __attribute__((packed)) {
    uint8_t type, playing;
    uint16_t underruns;
    uint32_t n;
    float fill_ms, corr_ppm;
} msg_out_t;

static bool io(int s, void *b, int n, bool rx)
{
    for (uint8_t *p = b; n > 0;) {
        int k = rx ? recv(s, p, n, 0) : send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

static int s_sock = -1;                 /* the script; written to by this task and by the read mode's */
static SemaphoreHandle_t s_wlock;
static volatile bool s_wire;            /* the telegrams out go to the script ('W') */

static bool out(const void *a, int na, const void *b, int nb)
{
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    bool ok = s_sock >= 0 && io(s_sock, (void *)a, na, false) && (!nb || io(s_sock, (void *)b, nb, false));
    xSemaphoreGive(s_wlock);
    return ok;
}

/* the amplifier's and the read mode's telegrams: to the script as 'M' once it asked for them, else dropped - no
   ring to carry them */
static bool wire(const most_msg_t *m)
{
    if (!s_wire) return true;
    uint8_t t[8 + MOST_DATA] = { m->dst >> 8, m->dst & 0xFF, m->fblock, m->inst, m->fkt >> 8, m->fkt & 0xFF,
                                 (uint8_t)(m->op | m->tel_id << 4), m->len };
    memcpy(t + 8, m->data, m->len);
    msg_out_t o = { 'M', 0, 0, (uint32_t)(8 + m->len), 0, 0 };
    return out(&o, sizeof o, t, 8 + m->len);
}

static uint8_t s_event;                 /* a request to the virtual phone, told with the next answer */

static void remote(phone_cmd_t cmd) { __atomic_store_n(&s_event, cmd == PHONE_PAUSE ? 'p' : 'g', __ATOMIC_RELEASE); }

/* a head unit's telegram, as the OS8104A would hand it to the emulation */
static bool command(int s, uint32_t n)
{
    uint8_t b[8 + MOST_DATA];
    if (n < 8 || n > sizeof b || !io(s, b, (int)n, true)) return false;
    most_msg_t m = { .type = MOST_RX_LOGICAL, .src = (uint16_t)(b[0] << 8 | b[1]), .fblock = b[2], .inst = b[3],
                     .fkt = (uint16_t)(b[4] << 8 | b[5]), .op = b[6] & 0xF, .tel_id = b[6] >> 4, .len = b[7] };
    if (m.len > n - 8) return false;
    memcpy(m.data, b + 8, m.len);
    power_on_msg(&m);
    if (learn_active()) learn_on_msg(&m);
    else amp_on_msg(&m);
    return true;
}

/* ring frames through the choice of sink 1's music and the sound engine */
static bool ring(int s, uint32_t n, int32_t *frames)
{
    static int16_t in[CHUNK * 5];
    static int32_t music[CHUNK * 2], mono[3][CHUNK];
    int k = n < CHUNK ? (int)n : CHUNK;
    if (!io(s, in, 10 * k, true)) return false;
    for (int i = 0; i < k; i++) {
        music[2 * i] = in[5 * i] << 16;
        music[2 * i + 1] = in[5 * i + 1] << 16;
        for (int j = 0; j < 3; j++) mono[j][i] = in[5 * i + 2 + j] << 16;
    }
    const int32_t *m3[3] = { mono[0], mono[1], mono[2] };
    music_block(music, music, k);
    dsp_process(music, m3, frames, k);
    music_stats_t ms;
    phone_stats_t st;
    music_stats(&ms);
    phone_stats(&st);
    uint8_t ev = __atomic_exchange_n(&s_event, 0, __ATOMIC_ACQ_REL);
    msg_out_t o = { ev ? ev : 'O', ms.phone, (uint16_t)st.underruns, (uint32_t)k, st.fill_ms, st.corr_ppm };
    return out(&o, sizeof o, frames, 8 * k);
}

static void serve(int s)
{
    static int16_t in[CHUNK * 2];
    static int32_t frames[CHUNK * 2];
    msg_in_t m;
    while (io(s, &m, sizeof m, true)) {
        switch (m.type) {
        case 'R':
            ESP_LOGI(TAG, "phone stream at %u Hz", (unsigned)m.n);
            phone_start(PHONE_VIRTUAL, (int)m.n);
            break;
        case 'X':
            ESP_LOGI(TAG, "phone stream stopped");
            phone_stop(PHONE_VIRTUAL);
            break;
        case 'I':
            for (uint32_t left = m.n; left;) {
                int k = left < CHUNK ? (int)left : CHUNK;
                if (!io(s, in, 4 * k, true)) return;
                phone_feed(PHONE_VIRTUAL, in, k);
                left -= k;
            }
            break;
        case 'P': {
            int k = m.n < CHUNK ? (int)m.n : CHUNK;
            phone_stats_t st;
            phone_pull(frames, k);
            phone_stats(&st);
            msg_out_t o = { 'O', st.playing, (uint16_t)st.underruns, (uint32_t)k, st.fill_ms, st.corr_ppm };
            if (!out(&o, sizeof o, frames, 8 * k)) return;
            break;
        }
        case 'T': {                     /* the type, or the amplifier read for it; ends the read mode */
            const amp_profile_t *p = amp_profile_of(m.n), *kept = learn_profile(p);
            learn_stop();
            ESP_LOGI(TAG, "amplifier: %s", kept ? kept->name : p->name);
            amp_init(kept ? kept : p, wire);
            break;
        }
        case 'W':
            s_wire = m.n != 0;
            break;
        case 'K':                       /* the ring's light: 1 locked as a car's slave, 0 dark */
            if (m.n) power_ring_slave(true);
            power_light(m.n != 0);
            break;
        case 'L':                       /* the read mode, this node at position n & FFh of n >> 8 */
            learn_start(wire, NULL);
            learn_ring(m.n & 0xFF, (m.n >> 8) & 0xFF);
            break;
        case 'C':
            if (!command(s, m.n)) return;
            break;
        case 'S':
            if (!ring(s, m.n, frames)) return;
            break;
        case 'Q':
            return;
        default:
            ESP_LOGW(TAG, "unknown message %02X", m.type);
            return;
        }
    }
}

static void task(void *arg)
{
    /* the emulated Ethernet comes up in wifi_qemu.c; until it has an address the connect fails and is tried again */
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    inet_pton(AF_INET, HOST, &a.sin_addr);
    ESP_LOGI(TAG, "waiting for tools/vphone.py or vamp.py on %s:%d", HOST, PORT);
    for (;;) {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            ESP_LOGI(TAG, "connected: the amplifier and the sound engine start afresh");
            amp_init(&AMP_PRE_HILINE, wire);
            dsp_init(PHONE_OUT_RATE);
            music_init();
            s_event = 0;
            s_wire = false;
            xSemaphoreTake(s_wlock, portMAX_DELAY);
            s_sock = s;
            xSemaphoreGive(s_wlock);
            serve(s);
            xSemaphoreTake(s_wlock, portMAX_DELAY);
            s_sock = -1;
            xSemaphoreGive(s_wlock);
            phone_stop(PHONE_VIRTUAL);
            ESP_LOGI(TAG, "virtual phone gone");
        }
        if (s >= 0) close(s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vphone_start(void)
{
    s_wlock = xSemaphoreCreateMutex();
    phone_remote_set(PHONE_VIRTUAL, remote);
    xTaskCreate(task, "vphone", 6144, NULL, 5, NULL);
}
