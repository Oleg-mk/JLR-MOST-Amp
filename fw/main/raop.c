/* AirPlay 1 (RAOP) receiver (docs/firmware.md, "AirPlay"). An iPhone finds it through mDNS (_raop._tcp), sets the
   session up over RTSP on TCP 5000 and sends ALAC, 16-bit 44.1 kHz, as RTP over UDP: every packet AES-128-CBC
   encrypted with a key the iPhone wraps in the AirPort Express RSA key (raop_key.c). Packets wait in a reorder buffer
   long enough for a resend request, then are decrypted, decoded (Apple's ALAC, components/alac) and fed to phone.c
   in order; the rate control there follows the iPhone's clock. The newest sender plays: its stream takes over from the
   first packet after RECORD or FLUSH, and a new session ends the one before. The iPhone is paused and resumed through
   its remote control (DACP). Protocol details as shairport-sync implements them */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mdns.h"
#include "alac_glue.h"
#include "phone.h"
#include "raop.h"

static const char *TAG = "airplay";

#define NAME        "Bitline"
#define RTSP_PORT   5000
#define AUDIO_PORT  6000
#define CTRL_PORT   6001
#define TIMING_PORT 6002
#define SLOTS       128         /* reorder buffer: 1 s of packets of 352 frames */
#define SLOT_BYTES  1536        /* an ALAC packet fits the Ethernet MTU */
#define HOLD        24          /* packets held back for a resend to arrive: 190 ms */
#define FRAMES_MAX  4096
#define BODY_MAX    (1 << 20)   /* the body of a request is cut to its buffer and the rest read and dropped, cover art
                                   say; a longer one is no request */

typedef struct { uint16_t seq, len; bool full; uint8_t data[SLOT_BYTES]; } slot_t;

static slot_t *s_slot;                  /* PSRAM */
static int16_t *s_pcm;
static mbedtls_pk_context s_pk;
static uint8_t s_mac[6];                /* the one in the mDNS name: the iPhone checks our signature with it */
static uint8_t s_key[16], s_iv[16];
static uint32_t s_fmtp[11];
static alac_t *s_alac;
static int s_audio = -1, s_ctrl = -1, s_timing = -1;
static struct sockaddr_in s_peer_ctrl;  /* the iPhone's control port: resend requests go there */
static uint16_t s_next, s_top;          /* the next packet to play; the highest seen + 1 */
static bool s_have_top;
static raop_stats_t s_st;
static SemaphoreHandle_t s_lock;        /* the session between the RTSP task and the RTP task */
static bool s_claim;                    /* the next packet played takes the phone's stream over */
static char s_dacp_id[24], s_active[16];    /* the sender's remote control, as its requests name it */
static uint32_t s_peer_ip;              /* the sender's address, network order */
static int s_cur = -1;                  /* the session's connection: a newer sender ends it */
static QueueHandle_t s_conns;           /* connections accepted, for the session task */

static int rng(void *ctx, unsigned char *b, size_t n)
{
    esp_fill_random(b, n);
    return 0;
}

/* ---- audio ---- */

static void request(uint16_t seq, uint16_t count)
{
    uint8_t r[8] = { 0x80, 0xD5, 0x00, 0x01, seq >> 8, seq & 0xFF, count >> 8, count & 0xFF };
    if (s_peer_ctrl.sin_port) sendto(s_ctrl, r, sizeof r, 0, (struct sockaddr *)&s_peer_ctrl, sizeof s_peer_ctrl);
    s_st.requests++;
}

