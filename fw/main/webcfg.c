/* see webcfg.h. Served on every interface (port 80); the DNS answers only while the setup access point is up */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"
#include "lwip/sockets.h"
#include "dsp.h"
#include "ota.h"
#include "wcred.h"
#include "webcfg.h"
#include "wifi.h"

static const char *TAG = "web";

static const char PAGE[] =
    "<!doctype html><html lang='uk'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Bitline</title><style>"
    "body{font:16px system-ui,sans-serif;margin:0 auto;max-width:28em;padding:1em;background:#111;color:#eee}"
    "h1{font-size:1.25em;font-weight:600}label{display:block;margin:1em 0 .3em}"
    "input,button{width:100%;box-sizing:border-box;font:inherit;padding:.65em;border-radius:.5em;"
    "border:1px solid #444;background:#1c1c1c;color:#eee}"
    "button{margin-top:.8em;border:0;background:#2d7d46;color:#fff}button.s{background:#333}"
    "#st{padding:.7em;border-radius:.5em;background:#1c1c1c;line-height:1.4}"
    "small{color:#999;display:block;margin-top:.6em;line-height:1.4}hr{border:0;border-top:1px solid #333;margin:1.5em 0}"
    "</style></head><body><h1>Bitline: Wi-Fi</h1><div id='st'>…</div>"
    "<form method='post' action='/save'><label for='ssid'>Точка доступу телефона</label>"
    "<input id='ssid' name='ssid' list='nets' maxlength='32' autocomplete='off' required><datalist id='nets'></datalist>"
    "<button type='button' class='s' onclick='scan(this)'>Знайти мережі поруч</button>"
    "<label for='pass'>Пароль</label><input id='pass' name='pass' type='password' maxlength='63'>"
    "<label for='pw'>Пароль точки налаштування</label><input id='pw' name='pw' type='password' required>"
    "<button>Зберегти й підключитися</button>"
    "<small>Лише 2.4 ГГц. На iPhone: Параметри → Режим модема → увімкніть «Максимальна сумісність».</small></form><hr>"
    "<form method='post' action='/appass'><label for='apold'>Теперішній пароль точки налаштування «" WIFI_AP_SSID
    "»</label><input id='apold' name='pw' type='password' required>"
    "<label for='ap'>Новий пароль</label>"
    "<input id='ap' name='appass' type='password' minlength='8' maxlength='63' required>"
    "<button class='s'>Змінити пароль</button></form><hr>"
    "<form id='ota'><label for='fw'>Оновлення прошивки, зараз <span id='ver'>…</span></label>"
    "<input id='fw' type='file' accept='.bin' required>"
    "<label for='otp'>Пароль точки налаштування</label><input id='otp' type='password' required>"
    "<button class='s'>Оновити</button><small id='op'>Файл bitline.bin зі збірки. Нова прошивка запускається на "
    "пробу: якщо після перезапуску не підніме мережу й цю сторінку, пристрій сам повернеться до теперішньої."
    "</small></form><script>"
    "function st(){fetch('/state').then(r=>r.json()).then(s=>{document.getElementById('st').textContent=s.text;"
    "document.getElementById('ver').textContent=s.ver}).catch(()=>{})}"
    "document.getElementById('ota').onsubmit=e=>{e.preventDefault();var op=document.getElementById('op'),"
    "x=new XMLHttpRequest();x.open('POST','/ota');x.setRequestHeader('X-Setup-Password',"
    "document.getElementById('otp').value);x.upload.onprogress=p=>{op.textContent='Передано '+"
    "Math.round(100*p.loaded/p.total)+'%'};x.onload=()=>{op.textContent=x.responseText;if(x.status==200)"
    "setTimeout(()=>location.reload(),20000)};x.onerror=()=>{op.textContent='Зв’язок обірвався'};"
    "x.send(document.getElementById('fw').files[0])};"
    "function scan(b){b.disabled=true;b.textContent='Шукаю…';fetch('/scan').then(r=>r.json()).then(a=>{"
    "var d=document.getElementById('nets');d.innerHTML='';a.forEach(n=>{var o=document.createElement('option');"
    "o.value=n.ssid;o.label=n.rssi+' дБм'+(n.open?', відкрита':'');d.appendChild(o)});"
    "b.textContent=a.length?'Знайдено мереж: '+a.length+'. Оберіть у полі вище':'Нічого не знайдено'})"
    ".catch(()=>{b.textContent='Не вдалося'}).finally(()=>{b.disabled=false})}"
    "st();setInterval(st,2000)</script></body></html>";

