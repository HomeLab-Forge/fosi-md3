// Wi-Fi station + mDNS (md3.local) + the authenticated HTTP control API. Credentials and
// the API token live in NVS namespace "md3" (keys ssid, pass, token), written by
// a provisioning NVS image (the host tool is not published: no radio works on this hardware).
// Every request must carry the token in X-MD3-Token. The API is
// control-only: it exposes no reboot, download-mode, OTA, or firmware-write operation.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Start networking without blocking boot on DHCP. Missing credentials disable the API. */
esp_err_t net_start(void);

/** Called by the UI/control task; retries HTTPD and refreshes RSSI occasionally. */
void net_tick(void);

/** Take the coalesced network-change notification without dropping it on a full UI queue. */
bool net_take_dirty(void);