static void play(slot_t *s)
{
    static uint8_t clear[SLOT_BYTES];
    int frames = -1, n = s ? s->len : 0;
    if (s && s_alac) {
        const uint8_t *in = s->data;
        if (s_st.encrypted) {
            /* CBC from the stream's IV in every packet; the tail short of a block travels in clear */
            mbedtls_aes_context aes;
            uint8_t iv[16];
            int whole = n & ~15;
            memcpy(iv, s_iv, sizeof iv);
            mbedtls_aes_init(&aes);
            mbedtls_aes_setkey_dec(&aes, s_key, 128);
            mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, whole, iv, s->data, clear);
            mbedtls_aes_free(&aes);
            memcpy(clear + whole, s->data + whole, n - whole);
            in = clear;
        }
        frames = alac_decode(s_alac, in, n, s_pcm);
    }
    if (frames <= 0) {                  /* lost or broken: a packet's worth of silence keeps the timing */
        frames = (int)s_fmtp[0];
        memset(s_pcm, 0, sizeof *s_pcm * 2 * frames);
        s_st.lost++;
    }
    if (s_claim) {                      /* the first packet after RECORD or FLUSH: this sender plays from now */
        s_claim = false;
        phone_start(PHONE_AIRPLAY, (int)s_fmtp[10]);
        phone_gain(s_st.volume_db);
    }
    phone_feed(PHONE_AIRPLAY, s_pcm, frames);
}

/* plays what is more than HOLD packets behind the newest */
static void drain(void)
{
    while (s_have_top && (int16_t)(s_top - s_next) > HOLD) {
        slot_t *s = &s_slot[s_next % SLOTS];
        play(s->full && s->seq == s_next ? s : NULL);
        s->full = false;
        s_next++;
    }
}

static void put(const uint8_t *p, int n, bool resent)
{
    if (n < 12 || (p[1] & 0x7F) != 0x60 || n - 12 > SLOT_BYTES || !s_st.streaming) return;
    uint16_t seq = p[2] << 8 | p[3];
    int16_t ahead = (int16_t)(seq - s_next);
    if (ahead < 0) {                    /* played already, or given up on */
        s_st.late++;
        return;
    }
    if (ahead >= SLOTS) {               /* far ahead: start again from it */
        for (int i = 0; i < SLOTS; i++) s_slot[i].full = false;
        s_next = seq;
        s_have_top = false;
    }
    slot_t *s = &s_slot[seq % SLOTS];
    memcpy(s->data, p + 12, n - 12);
    s->len = n - 12;
    s->seq = seq;
    s->full = true;
    if (resent) s_st.resent++;
    else s_st.packets++;
    if (!s_have_top) {
        s_top = seq + 1;
        s_have_top = true;
    } else if ((int16_t)(seq - s_top) >= 0) {
        if (seq != s_top) request(s_top, seq - s_top);     /* a gap: ask once for what is missing */
        s_top = seq + 1;
    }
    drain();
}

/* NTP time for timing replies: seconds since 1900 and a 32-bit fraction */
static void ntp_now(uint8_t *p)
{
    int64_t us = esp_log_timestamp() * 1000LL;
    uint32_t sec = (uint32_t)(us / 1000000) + 2208988800u, frac = (uint32_t)(((us % 1000000) << 32) / 1000000);
    for (int i = 0; i < 4; i++) {
        p[i] = sec >> (24 - 8 * i);
        p[4 + i] = frac >> (24 - 8 * i);
    }
}

static int udp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s);
        s = -1;
    }
    return s;
}