/* s as a JSON string, quotes included; d has room for 6 bytes per character + 3 */
static int jstr(char *d, const char *s)
{
    int k = 0;
    d[k++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { d[k++] = '\\'; d[k++] = (char)c; }
        else if (c < 0x20) k += sprintf(d + k, "\\u%04x", c);
        else d[k++] = (char)c;
    }
    d[k++] = '"';
    d[k] = 0;
    return k;
}

static void html_esc(char *d, size_t n, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < n; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (r) {
            strcpy(d + k, r);
            k += strlen(r);
        } else {
            d[k++] = *s;
        }
    }
    d[k] = 0;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    char html[1024];
    snprintf(html, sizeof html, "<!doctype html><html lang='uk'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<meta http-equiv='refresh' content='4;url=/'><title>Bitline</title><style>body{font:16px system-ui,"
             "sans-serif;margin:0 auto;max-width:28em;padding:1em;background:#111;color:#eee}</style></head>"
             "<body><p>%s</p></body></html>", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, html);
}

static const char *why(uint8_t r)
{
    switch (r) {
    case 0:
        return NULL;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "мережу не знайдено: точка доступу ввімкнена й на 2.4 ГГц?";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "пароль не підійшов";
    default:
        return "з'єднання обірвалося";
    }
}

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, sizeof PAGE - 1);
}

static esp_err_t h_state(httpd_req_t *req)
{
    wifi_state_t st;
    wifi_state(&st);
    char text[240], js[8 + 6 * sizeof text];
    const char *w = why(st.reason);
    if (st.emulated)
        snprintf(text, sizeof text, "QEMU: Wi-Fi тут не емулюється, працює емульований Ethernet, IP %s",
                 st.ip[0] ? st.ip : "ще немає");
    else if (st.link == WIFI_ONLINE)
        snprintf(text, sizeof text, "Підключено до «%s», IP %s, сигнал %d дБм", st.ssid, st.ip, st.rssi);
    else if (st.link == WIFI_JOINING)
        snprintf(text, sizeof text, "Підключаюся до «%s»%s%s", st.ssid, w ? ": " : "…", w ? w : "");
    else
        snprintf(text, sizeof text, "Точку доступу телефона ще не задано");
    if (st.setup_ap) strlcat(text, ". Точка налаштування ввімкнена", sizeof text);
    int k = sprintf(js, "{\"text\":");
    k += jstr(js + k, text);
    k += sprintf(js + k, ",\"ver\":");
    k += jstr(js + k, ota_version());
    strcpy(js + k, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, js);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    enum { MAX = 16, ROOM = 4096 };
    wifi_seen_t *v = calloc(MAX, sizeof *v);
    char *js = malloc(ROOM);
    int n = v && js ? wifi_scan(v, MAX) : 0, k = 0;
    if (js) {
        js[k++] = '[';
        for (int i = 0; i < n && k < ROOM - 260; i++) {
            k += sprintf(js + k, "%s{\"ssid\":", i ? "," : "");
            k += jstr(js + k, v[i].ssid);
            k += sprintf(js + k, ",\"rssi\":%d,\"open\":%s}", v[i].rssi, v[i].open ? "true" : "false");
        }
        strcpy(js + k, "]");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, js ? js : "[]");
    free(v);
    free(js);
    return e;
}

