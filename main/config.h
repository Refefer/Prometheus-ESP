/*
 * Persisted configuration: a versioned JSON document on LittleFS.
 *
 * Chosen over NVS blobs because the payload grows with the number of selected
 * series (hundreds is plausible), because LittleFS gives an atomic rename
 * over an existing file -- the primitive that makes "survives a power cut"
 * true rather than probable -- and because `cat /cfg/config.json` over serial
 * is a usable escape hatch on a device with no web UI.
 *
 * Secrets are deliberately NOT here; they live in NVS (see secrets.h), so
 * this file stays safe to dump, screenshot or paste into a bug report.
 */
#pragma once

#include "esp_err.h"
#include "ui_fmt.h"
#include "ui_tile.h"

#include <stdbool.h>
#include <stdint.h>

#define CFG_SCHEMA_VERSION   1
#define CFG_MAX_ENDPOINTS    8
#define CFG_MAX_PANELS      24
#define CFG_MAX_SCREENS      6
#define CFG_URL_MAX        192
#define CFG_NAME_MAX        24
#define CFG_SEL_MAX        160
#define CFG_TITLE_MAX       32

typedef enum { EP_TEXT = 0, EP_PROMAPI } ep_kind_t;
typedef enum { AUTH_NONE = 0, AUTH_BEARER, AUTH_BASIC } auth_kind_t;

typedef struct {
    uint16_t    id;
    char        name[CFG_NAME_MAX];
    ep_kind_t   kind;
    char        url[CFG_URL_MAX];
    uint16_t    poll_s;          /* 0 => use device.poll_default_s */
    uint16_t    timeout_ms;
    auth_kind_t auth;            /* the secret itself is in NVS, keyed by id */
    bool        insecure_tls;
    bool        enabled;
} cfg_endpoint_t;

typedef struct {
    uint16_t    id;
    uint16_t    ep_id;
    char        sel[CFG_SEL_MAX];    /* name{label="value",...} */
    char        title[CFG_TITLE_MAX];/* "" => derive from the metric name */
    tile_kind_t kind;
    fmt_mode_t  fmt;
    agg_mode_t  agg;
    char        unit[8];
    float       vmin, vmax;          /* NAN => auto */
    float       warn, crit;          /* NAN => no threshold */
    bool        lower_is_worse;
    uint8_t     screen;              /* index into screens[] */
    uint8_t     col, row, w, h;
} cfg_panel_t;

typedef struct {
    char    title[CFG_NAME_MAX];
    bool    pinned;      /* the auto-packer leaves a pinned screen alone */
} cfg_screen_t;

typedef struct {
    char     theme[16];
    uint16_t poll_default_s;
    bool     rotate_enabled;
    uint16_t rotate_dwell_s;
} cfg_device_t;

typedef struct {
    uint16_t       schema;
    cfg_device_t   device;
    cfg_endpoint_t endpoints[CFG_MAX_ENDPOINTS];
    uint8_t        n_endpoints;
    cfg_panel_t    panels[CFG_MAX_PANELS];
    uint8_t        n_panels;
    cfg_screen_t   screens[CFG_MAX_SCREENS];
    uint8_t        n_screens;
    uint16_t       next_id;
} config_t;

/* Load at boot: config.json, then config.bak, then factory defaults.
 * Returns ESP_OK only when a stored file was read -- a defaults fallback is
 * reported so the UI can say so rather than silently starting empty. */
esp_err_t config_load(void);

config_t *config_get(void);

/* Mark dirty; the flush happens a couple of seconds after the last change so
 * ticking forty checkboxes writes once, not forty times. */
void config_touch(void);

/*
 * Write now, atomically, ON A WORKER TASK.
 *
 * Never write from the LVGL task: LittleFS plus stdio needs several KB of
 * stack and the LVGL task runs on 6KB with under 2KB of idle headroom, and a
 * flash erase blocks for long enough to visibly stall rendering. Every caller
 * is a button handler or a timer running in that task, so this spawns a
 * short-lived writer and returns immediately.
 */
void config_flush(void);

/* Synchronous variant, for callers that are already off the LVGL task. */
esp_err_t config_flush_sync(void);

/* True when the last load fell back to defaults. */
bool config_was_reset(void);

cfg_endpoint_t *config_endpoint_by_id(uint16_t id);
cfg_endpoint_t *config_endpoint_add(void);
void            config_endpoint_remove(uint16_t id);

/* Copy the live config to config.bak, once the UI is up and the config has
 * proven loadable. The backup is then a known-good-BOOT config rather than
 * merely the previous save. */
void config_mark_good_boot(void);
