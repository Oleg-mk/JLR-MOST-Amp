#pragma once
/* Wi-Fi (docs/firmware.md, "Wi-Fi"): the device joins the phone's hotspot as a station - AirPlay needs a network
   shared with the iPhone, and on its own hotspot the iPhone keeps its internet over the mobile network. The hotspot's
   name and password come from the device's own setup access point and page (webcfg.c). In QEMU the emulated Ethernet
   stands in (wifi_qemu.c) */
#include <stdbool.h>
#include <stdint.h>

typedef enum { WIFI_NONE, WIFI_JOINING, WIFI_ONLINE } wifi_link_t;

typedef struct {
    wifi_link_t link;
    char ssid[33];          /* the network joined, or being joined */
    char ip[16];
    int rssi;               /* dBm, while online */
    uint8_t reason;         /* why the last attempt failed (WIFI_REASON_*), 0 = none */
    bool setup_ap;          /* the setup access point is up */
    bool emulated;          /* QEMU: the emulated Ethernet */
} wifi_state_t;

typedef struct { char ssid[33]; int rssi; bool open; } wifi_seen_t;

#define WIFI_AP_SSID    "Bitline"
#define WIFI_AP_IP      "192.168.4.1"

void wifi_start(void);
void wifi_state(wifi_state_t *st);
int wifi_scan(wifi_seen_t *out, int max);       /* networks in reach, strongest first */
void wifi_apply(void);                          /* the stored hotspot changed: join it */
void wifi_setup(bool on);                       /* the setup access point on or off by hand */
