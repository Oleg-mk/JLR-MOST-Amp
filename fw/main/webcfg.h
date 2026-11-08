#pragma once
/* The device's own page: http://192.168.4.1 on the setup access point, or its address on the hotspot. It takes the
   phone's hotspot and the setup access point's password and shows the link's state. While the setup access point is
   up, every DNS name points to the page, so a phone opens it by itself (captive portal) */
bool webcfg_start(void);        /* false: the HTTP server did not start (an update on trial is not confirmed then) */
