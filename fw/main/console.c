/* bench console on UART0 (USB-C J10 through the CH340C): drive HOLD, REM and the LED, read the switch and the ring clocks, run the OS8104A */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_timer.h"
#include "amp.h"
#include "audio.h"
#include "board.h"
#include "console.h"
#include "dsp.h"
#include "music.h"
#include "os8104.h"
#include "learn.h"
#include "ota.h"
#include "power.h"
#include "pcm5122.h"
#include "phone.h"
#include "raop.h"
#include "wcred.h"
#include "wifi.h"

/* 1|on -> 1, 0|off -> 0, anything else -> -1: a bare 'hold' must never switch the supply off */
static int arg_level(int argc, char **argv)
{
    if (argc != 2) return -1;
    if (!strcmp(argv[1], "1") || !strcmp(argv[1], "on")) return 1;
    if (!strcmp(argv[1], "0") || !strcmp(argv[1], "off")) return 0;
    return -1;
}

static int cmd_hold(int argc, char **argv)
{
    int v = arg_level(argc, argv);
    if (v < 0) { printf("usage: hold 1|0   (HOLD is %d)\n", board_hold_get()); return 1; }
    board_hold(v); printf("HOLD=%d\n", v); return 0;
}

static int cmd_rem(int argc, char **argv)
{
    int v = arg_level(argc, argv);
    if (v < 0) { printf("usage: rem 1|0   (REM is %d)\n", board_rem_get()); return 1; }
    board_rem(v); printf("REM=%d\n", v); return 0;
}

static int cmd_led(int argc, char **argv)
{
    int v = arg_level(argc, argv);
    if (v < 0) { printf("usage: led 1|0\n"); return 1; }
    board_led(v); return 0;
}

static int cmd_sw(int argc, char **argv)
{
    uint8_t s = board_switch();
    printf("SW1 keys closed: 1:%d 2:%d 3:%d 4:%d (type bits %u, read serial %d)\n",
           !!(s & 8), !!(s & 4), !!(s & 2), s & 1, (unsigned)(s >> 1), s & 1);
    return 0;
}

static int cmd_clk(int argc, char **argv)
{
    /* count LRCK edges for ~100 ms by polling: 48 kHz gives 9600 edges. The OS8104A drives FSY only after stage 1
       sets bSDC1.IO; until then the line sits on the pad pull-down and a few edges are noise */
    int64_t t0 = esp_timer_get_time();
    int last = gpio_get_level(PIN_LRCK), edges = 0;
    while (esp_timer_get_time() - t0 < 100000) {
        int v = gpio_get_level(PIN_LRCK);
        edges += v != last;
        last = v;
    }
    if (edges < 1000) printf("LRCK: no clock (%d edges in 100 ms)\n", edges);
    else printf("LRCK edges in 100 ms: %d (~%d Hz)\n", edges, edges * 5);
    return 0;
}

static int most_status(void)
{
    static const uint8_t reg[] = { OS_bXCR, OS_bXSR, OS_bXSR2, OS_bSDC1, OS_bSDC2, OS_bSDC3, OS_bCM1, OS_bCM2, OS_bCM3,
                                   OS_bSBC, OS_bIE, OS_bMSGS, OS_bNPR, OS_bMPR, OS_bNDR, OS_bMDR };
    static const char *name[] = { "XCR", "XSR", "XSR2", "SDC1", "SDC2", "SDC3", "CM1", "CM2", "CM3",
                                  "SBC", "IE", "MSGS", "NPR", "MPR", "NDR", "MDR" };
    uint8_t v[sizeof reg];
    for (size_t i = 0; i < sizeof reg; i++) {
        if (os8104_read(reg[i], &v[i], 1) != ESP_OK) { printf("OS8104A: no answer on I2C\n"); return 1; }
        printf("%s=%02X%s", name[i], v[i], i + 1 < sizeof reg ? " " : "\n");
    }
    printf("%s, %s, PLL %s, RX %s, position %u of %u nodes, delay %u of %u\n", v[0] & XCR_MTR ? "master" : "slave",
           v[0] & XCR_ABY ? "active" : "All-Bypass", v[7] & CM2_LOK ? "unlocked" : "locked",
           v[7] & CM2_NAC ? "active" : "dark", v[12], v[13], v[14], v[15]);
    return 0;
}