static void rtp_task(void *arg)
{
    static uint8_t b[SLOT_BYTES + 16];
    for (;;) {
        fd_set f;
        FD_ZERO(&f);
        FD_SET(s_audio, &f);
        FD_SET(s_ctrl, &f);
        FD_SET(s_timing, &f);
        int top = s_audio > s_ctrl ? s_audio : s_ctrl;
        if (s_timing > top) top = s_timing;
        if (select(top + 1, &f, NULL, NULL, NULL) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        if (FD_ISSET(s_audio, &f)) {
            int n = recvfrom(s_audio, b, sizeof b, 0, NULL, NULL);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            put(b, n, false);
            xSemaphoreGive(s_lock);
        }
        if (FD_ISSET(s_ctrl, &f)) {
            int n = recvfrom(s_ctrl, b, sizeof b, 0, NULL, NULL);
            if (n > 4 && b[1] == 0xD6) {    /* a resent packet: 4 bytes, then the RTP packet itself */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                put(b + 4, n - 4, true);
                xSemaphoreGive(s_lock);
            }                               /* 0xD4, sync: the rate control follows the fill instead */
        }
        if (FD_ISSET(s_timing, &f)) {
            int n = recvfrom(s_timing, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
            if (n >= 32 && (b[1] & 0x7F) == 0x52) {    /* a timing request: answer with our clock */
                memcpy(b + 8, b + 24, 8);
                ntp_now(b + 16);
                memcpy(b + 24, b + 16, 8);
                b[1] = 0xD3;
                sendto(s_timing, b, 32, 0, (struct sockaddr *)&from, fl);
            }
        }
    }
}

/* ---- the sender's remote control (DACP) ---- */

/* An iPhone names its remote control in its requests (DACP-ID, Active-Remote) and serves it over HTTP on the port it
   announces through mDNS as iTunes_Ctrl_<DACP-ID>._dacp._tcp. GET /ctrl-int/1/pause or /play with the Active-Remote
   header does what its own buttons do (shairport-sync's dacp.c works so) */
static int connect_to(uint32_t ip, uint16_t port, int ms)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    struct timeval tv = { ms / 1000, ms % 1000 * 1000 };
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno == EINPROGRESS) {        /* an iPhone gone from the network must not hold the caller long */
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        int err = -1;
        socklen_t el = sizeof err;
        if (select(s + 1, NULL, &w, NULL, &tv) == 1) getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        else err = ETIMEDOUT;
        r = err ? -1 : 0;
        if (err) errno = err;
    }
    if (r < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

static uint16_t dacp_port(const char *id)
{
#if CONFIG_ETH_USE_OPENETH
    return 3689;    /* QEMU's user network carries no multicast for mDNS to ask; tools/vsource.py serves there */
#else
    char inst[48];
    snprintf(inst, sizeof inst, "iTunes_Ctrl_%s", id);
    mdns_result_t *r = NULL;
    uint16_t port = 0;
    if (mdns_query_srv(inst, "_dacp", "_tcp", 2000, &r) == ESP_OK && r) port = r->port;
    mdns_query_results_free(r);
    return port;
#endif
}

/* phone.c's pause and play for the sender of this session */
static void remote(phone_cmd_t cmd)
{
    const char *what = cmd == PHONE_PAUSE ? "pause" : "play";
    char id[sizeof s_dacp_id], active[sizeof s_active], ip_s[16];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool up = s_st.session;
    uint32_t ip = s_peer_ip;
    strlcpy(id, s_dacp_id, sizeof id);
    strlcpy(active, s_active, sizeof active);
    xSemaphoreGive(s_lock);
    if (!up || !id[0] || !active[0]) {
        ESP_LOGW(TAG, "%s: %s", what, up ? "the sender named no remote control" : "no session");
        return;
    }
    uint16_t port = dacp_port(id);
    int s = port ? connect_to(ip, port, 2000) : -1;
    inet_ntoa_r(*(struct in_addr *)&ip, ip_s, sizeof ip_s);
    if (s < 0) {
        ESP_LOGW(TAG, "%s: the sender's remote control at %s:%u not reached (%s)", what, ip_s, port,
                 port ? strerror(errno) : "not found through mDNS");
        return;
    }
    char req[192], resp[48] = "";
    int n = snprintf(req, sizeof req, "GET /ctrl-int/1/%s HTTP/1.1\r\nHost: %s:%u\r\nActive-Remote: %s\r\n"
                     "Connection: close\r\n\r\n", what, ip_s, port, active);
    send(s, req, n, 0);
    int k = recv(s, resp, sizeof resp - 1, 0);
    close(s);
    resp[k > 0 ? k : 0] = 0;
    resp[strcspn(resp, "\r\n")] = 0;
    ESP_LOGI(TAG, "%s sent to the sender: %s", what, resp[0] ? resp : "no answer");
}

/* ---- RTSP ---- */

static bool header(const char *head, const char *name, char *v, size_t n)
{
    size_t k = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, k) || p[2 + k] != ':') continue;
        const char *a = p + 3 + k, *e = strstr(a, "\r\n");
        while (*a == ' ') a++;
        size_t len = e ? (size_t)(e - a) : strlen(a);
        if (len >= n) len = n - 1;
        memcpy(v, a, len);
        v[len] = 0;
        return true;
    }
    return false;
}

