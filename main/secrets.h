/*
 * Secrets in NVS, deliberately separate from the JSON config on LittleFS.
 *
 * Three reasons for the split: the WiFi driver needs an NVS partition anyway;
 * a corrupt config file still leaves the device on the network and therefore
 * recoverable; and config.json stays safe to `cat` over serial, screenshot, or
 * paste into a bug report.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECRETS_SSID_MAX 33    /* 32 + NUL */
#define SECRETS_PASS_MAX 65    /* 64 + NUL */
#define SECRETS_AUTH_MAX 513   /* bearer tokens get long */

bool      secrets_have_wifi(void);
esp_err_t secrets_get_wifi(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t secrets_set_wifi(const char *ssid, const char *pass);
esp_err_t secrets_clear_wifi(void);

/* Per-endpoint auth, keyed by endpoint id. NVS keys are capped at 15 chars;
 * "ep%u_auth" is 9 at the 16-endpoint limit, so there is room to spare. */
esp_err_t secrets_get_ep_auth(uint16_t ep_id, char *buf, size_t cap);
esp_err_t secrets_set_ep_auth(uint16_t ep_id, const char *value);
esp_err_t secrets_del_ep_auth(uint16_t ep_id);

/* Factory reset: wipes the whole namespace. */
esp_err_t secrets_erase_all(void);