/* In a car's ring - a slave start, the auto-start's or 'most slave' - a bench command that restarts the chip or writes
   the ring's channels breaks the ring for every node: a second timing master, a node gone to All-Bypass, test noise
   in channels 0..7. Refused unless the last word is "force" */
static bool car_ring(int argc, char **argv, const char *what)
{
    if (!power_slave() || !strcmp(argv[argc - 1], "force")) return false;
    printf("%s: this node is a slave in a car's ring; add 'force' to do it anyway\n", what);
    return true;
}

static int cmd_most(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "reset")) {
        if (car_ring(argc, argv, "most reset")) return 1;
        esp_err_t e = os8104_reset();
        amp_reset_link();                   /* the chip forgot its routes and allocations: so does the emulation */
        return e != ESP_OK;
    }
    if (!strcmp(c, "master")) {             /* the bench's own ring: its light does not put the board to sleep */
        if (car_ring(argc, argv, "most master")) return 1;
        power_ring_slave(false);
        esp_err_t e = os8104_start_master();
        amp_reset_link();
        return e != ESP_OK;
    }
    /* defaults for the XF X250 amplifier: node 186h (docs/firmware.md); group 22h = its FBlock AudioAmplifier, by the
       usual 300h + FBlock rule, to be checked against the car log in stage 2 */
    if (!strcmp(c, "slave")) {
        uint16_t node = argc > 2 ? strtoul(argv[2], NULL, 16) : 0x186;
        uint8_t group = argc > 3 ? strtoul(argv[3], NULL, 16) : 0x22;
        esp_err_t e = os8104_start_slave(node, group);
        amp_reset_link();                   /* a fresh start in the ring: the head unit connects again */
        if (e != ESP_OK) return 1;
        power_ring_slave(true);             /* a car's ring: the light going for good means sleep */
        return 0;
    }
    if (!strcmp(c, "st")) return most_status();
    if (!strcmp(c, "loop")) {
        if (car_ring(argc, argv, "most loop")) return 1;
        if (os8104_loop_routes(true) == ESP_OK) { printf("routes: SR1 -> ring bytes 0..7 -> SX1, SX3\n"); return 0; }
        printf("OS8104A: no answer on I2C\n");
        return 1;
    }
    if (!strcmp(c, "rd") && argc > 2) {
        uint8_t reg = strtoul(argv[2], NULL, 16), b[64];
        int n = argc > 3 ? atoi(argv[3]) : 1;
        if (n < 1 || n > 64) n = 1;
        if (os8104_read(reg, b, n) != ESP_OK) { printf("OS8104A: no answer on I2C\n"); return 1; }
        for (int i = 0; i < n; i++) {
            if (i % 16 == 0) printf("%s%02X:", i ? "\n" : "", (reg + i) & 0xFF);
            printf(" %02X", b[i]);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(c, "wr") && argc > 3 && argc - 3 <= 64) {
        uint8_t b[64];
        for (int i = 3; i < argc; i++) b[i - 3] = strtoul(argv[i], NULL, 16);
        if (os8104_write(strtoul(argv[2], NULL, 16), b, argc - 3) == ESP_OK) return 0;
        printf("OS8104A: no answer on I2C\n");
        return 1;
    }
    if (!strcmp(c, "log") && argc == 3) {
        int v = arg_level(2, argv + 1);
        if (v < 0) { printf("usage: most log 1|0\n"); return 1; }
        amp_set_log(v);
        return 0;
    }
    /* most send DST FB INST FKT OP [BYTE..]: one message by hand, split into segments when longer than 12 bytes */
    if (!strcmp(c, "send") && argc >= 7 && argc - 7 <= MOST_LONG) {
        most_long_t l = { .dst = strtoul(argv[2], NULL, 16), .fblock = strtoul(argv[3], NULL, 16),
                          .inst = strtoul(argv[4], NULL, 16), .fkt = strtoul(argv[5], NULL, 16) & 0xFFF,
                          .op = strtoul(argv[6], NULL, 16) & 0xF, .len = (uint8_t)(argc - 7) };
        for (int i = 7; i < argc; i++) l.data[i - 7] = strtoul(argv[i], NULL, 16);
        most_msg_t t[8];
        int n = most_split(&l, t, 8);
        if (!n) { printf("too long\n"); return 1; }
        for (int i = 0; i < n; i++) if (!amp_tx(&t[i])) return 1;
        return 0;
    }
    printf("usage: most reset | master | slave [node group] | st | loop | rd REG [N] | wr REG BYTE.. | log 1|0 |\n"
           "       send DST FB INST FKT OP [BYTE..]   (all hex)\n");
    return 1;
}

