/* The phone over Bluetooth (docs/firmware.md, "Телефон"): Classic only, A2DP sink with the stack's own SBC decoder
   (ESP-IDF 5.5 API, as its a2dp_sink_stream example uses it). The decoded 16-bit stereo goes to phone.c, the stream's
   rate comes with the codec configuration; the stream plays from its start to its suspend. AVRCP as a controller
   pauses and resumes the phone with its own Pause and Play keys ("Перемикання джерел"); as a target it takes the
   phone's volume (absolute volume, AVRCP 1.4). Pairing: Secure Simple
   Pairing without a display or keys ("Just Works"), so the device is visible to new phones only for DISCOVER_S after
   the start, and then only while no phone is connected; a paired phone connects at any time. Not run yet: needs the
   board */
#include <stdint.h>
#include <string.h>
#include "esp_a2dp_api.h"
#include "esp_attr.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "a2dp.h"
#include "phone.h"

static const char *TAG = "a2dp";

#define NAME        "Bitline"
#define DISCOVER_S  600     /* visible to new phones this long after the start: a car's module is not for anyone passing
                               to pair with, and pairing without a code cannot ask */

static int s_rate = 44100;              /* from the codec configuration */
static volatile bool s_rc;              /* AVRCP connected: the phone takes Pause and Play */
static uint8_t s_tl;                    /* AVRCP transaction label, 0..15 */
static volatile uint8_t s_vol = 127;    /* the phone's absolute volume, 0..127: full until it says */
static volatile bool s_phone;           /* a phone is connected */
static int64_t s_up;                    /* when Bluetooth came up */
static esp_timer_handle_t s_hide;

/* connectable always; discoverable while the window is open and no phone is on */
static void visible(void)
{
    bool open = esp_timer_get_time() - s_up < DISCOVER_S * 1000000LL;
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, open ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

static void hide(void *arg)
{
    if (s_phone) return;
    visible();
    ESP_LOGI(TAG, "no longer visible to new phones; a paired one connects as before");
}

/* the phone's volume over AirPlay's range, -30..0 dB; 0 mutes. With absolute volume the phone sends its sound at full
   scale and the volume is applied here, in floats, instead of the phone cutting bits off */
static float vol_db(uint8_t v) { return v ? 30.0f * ((float)v - 127) / 127 : -144; }

static void apply_vol(void) { if (phone_owner(NULL) == PHONE_A2DP) phone_gain(vol_db(s_vol)); }

/* AVRCP as a target: the phone sets the volume (SetAbsoluteVolume) and asks to hear of changes made here */
static void tg_cb(esp_avrc_tg_cb_event_t ev, esp_avrc_tg_cb_param_t *p)
{
    if (ev == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        s_vol = p->set_abs_vol.volume & 0x7F;
        ESP_LOGI(TAG, "phone volume %u of 127: %.1f dB", s_vol, vol_db(s_vol));
        apply_vol();
    } else if (ev == ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT && p->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        esp_avrc_rn_param_t rn = { .volume = s_vol };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
    }
}

static void rc_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t *p)
{
    if (ev == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        s_rc = p->conn_stat.connected;
        ESP_LOGI(TAG, "remote control %s", s_rc ? "connected" : "gone");
    }
}