/* the form's body, at most n - 1 bytes */
static bool body(httpd_req_t *req, char *b, size_t n)
{
    if (req->content_len >= n) return false;
    for (size_t got = 0; got < req->content_len;) {
        int k = httpd_req_recv(req, b + got, req->content_len - got);
        if (k <= 0) return false;
        got += k;
    }
    b[req->content_len] = 0;
    return true;
}

/* one field of key=value&...: '+' is a space, %XX a byte */
static bool field(const char *b, const char *key, char *v, size_t n)
{
    char raw[400];      /* a name of 32 Cyrillic characters takes 288 bytes encoded */
    if (httpd_query_key_value(b, key, raw, sizeof raw) != ESP_OK) return false;
    size_t k = 0;
    for (const char *p = raw; *p && k + 1 < n; p++) {
        if (*p == '+') {
            v[k++] = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            v[k++] = (char)strtol(h, NULL, 16);
            p += 2;
        } else {
            v[k++] = *p;
        }
    }
    v[k] = 0;
    return true;
}

/* every change of state carries the setup access point's password in its form (pw): whoever is on the hotspot or on
   the point can see the page, and a page opened from elsewhere in the browser cannot post it without the password */
static bool authorized(const char *b)
{
    char pw[65], want[65];
    wcred_ap_pass(want, sizeof want);
    return field(b, "pw", pw, sizeof pw) && !strcmp(pw, want);
}

#define WRONG_PW "Пароль точки налаштування не той."

static esp_err_t h_save(httpd_req_t *req)
{
    char b[1024], ssid[33], pass[65], esc[200], msg[512];
    if (!body(req, b, sizeof b)) return reply(req, "400 Bad Request", "Форма задовга.");
    if (!authorized(b)) return reply(req, "403 Forbidden", WRONG_PW);
    if (!field(b, "ssid", ssid, sizeof ssid) || !ssid[0])
        return reply(req, "400 Bad Request", "Назва мережі порожня або задовга.");
    if (!field(b, "pass", pass, sizeof pass)) pass[0] = 0;
    size_t pl = strlen(pass);
    if (pl && (pl < 8 || pl > 63))
        return reply(req, "400 Bad Request", "Пароль має бути від 8 до 63 символів, для відкритої мережі — порожній.");
    if (wcred_set(ssid, pass) != ESP_OK) return reply(req, "500 Internal Server Error", "Не вдалося зберегти.");
    ESP_LOGI(TAG, "hotspot '%s' saved on the page", ssid);
    html_esc(esc, sizeof esc, ssid);
    snprintf(msg, sizeof msg, "Збережено. Підключаюся до «%s»… Якщо телефон був на точці налаштування, вона "
             "вимкнеться, щойно пристрій підключиться.", esc);
    esp_err_t e = reply(req, "200 OK", msg);
    wifi_apply();
    return e;
}

static esp_err_t h_appass(httpd_req_t *req)
{
    char b[512], pass[65];
    if (!body(req, b, sizeof b)) return reply(req, "400 Bad Request", "Форма задовга.");
    if (!authorized(b)) return reply(req, "403 Forbidden", WRONG_PW);
    if (!field(b, "appass", pass, sizeof pass) || wcred_set_ap_pass(pass) != ESP_OK)
        return reply(req, "400 Bad Request", "Пароль має бути від 8 до 63 символів.");
    ESP_LOGI(TAG, "setup access point password changed on the page");
    return reply(req, "200 OK", "Пароль точки налаштування змінено. Він діятиме, коли точка ввімкнеться наступного разу.");
}

static void restart(void *arg) { esp_restart(); }