static int cmd_amp(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "test")) {
        /* the test drives the emulation's state and the sound processing itself, and leaves both reset */
        if (amp_active()) { printf("amp test: the emulation is on; 'amp off' first (the test resets it)\n"); return 1; }
        return amp_selftest() != 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        amp_set_active(!strcmp(argv[1], "on"));
        return 0;
    }
    if (argc == 1 || !strcmp(argv[1], "st")) { amp_show(); return 0; }
    printf("usage: amp [st] | on | off | test\n");
    return 1;
}

static int cmd_phone(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "test")) return phone_selftest() ? 1 : 0;
    static const char *src[] = { "-", "A2DP", "AirPlay", "virtual" }, *hu[] = { "not known yet", "silent", "playing" };
    music_stats_t ms;
    music_stats(&ms);
    printf("sink 1 plays %s; the head unit's music %s (%.1f dBFS)%s%s\n", ms.phone ? "the phone" : "the head unit",
           hu[ms.hu], ms.hu_db, ms.paused ? "; paused for it: " : "", ms.paused ? src[ms.paused] : "");
    phone_stats_t st;
    phone_stats(&st);
    if (!st.rate) {
        printf("phone: no stream\n");
        return 0;
    }
    printf("phone: %s %d Hz, %s, fill %.1f ms, correction %+.1f ppm; frames in %" PRIu32 ", out %" PRIu32
           "; underruns %" PRIu32 ", overruns %" PRIu32 "\n", src[st.src], st.rate, st.playing ? "playing" : "filling",
           st.fill_ms, st.corr_ppm, st.in_frames, st.out_frames, st.underruns, st.overruns);
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    ota_show();
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    power_sleep("asked on the console");
    return 0;
}

static int cmd_learn(int argc, char **argv)
{
    if (argc == 1) { learn_show(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "forget")) { learn_forget(); return 0; }
    printf("usage: learn | learn forget\n");
    return 1;
}

static int cmd_dsp(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "test")) {
        /* it runs on the live sound processing - full levels, tones - and leaves it at the defaults */
        if (amp_active()) { printf("dsp test: the emulation is on; 'amp off' first\n"); return 1; }
        return dsp_selftest() ? 1 : 0;
    }
    printf("usage: dsp test\n");
    return 1;
}

static int cmd_airplay(int argc, char **argv)
{
    raop_stats_t st;
    raop_stats(&st);
    printf("airplay: %s%s%s, volume %.1f; packets %" PRIu32 ", resent %" PRIu32 ", lost %" PRIu32 ", late %" PRIu32
           ", resend requests %" PRIu32 "\n", st.session ? "session from " : "no session", st.session ? st.peer : "",
           st.streaming ? (st.encrypted ? ", streaming encrypted" : ", streaming clear") : "", st.volume_db,
           st.packets, st.resent, st.lost, st.late, st.requests);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "setup")) {
        wifi_setup(true);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "forget")) {
        wcred_forget();
        wifi_apply();
        return 0;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "join")) {
        if (wcred_set(argv[2], argc == 4 ? argv[3] : "") != ESP_OK) return 1;
        wifi_apply();
        return 0;
    }
    if (argc != 1) {
        printf("usage: wifi | wifi join SSID [PASSWORD] | wifi forget | wifi setup\n");
        return 1;
    }
    static const char *link[] = { "no hotspot stored", "joining", "online" };
    wifi_state_t st;
    wifi_state(&st);
    printf("wifi: %s%s%s", link[st.link], st.ssid[0] ? " '" : "", st.ssid);
    if (st.ssid[0]) printf("'");
    if (st.link == WIFI_ONLINE) printf(" as %s, %d dBm", st.ip, st.rssi);
    if (st.reason) printf(", last failure reason %u", st.reason);
    printf("%s\n", st.setup_ap ? "; setup access point " WIFI_AP_SSID " up" : "");
    return 0;
}