/* phone.c's pause and play: a press and a release of the key, as a car kit sends them */
static void remote(phone_cmd_t cmd)
{
    if (!s_rc) {
        ESP_LOGW(TAG, "no remote control connection: the phone is not %s", cmd == PHONE_PAUSE ? "paused" : "resumed");
        return;
    }
    uint8_t key = cmd == PHONE_PAUSE ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
    esp_avrc_ct_send_passthrough_cmd(s_tl, key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    s_tl = (s_tl + 1) & 15;
    esp_avrc_ct_send_passthrough_cmd(s_tl, key, ESP_AVRC_PT_CMD_STATE_RELEASED);
    s_tl = (s_tl + 1) & 15;
}

static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) ESP_LOGI(TAG, "paired with %s", p->auth_cmpl.device_name);
        else ESP_LOGW(TAG, "pairing failed: %d", p->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:        /* nothing to show the number on: accept it */
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p)
{
    switch (ev) {
    case ESP_A2D_PROF_STATE_EVT:
        if (p->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
            /* the sink's SBC endpoint, after the init and before any connection (IDF 5.5): every SBC option */
            esp_a2d_mcc_t mcc = { .type = ESP_A2D_MCT_SBC };
            mcc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_16K | ESP_A2D_SBC_CIE_SF_32K | ESP_A2D_SBC_CIE_SF_44K |
                                         ESP_A2D_SBC_CIE_SF_48K;
            mcc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_MONO | ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL |
                                       ESP_A2D_SBC_CIE_CH_MODE_STEREO | ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
            mcc.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_4 | ESP_A2D_SBC_CIE_BLOCK_LEN_8 |
                                         ESP_A2D_SBC_CIE_BLOCK_LEN_12 | ESP_A2D_SBC_CIE_BLOCK_LEN_16;
            mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 | ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
            mcc.cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR | ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
            mcc.cie.sbc_info.min_bitpool = 2;
            mcc.cie.sbc_info.max_bitpool = 250;
            if (esp_a2d_sink_register_stream_endpoint(0, &mcc) != ESP_OK) ESP_LOGE(TAG, "SBC endpoint refused");
        }
        break;
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "phone connected");
            s_phone = true;
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "phone disconnected");
            s_phone = false;
            phone_stop(PHONE_A2DP);
            s_vol = 127;                /* the next phone tells its own */
            visible();
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        /* the phone starts its stream when it plays and suspends it a few seconds after a pause: the newest stream
           takes over from whatever played */
        if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "stream started");
            phone_start(PHONE_A2DP, s_rate);
            apply_vol();
        } else {
            ESP_LOGI(TAG, "stream suspended");
            phone_stop(PHONE_A2DP);
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT: {
        uint8_t f = p->audio_cfg.mcc.cie.sbc_info.samp_freq;
        s_rate = f & ESP_A2D_SBC_CIE_SF_48K ? 48000 : f & ESP_A2D_SBC_CIE_SF_44K ? 44100 :
                 f & ESP_A2D_SBC_CIE_SF_32K ? 32000 : 16000;
        ESP_LOGI(TAG, "SBC at %d Hz", s_rate);
        break;
    }
    default:
        break;
    }
}

/* decoded PCM, 16-bit stereo; copied when the stack's buffer is not aligned for 16-bit reads */
static void data_cb(const uint8_t *buf, uint32_t len)
{
    EXT_RAM_BSS_ATTR static int16_t al[512 * 2];     /* rarely used: PSRAM */
    int frames = (int)(len / 4);
    if (!((uintptr_t)buf & 1)) {
        phone_feed(PHONE_A2DP, (const int16_t *)(const void *)buf, frames);
        return;
    }
    for (int done = 0; done < frames;) {
        int k = frames - done < 512 ? frames - done : 512;
        memcpy(al, buf + 4 * done, 4 * k);
        phone_feed(PHONE_A2DP, al, k);
        done += k;
    }
}

void a2dp_start(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bc = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t hc = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bc) != ESP_OK || esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK ||
        esp_bluedroid_init_with_cfg(&hc) != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth did not start");
        return;
    }
    esp_bt_io_cap_t io = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io, sizeof io);
    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name(NAME);
    if (esp_avrc_ct_init() != ESP_OK || esp_avrc_ct_register_callback(rc_cb) != ESP_OK)
        ESP_LOGE(TAG, "AVRCP did not start: the phone cannot be paused");
    esp_avrc_rn_evt_cap_mask_t evs = { 0 };
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evs, ESP_AVRC_RN_VOLUME_CHANGE);
    if (esp_avrc_tg_init() != ESP_OK || esp_avrc_tg_register_callback(tg_cb) != ESP_OK ||
        esp_avrc_tg_set_rn_evt_cap(&evs) != ESP_OK)
        ESP_LOGE(TAG, "AVRCP target did not start: the phone keeps its volume to itself");
    phone_remote_set(PHONE_A2DP, remote);
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(data_cb);
    if (esp_a2d_sink_init() != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink did not start");
        return;
    }
    s_up = esp_timer_get_time();
    visible();
    const esp_timer_create_args_t ha = { .callback = hide, .name = "bt_hide" };
    if (esp_timer_create(&ha, &s_hide) == ESP_OK) esp_timer_start_once(s_hide, DISCOVER_S * 1000000LL);
    ESP_LOGI(TAG, "visible over Bluetooth as %s for %d min", NAME, DISCOVER_S / 60);
}
