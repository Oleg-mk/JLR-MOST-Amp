/* QEMU: no Wi-Fi radio there. The emulated OpenCores Ethernet (QEMU's user network shows the PC as 10.0.2.2) stands in,
   so the setup page and the virtual phone run over it; a hotspot saved on the page is stored, never joined */
#include <string.h>
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "wcred.h"
#include "wifi.h"

static const char *TAG = "wifi";

static char s_ip[16];
static volatile bool s_up;

/* esp_restart() ends in a CPU reset, and QEMU resets only the CPUs: its emulated peripherals run on, and the next boot
   crashes early in its start-up (seen when testing updates). The system reset resets the whole emulated chip; the
   flash image stays */
static void full_reset(void) { esp_rom_software_reset_system(); }

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_ip4addr_ntoa(&((ip_event_got_ip_t *)data)->ip_info.ip, s_ip, sizeof s_ip);
    s_up = true;
    ESP_LOGI(TAG, "QEMU: emulated Ethernet up as %s", s_ip);
}

void wifi_start(void)
{
    esp_register_shutdown_handler(full_reset);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&nc);
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.autonego_timeout_ms = 100;
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(esp_eth_mac_new_openeth(&mc), esp_eth_phy_new_dp83848(&pc));
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&ec, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

void wifi_state(wifi_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->link = s_up ? WIFI_ONLINE : WIFI_JOINING;
    st->emulated = true;
    strlcpy(st->ssid, "QEMU Ethernet", sizeof st->ssid);
    strlcpy(st->ip, s_ip, sizeof st->ip);
}

int wifi_scan(wifi_seen_t *out, int max) { return 0; }

void wifi_apply(void)
{
    char ssid[33], pass[65];
    if (wcred_get(ssid, sizeof ssid, pass, sizeof pass) == ESP_OK)
        ESP_LOGI(TAG, "QEMU: hotspot '%s' stored, %u-character password; not joined here", ssid,
                 (unsigned)strlen(pass));
    else
        ESP_LOGI(TAG, "QEMU: no hotspot stored");
}

void wifi_setup(bool on) { ESP_LOGI(TAG, "QEMU: no setup access point"); }