static int cmd_tone(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) {
        audio_tone(0, 0);
        return 0;
    }
    float hz = argc > 1 ? atof(argv[1]) : 1000, db = argc > 2 ? atof(argv[2]) : -20;
    audio_tone(hz, db);
    printf("tone %.0f Hz %.1f dBFS on DOUT (PCM5122, HDMI) and SR1\n", hz, db);
    return 0;
}

static float dbfs(uint32_t a) { return a ? 20 * log10f(a / 2147483648.0f) : -INFINITY; }

static int cmd_i2s(int argc, char **argv)
{
    audio_stats_t s;
    audio_stats(&s);
    printf("TX: %" PRIu32 " frames, %" PRIu32 " timeouts\n", s.frames, s.tx_timeouts);
    for (int p = 0; p < 2; p++)
        printf("RX I2S%d <- %s: %" PRIu32 " frames, %" PRIu32 " timeouts, peak L %.1f R %.1f dBFS\n", p,
               p ? "SX3" : "SX1", s.rx_frames[p], s.rx_timeouts[p], dbfs(s.peak[p][0]), dbfs(s.peak[p][1]));
    return 0;
}

static int cmd_dac(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "mute")) {
        int v = arg_level(2, argv + 1);
        if (v < 0) { printf("usage: dac mute 1|0\n"); return 1; }
        return pcm5122_mute(v) != ESP_OK;
    }
    static const char *state[] = { "powerdown", "waiting for the charge pump", "calibrating", "calibrating",
                                   "volume ramp up", "playing", "line output short", "volume ramp down", "standby" };
    static const char *rate[] = { "error", "8 kHz", "16 kHz", "32-48 kHz", "88.2-96 kHz", "176.4-192 kHz", "384 kHz",
                                  "?" };
    static const uint16_t ratio[16] = { 0, 32, 48, 64, 128, 192, 256, 384, 512, 768, 1024, 1152, 1536, 2048, 3072 };
    uint8_t pw, mu, ck, er, rt, am;
    if (pcm5122_read(PCM_POWER, &pw) || pcm5122_read(PCM_MUTE, &mu) || pcm5122_read(PCM_CLKSTAT, &ck) ||
        pcm5122_read(PCM_CLKERR, &er) || pcm5122_read(PCM_RATE, &rt) || pcm5122_read(PCM_AMUTE, &am)) {
        printf("PCM5122: no answer on I2C\n");
        return 1;
    }
    printf("PCM5122: %s, %s; clocks %s (5Eh %02X, 5Fh %02X), fS %s, SCK %uFs; analog out L %s, R %s\n",
           (pw & 0x0F) < 9 ? state[pw & 0x0F] : "?", mu & 0x11 ? "muted" : "not muted",
           !(ck & 0x7F) && !(er & 0x01) ? "ok" : "error", ck, er, rate[(rt >> 4) & 7], ratio[rt & 0x0F],
           am & 0x02 ? "on" : "off", am & 0x01 ? "on" : "off");
    return 0;
}

/* check on [ring|direct]: the LFSR to SR1 and back to SX1 and SX3 through the ring (light from TX back to RX) or
   inside the OS8104A only; DOUT is silent and S/PDIF muted meanwhile, the pattern is full-scale noise */