static esp_err_t text(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

/* a new image: the setup access point's password in X-Setup-Password - not everyone on the hotspot may reflash the
   device - and the image as the body, written as it comes; a second later the device restarts into it on trial */
static esp_err_t h_ota(httpd_req_t *req)
{
    char pass[65] = "", want[65];
    wcred_ap_pass(want, sizeof want);
    if (httpd_req_get_hdr_value_str(req, "X-Setup-Password", pass, sizeof pass) != ESP_OK || strcmp(pass, want))
        return text(req, "403 Forbidden", WRONG_PW);
    ota_t *o = req->content_len > 65536 ? ota_begin(req->content_len) : NULL;
    if (!o) return text(req, "400 Bad Request", "Файл не схожий на прошивку або не вміщається.");
    /* silence first: every sector erased stops the sound task with the flash cache for tens of ms, and the DMA would
       send zeros in pieces - a chopped sound for the whole update. The gate opens again only if the update fails;
       a successful one restarts the device */
    dsp_gate(false, 50);
    vTaskDelay(pdMS_TO_TICKS(80));
    char *b = malloc(4096);
    esp_err_t e = b ? ESP_OK : ESP_ERR_NO_MEM;
    for (size_t left = req->content_len, waits = 0; e == ESP_OK && left;) {
        int k = httpd_req_recv(req, b, left < 4096 ? left : 4096);
        if (k == HTTPD_SOCK_ERR_TIMEOUT && ++waits < 4) continue;
        if (k <= 0) {
            e = ESP_FAIL;
            break;
        }
        e = ota_write(o, b, k);
        left -= k;
    }
    free(b);
    esp_err_t f = ota_finish(o, e == ESP_OK);
    if (e == ESP_OK) e = f;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "update refused: %s", esp_err_to_name(e));
        dsp_gate(true, 50);
        return text(req, "400 Bad Request", e == ESP_ERR_INVALID_VERSION ? "Це прошивка не для цього пристрою."
                                                                         : "Файл пошкоджений або передача обірвалася.");
    }
    text(req, "200 OK", "Записано. Пристрій перезапускається з новою прошивкою на пробу, сторінка оновиться сама.");
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = restart, .name = "ota_restart" };
    if (t || esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 1000000);
    return ESP_OK;
}

/* on the setup access point every unknown address leads to the page: that is how a phone finds it */
static esp_err_t h_missing(httpd_req_t *req, httpd_err_code_t err)
{
    wifi_state_t st;
    wifi_state(&st);
    if (!st.setup_ap) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

/* every A query gets the setup access point's address, every other one an empty answer; only while it is up */
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "DNS: no socket");
        vTaskDelete(NULL);
    }
    static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
    static uint8_t q[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q - sizeof ans, 0, (struct sockaddr *)&from, &fl);
        wifi_state_t st;
        wifi_state(&st);
        if (n < 12 || !st.setup_ap || (q[2] & 0x80) || q[4] || q[5] != 1) continue;     /* one question, a query */
        int p = 12;
        while (p < n && q[p] && !(q[p] & 0xC0)) p += q[p] + 1;
        if (p + 5 > n || q[p]) continue;
        uint16_t type = q[p + 1] << 8 | q[p + 2];
        p += 5;                                 /* the name's end, type, class */
        q[2] = 0x84 | (q[2] & 0x01);            /* an authoritative response, recursion flag kept */
        q[3] = 0;
        q[6] = 0;
        q[7] = type == 1;
        memset(q + 8, 0, 4);
        if (type == 1) {
            memcpy(q + p, ans, sizeof ans);
            p += sizeof ans;
        }
        sendto(s, q, p, 0, (struct sockaddr *)&from, fl);
    }
}

bool webcfg_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    /* lwIP's 16 sockets (sdkconfig.defaults) also hold AirPlay's three UDP, its RTSP listener and sessions, the
       captive DNS and the remote-control client: the page gets 4 connections, the least recently used one dropped */
    c.max_open_sockets = 4;
    c.lru_purge_enable = true;
    c.stack_size = 8192;        /* the save handler holds a 1 kB form, its fields and the answer page on the stack */
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &c) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server did not start");
        return false;
    }
    const httpd_uri_t u[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_page },
        { .uri = "/state", .method = HTTP_GET, .handler = h_state },
        { .uri = "/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/save", .method = HTTP_POST, .handler = h_save },
        { .uri = "/appass", .method = HTTP_POST, .handler = h_appass },
        { .uri = "/ota", .method = HTTP_POST, .handler = h_ota },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(h, &u[i]);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, h_missing);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "setup page on port 80");
    return true;
}
