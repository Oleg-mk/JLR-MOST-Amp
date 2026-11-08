#pragma once
/* AirPlay 1 (RAOP) receiver (docs/firmware.md, "AirPlay"): lossless ALAC from an iPhone, into phone.c */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool session;               /* an iPhone holds an RTSP session */
    bool streaming;             /* between RECORD and FLUSH / TEARDOWN */
    bool encrypted;
    char peer[16];
    float volume_db;            /* the iPhone's volume: 0 full, -30 lowest, -144 mute */
    uint32_t packets, resent, lost, late, requests;
} raop_stats_t;

extern const char RAOP_KEY[];   /* raop_key.c: the AirPort Express RSA key, PEM */

void raop_start(void);          /* mDNS and the RTSP server, once the network stack is up */
void raop_stats(raop_stats_t *st);
