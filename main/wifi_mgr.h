/*
 * WiFi station management: scan, connect, and reconnect without rebooting.
 *
 * The no-reboot part is a deliberate divergence from esp32flight, which calls
 * esp_restart() on every settings save. That is right for a device whose whole
 * state arrives over the network; it is wrong here, where you are often
 * mid-configuration with a metric selection in flight and losing it is
 * punishing.
 */
#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_ST_IDLE = 0,      /* no credentials stored */
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,        /* gave up; see wifi_mgr_fail_reason() */
} wifi_state_t;

/* Brings up the stack and, if credentials are stored, starts connecting.
 * Safe to call with no credentials -- the radio still starts so scanning
 * works, which is what the setup wizard needs. */
esp_err_t wifi_mgr_start(void);

wifi_state_t wifi_mgr_state(void);
bool         wifi_mgr_is_connected(void);

/* A human-readable reason for the last failure, e.g. "wrong password",
 * "network not found". Generic errors help nobody at setup time. */
const char  *wifi_mgr_fail_reason(void);

/* Apply new credentials live: disconnect, reconfigure, reconnect. Does not
 * persist them -- the caller owns that, via secrets_set_wifi(). */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass);

/* Blocks until connected or the timeout expires. Never call from the LVGL
 * task. */
bool wifi_mgr_wait_connected(uint32_t timeout_ms);

/* Blocking scan; must run off the UI task. Pauses the reconnect loop for the
 * duration so the scan is not fighting an in-flight association. Results are
 * sorted by RSSI descending and de-duplicated by SSID. */
esp_err_t wifi_mgr_scan(wifi_ap_record_t *out, uint16_t *n_inout);

/* Current IP as a dotted string and the RSSI; either pointer may be NULL. */
void wifi_mgr_info(char *ip, size_t ip_cap, int8_t *rssi);