/* key=value in a ';'-separated list, as Transport and RTP-Info carry them */
static long param(const char *list, const char *key)
{
    const char *p = strstr(list, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static int b64dec(const char *in, uint8_t *out, size_t n)
{
    char padded[512];
    size_t len = strlen(in), k;
    if (len + 3 >= sizeof padded) return -1;
    memcpy(padded, in, len);
    while (len % 4) padded[len++] = '=';     /* iPhones leave the padding off */
    return mbedtls_base64_decode(out, n, &k, (const uint8_t *)padded, len) ? -1 : (int)k;
}

/* Apple-Response: the challenge, our address and MAC, zero-padded to 32 bytes, as a raw PKCS#1 v1.5 signature */
static bool answer(const char *challenge, int c, char *out, size_t n)
{
    uint8_t buf[32] = { 0 }, sig[256];
    if (b64dec(challenge, buf, 16) != 16) return false;
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    getsockname(c, (struct sockaddr *)&me, &ml);
    memcpy(buf + 16, &me.sin_addr.s_addr, 4);
    memcpy(buf + 20, s_mac, 6);
    size_t len, k;
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(s_pk), MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    if (mbedtls_pk_sign(&s_pk, MBEDTLS_MD_NONE, buf, sizeof buf, sig, sizeof sig, &len, rng, NULL) ||
        mbedtls_base64_encode((uint8_t *)out, n, &k, sig, len))
        return false;
    while (k && out[k - 1] == '=') out[--k] = 0;
    return true;
}

/* the stream's parameters into the session, under s_lock and only once everything is parsed and made: a refused
   ANNOUNCE leaves the stream before as it was, and the RTP task never meets a decoder of one frame with a buffer of
   another */
static bool announce_sdp(const char *sdp)
{
    char v[400];
    const char *p;
    uint8_t wrapped[256], key[16] = { 0 }, iv[16] = { 0 };
    uint32_t f[11];
    bool enc = false;
    if ((p = strstr(sdp, "a=fmtp:")) == NULL ||
        sscanf(p, "a=fmtp:%*d %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", (unsigned long *)&f[0],
               (unsigned long *)&f[1], (unsigned long *)&f[2], (unsigned long *)&f[3], (unsigned long *)&f[4],
               (unsigned long *)&f[5], (unsigned long *)&f[6], (unsigned long *)&f[7], (unsigned long *)&f[8],
               (unsigned long *)&f[9], (unsigned long *)&f[10]) != 11 || f[0] < ALAC_FRAME_MIN || f[0] > FRAMES_MAX)
        return false;
    if ((p = strstr(sdp, "a=rsaaeskey:")) != NULL && sscanf(p, "a=rsaaeskey:%399[^\r\n]", v) == 1) {
        int n = b64dec(v, wrapped, sizeof wrapped);
        size_t k = 0;
        mbedtls_rsa_set_padding(mbedtls_pk_rsa(s_pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        if (n != 256 || mbedtls_rsa_pkcs1_decrypt(mbedtls_pk_rsa(s_pk), rng, NULL, &k, wrapped, key, sizeof key) ||
            k != 16 || (p = strstr(sdp, "a=aesiv:")) == NULL || sscanf(p, "a=aesiv:%399[^\r\n]", v) != 1 ||
            b64dec(v, iv, sizeof iv) != 16)
            return false;
        enc = true;
    }
    alac_t *a = alac_new(f);
    int16_t *pcm = malloc(sizeof *pcm * 2 * f[0]);
    if (!a || !pcm) {
        alac_free(a);
        free(pcm);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    alac_free(s_alac);
    free(s_pcm);
    s_alac = a;
    s_pcm = pcm;
    memcpy(s_fmtp, f, sizeof s_fmtp);
    memcpy(s_key, key, sizeof s_key);
    memcpy(s_iv, iv, sizeof s_iv);
    s_st.encrypted = enc;
    xSemaphoreGive(s_lock);
    return true;
}

static void respond(int c, const char *cseq, const char *status, const char *extra)
{
    char r[1024];
    int n = snprintf(r, sizeof r, "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: AirTunes/105.1\r\n"
                     "Audio-Jack-Status: connected; type=analog\r\n%s\r\n", status, cseq, extra);
    send(c, r, n < (int)sizeof r ? n : (int)sizeof r - 1, 0);
}

static void stop_stream(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.streaming = false;
    s_have_top = false;
    for (int i = 0; i < SLOTS; i++) s_slot[i].full = false;
    xSemaphoreGive(s_lock);
    phone_stop(PHONE_AIRPLAY);
}

typedef struct { char buf[4096]; int len; } rx_t;

static int fill(int c, rx_t *rx)
{
    if (rx->len >= (int)sizeof rx->buf - 1) return -1;
    int k = recv(c, rx->buf + rx->len, sizeof rx->buf - 1 - rx->len, 0);
    if (k > 0) rx->buf[rx->len += k] = 0;
    return k;
}

static void eat(rx_t *rx, int k)
{
    memmove(rx->buf, rx->buf + k, rx->len - k + 1);
    rx->len -= k;
}

/* one request: the head with its headers, and the body cut to bodymax - 1 bytes; false when the connection ends or
   the length is no length (not a number, negative, beyond BODY_MAX): such a sender gets no further */
static bool request_in(int c, rx_t *rx, char *head, int headmax, char *body, int bodymax)
{
    char *end, v[16];
    while ((end = strstr(rx->buf, "\r\n\r\n")) == NULL)
        if (fill(c, rx) <= 0) return false;
    int hl = end + 4 - rx->buf;
    if (hl >= headmax) return false;
    memcpy(head, rx->buf, hl);
    head[hl] = 0;
    eat(rx, hl);
    long clen = 0;
    if (header(head, "Content-Length", v, sizeof v)) {
        char *e;
        clen = strtol(v, &e, 10);
        if (e == v || *e || clen < 0 || clen > BODY_MAX) return false;
    }
    int got = 0;
    while (got < clen) {
        if (!rx->len && fill(c, rx) <= 0) return false;
        int take = rx->len < clen - got ? rx->len : clen - got;
        if (got < bodymax - 1) memcpy(body + got, rx->buf, take < bodymax - 1 - got ? take : bodymax - 1 - got);
        got += take;
        eat(rx, take);
    }
    body[clen < bodymax - 1 ? clen : bodymax - 1] = 0;
    return true;
}

static void serve(int c)
{
    static rx_t rx;
    static char head[2048], body[2048];
    char cseq[16], v[400], extra[768];
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    getpeername(c, (struct sockaddr *)&peer, &pl);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    inet_ntoa_r(peer.sin_addr, s_st.peer, sizeof s_st.peer);
    s_peer_ip = peer.sin_addr.s_addr;
    s_dacp_id[0] = s_active[0] = 0;
    s_st.session = true;
    xSemaphoreGive(s_lock);
    rx.len = 0;
    rx.buf[0] = 0;
    ESP_LOGI(TAG, "session from %s", s_st.peer);
    while (request_in(c, &rx, head, sizeof head, body, sizeof body)) {
        char method[24] = "";
        sscanf(head, "%23s", method);
        if (!header(head, "CSeq", cseq, sizeof cseq)) strcpy(cseq, "0");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (header(head, "DACP-ID", v, sizeof v)) strlcpy(s_dacp_id, v, sizeof s_dacp_id);
        if (header(head, "Active-Remote", v, sizeof v)) strlcpy(s_active, v, sizeof s_active);
        xSemaphoreGive(s_lock);
        extra[0] = 0;
        if (!strcmp(method, "OPTIONS")) {
            int k = snprintf(extra, sizeof extra, "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                             "GET_PARAMETER, SET_PARAMETER\r\n");
            if (header(head, "Apple-Challenge", v, sizeof v)) {
                char sig[400];
                if (answer(v, c, sig, sizeof sig)) snprintf(extra + k, sizeof extra - k, "Apple-Response: %s\r\n", sig);
                else ESP_LOGW(TAG, "challenge not answered");
            }
            respond(c, cseq, "200 OK", extra);
        } else if (!strcmp(method, "ANNOUNCE")) {
            stop_stream();                      /* a stream on ends here, as a sender starting afresh means it: no new
                                                   decoder under the packets; the next one plays from its RECORD */
            bool ok = announce_sdp(body);
            if (ok)
                ESP_LOGI(TAG, "stream accepted: ALAC %lu frames %lu-bit %lu ch %lu Hz", (unsigned long)s_fmtp[0],
                         (unsigned long)s_fmtp[2], (unsigned long)s_fmtp[6], (unsigned long)s_fmtp[10]);
            else ESP_LOGW(TAG, "stream REFUSED: not 16-bit ALAC of %d..%d frames, or a key that does not unwrap",
                          ALAC_FRAME_MIN, FRAMES_MAX);
            respond(c, cseq, ok ? "200 OK" : "415 Unsupported Media Type", "");
        } else if (!strcmp(method, "SETUP")) {
            long cp = header(head, "Transport", v, sizeof v) ? param(v, "control_port=") : -1;
            s_peer_ctrl = peer;
            s_peer_ctrl.sin_port = cp > 0 ? htons((uint16_t)cp) : 0;
            snprintf(extra, sizeof extra, "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;"
                     "timing_port=%d\r\nSession: 1\r\n", AUDIO_PORT, CTRL_PORT, TIMING_PORT);
            respond(c, cseq, "200 OK", extra);
        } else if (!strcmp(method, "RECORD")) {
            long seq = header(head, "RTP-Info", v, sizeof v) ? param(v, "seq=") : 0;
            if (!s_alac || !s_pcm) {            /* no stream accepted in ANNOUNCE */
                respond(c, cseq, "455 Method Not Valid in This State", "");
                continue;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < SLOTS; i++) s_slot[i].full = false;
            s_next = (uint16_t)seq;
            s_have_top = false;
            s_st.streaming = true;
            s_claim = true;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "playing from packet %ld%s", seq, s_st.encrypted ? ", encrypted" : "");
            respond(c, cseq, "200 OK", "Audio-Latency: 2205\r\n");
        } else if (!strcmp(method, "FLUSH")) {
            /* a pause or a seek: the stream ends here and plays again from the next packet */
            long seq = header(head, "RTP-Info", v, sizeof v) ? param(v, "seq=") : -1;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < SLOTS; i++) s_slot[i].full = false;
            if (seq >= 0) s_next = (uint16_t)seq;
            s_have_top = false;
            s_claim = true;
            xSemaphoreGive(s_lock);
            phone_stop(PHONE_AIRPLAY);
            respond(c, cseq, "200 OK", "");
        } else if (!strcmp(method, "SET_PARAMETER")) {
            char *p = strstr(body, "volume:");
            float vol = p ? strtof(p + 7, NULL) : NAN;
            if (header(head, "Content-Type", v, sizeof v) && !strncmp(v, "text/parameters", 15) && isfinite(vol)) {
                s_st.volume_db = vol > 0 ? 0 : vol < -144 ? -144 : vol;     /* -30..0 dB, -144 mute: nothing else */
                if (phone_owner(NULL) == PHONE_AIRPLAY) phone_gain(s_st.volume_db);    /* not another phone's */
                ESP_LOGI(TAG, "volume %.1f", s_st.volume_db);
            }
            respond(c, cseq, "200 OK", "");
        } else if (!strcmp(method, "GET_PARAMETER")) {
            snprintf(extra, sizeof extra, "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\nvolume: %.6f\r\n",
                     snprintf(NULL, 0, "volume: %.6f\r\n", s_st.volume_db), s_st.volume_db);
            char r[256];
            int n = snprintf(r, sizeof r, "RTSP/1.0 200 OK\r\nCSeq: %s\r\nServer: AirTunes/105.1\r\n%s", cseq, extra);
            send(c, r, n, 0);
        } else if (!strcmp(method, "TEARDOWN")) {
            stop_stream();
            respond(c, cseq, "200 OK", "Connection: close\r\n");
            break;
        } else {
            respond(c, cseq, "200 OK", "");
        }
    }
    stop_stream();
    s_st.session = false;
    ESP_LOGI(TAG, "session from %s over: packets %lu, resent %lu, lost %lu, late %lu, resend requests %lu", s_st.peer,
             (unsigned long)s_st.packets, (unsigned long)s_st.resent, (unsigned long)s_st.lost,
             (unsigned long)s_st.late, (unsigned long)s_st.requests);
}

/* a newer sender takes over: the session before ends, and its iPhone is paused - unless it is the same one coming
   again */
static void accept_task(void *arg)
{
    int l = (int)(intptr_t)arg, one = 1;
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int c = accept(l, (struct sockaddr *)&from, &fl);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool busy = s_cur >= 0, same = s_peer_ip == from.sin_addr.s_addr;
        xSemaphoreGive(s_lock);
        if (busy) {
            ESP_LOGI(TAG, "a newer sender: the session from %s ends", s_st.peer);
            if (!same) remote(PHONE_PAUSE);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_cur >= 0) shutdown(s_cur, SHUT_RDWR);
            xSemaphoreGive(s_lock);
        }
        if (xQueueSend(s_conns, &c, 0) != pdTRUE) close(c);
    }
}

static void rtsp_task(void *arg)
{
    int l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RTSP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) < 0 || listen(l, 2) < 0) {
        ESP_LOGE(TAG, "RTSP: no socket");
        vTaskDelete(NULL);
    }
    xTaskCreate(accept_task, "rtsp_accept", 4096, (void *)(intptr_t)l, 5, NULL);
    for (;;) {
        int c;
        if (xQueueReceive(s_conns, &c, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cur = c;
        xSemaphoreGive(s_lock);
        serve(c);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cur = -1;
        xSemaphoreGive(s_lock);
        close(c);
    }
}

void raop_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_conns = xQueueCreate(2, sizeof(int));
    phone_remote_set(PHONE_AIRPLAY, remote);
    s_slot = heap_caps_calloc(SLOTS, sizeof *s_slot, MALLOC_CAP_SPIRAM);
    mbedtls_pk_init(&s_pk);
    if (!s_slot || mbedtls_pk_parse_key(&s_pk, (const uint8_t *)RAOP_KEY, strlen(RAOP_KEY) + 1, NULL, 0, rng, NULL)) {
        ESP_LOGE(TAG, "no memory or no key: AirPlay off");
        return;
    }
    s_audio = udp(AUDIO_PORT);
    s_ctrl = udp(CTRL_PORT);
    s_timing = udp(TIMING_PORT);
    if (s_audio < 0 || s_ctrl < 0 || s_timing < 0) {
        ESP_LOGE(TAG, "no UDP sockets: AirPlay off");
        return;
    }
#if CONFIG_ETH_USE_OPENETH
    esp_read_mac(s_mac, ESP_MAC_ETH);
#else
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
#endif
    char inst[48];
    snprintf(inst, sizeof inst, "%02X%02X%02X%02X%02X%02X@%s", s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4],
             s_mac[5], NAME);
    /* what an AirPlay 1 receiver of 16-bit 44.1 kHz ALAC announces: no password, RSA-wrapped or clear streams */
    mdns_txt_item_t txt[] = {
        { "txtvers", "1" }, { "ch", "2" }, { "cn", "0,1" }, { "et", "0,1" }, { "ek", "1" }, { "sv", "false" },
        { "sm", "false" }, { "ss", "16" }, { "sr", "44100" }, { "tp", "UDP" }, { "vn", "3" }, { "pw", "false" },
    };
    if (mdns_init() != ESP_OK || mdns_hostname_set("bitline") != ESP_OK || mdns_instance_name_set(NAME) != ESP_OK ||
        mdns_service_add(inst, "_raop", "_tcp", RTSP_PORT, txt, sizeof txt / sizeof txt[0]) != ESP_OK)
        ESP_LOGE(TAG, "mDNS did not start: iPhones will not see the receiver");
    xTaskCreate(rtsp_task, "rtsp", 8192, NULL, 5, NULL);
    xTaskCreate(rtp_task, "rtp", 6144, NULL, 6, NULL);
    ESP_LOGI(TAG, "receiver '%s' on RTSP %d", inst, RTSP_PORT);
}

void raop_stats(raop_stats_t *st) { *st = s_st; }