static int cmd_check(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        bool ring = argc < 3 || strcmp(argv[2], "direct");
        if (car_ring(argc, argv, "check on")) return 1;
        if (os8104_loop_routes(ring) != ESP_OK || os8104_spdif(false) != ESP_OK) {
            printf("OS8104A: no answer on I2C\n");
            return 1;
        }
        audio_check(true);
        printf("pattern on SR1, back to SX1 and SX3 %s; DOUT silent, S/PDIF muted\n",
               ring ? "through the ring" : "inside the OS8104A");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "off")) {
        audio_check(false);
        /* channels 0..7 pass the ring through again, SX1 and SX3 wait for the head unit's sinks */
        esp_err_t e = os8104_loop_off();
        amp_reset_link();
        return e != ESP_OK || os8104_spdif(true) != ESP_OK;
    }
    audio_check_t st[2];
    audio_check_stats(st);
    for (int p = 0; p < 2; p++) {
        printf("I2S%d <- %s: ", p, p ? "SX3" : "SX1");
        if (!st[p].frames)
            printf("pattern not found yet, %" PRIu32 " silent frames, last L %08" PRIX32 " R %08" PRIX32 "\n",
                   st[p].silent, st[p].got_l, st[p].got_r);
        else printf("%" PRIu32 " frames, %" PRIu32 " errors%s\n", st[p].frames, st[p].errors,
                    st[p].errors ? "" : ": bit-exact");
        if (st[p].errors)
            printf("  first error: expected L %08" PRIX32 " R %08" PRIX32 ", got L %08" PRIX32 " R %08" PRIX32 "\n",
                   st[p].exp, ~st[p].exp, st[p].got_l, st[p].got_r);
    }
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "amp>";
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    /* the registry keeps these pointers: the strings are literals, so a stack array is fine */
    const esp_console_cmd_t cmds[] = {
        { .command = "hold", .help = "hold 1|0: keep the supply on without light",             .func = cmd_hold },
        { .command = "rem",  .help = "rem 1|0: REM +12 V",                                     .func = cmd_rem },
        { .command = "led",  .help = "led 1|0",                                                .func = cmd_led },
        { .command = "sw",   .help = "read SW1",                                               .func = cmd_sw },
        { .command = "clk",  .help = "measure LRCK from OS8104A (driven only after bSDC1.IO)", .func = cmd_clk },
        { .command = "most", .help = "OS8104A: reset|master|slave [node group]|st|loop|rd REG [N]|wr REG BYTE..|"
          "log 1|0|send DST FB INST FKT OP [BYTE..]", .func = cmd_most },
        { .command = "amp",  .help = "amp [st] | on | off | test: amplifier emulation (stage 3 skeleton)",
          .func = cmd_amp },
        { .command = "tone", .help = "tone [Hz] [dBFS] | tone off: test tone on DOUT and SR1", .func = cmd_tone },
        { .command = "phone",
          .help = "phone | phone test: sink 1's music, the phone's stream and rate control; converter test",
          .func = cmd_phone },
        { .command = "ota", .help = "ota: this image's version, partition and trial state", .func = cmd_ota },
        { .command = "learn", .help = "learn | learn forget: the stock amplifier read with SW1 key 4 (read mode)",
          .func = cmd_learn },
        { .command = "sleep", .help = "sleep: the shutdown sequence now - fade, REM off, DAC muted, HOLD released",
          .func = cmd_sleep },
        { .command = "dsp", .help = "dsp test: the sound processing's selftest (levels, mute, tone, sinks)",
          .func = cmd_dsp },
        { .command = "airplay", .help = "airplay: the AirPlay session, packets, losses", .func = cmd_airplay },
        { .command = "wifi", .help = "wifi | wifi join SSID [PASSWORD] | wifi forget | wifi setup: the phone's hotspot",
          .func = cmd_wifi },
        { .command = "i2s",  .help = "I2S frames, timeouts and RX peaks since the last call",  .func = cmd_i2s },
        { .command = "dac",  .help = "dac | dac mute 1|0: PCM5122 state, mute",                .func = cmd_dac },
        { .command = "check", .help = "check on [ring|direct] | off | check: bit-exact SR1 -> SX1, SX3",
          .func = cmd_check },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
