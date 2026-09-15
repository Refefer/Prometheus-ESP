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
/* A screen holds twelve 1x1 cells, so it cannot show more than twelve panels.
 * Keeping the cap at the real limit matters: poller_snap_t carries every slot
 * and three static copies of it live in internal SRAM. */
#define CFG_MAX_PANELS      12
#define CFG_MAX_SCREENS      6
#define CFG_URL_MAX        192
#define CFG_NAME_MAX        24
#define CFG_SEL_MAX        160
#define CFG_TITLE_MAX       32

typedef enum { EP_TEXT = 0, EP_PROMAPI } ep_kind_t;

/*
 * How a panel combines two series into one number.
 *
 * Deliberately four fixed shapes rather than an expression language: these
 * cover hit rates, error rates, shares and headroom, which is essentially
 * every ratio anyone puts on a panel, and each is one tap to choose rather
 * than a formula to type on a touchscreen.
 */
typedef enum {
    OP_NONE = 0,   /* single series */
    OP_SHARE,      /* a / (a+b)  -- cache hit rate, error rate */
    OP_RATIO,      /* a / b      -- ratio against a total or a capacity */
    OP_DIFF,       /* a - b      -- headroom */
    OP_SUM,        /* a + b      -- combined throughput */
} panel_op_t;
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
    char        sel_b[CFG_SEL_MAX];  /* the other operand, when op != OP_NONE */
    panel_op_t  op;
    char        title[CFG_TITLE_MAX];/* "" => derive from the metric name */
    tile_kind_t kind;
    fmt_mode_t  fmt;
    agg_mode_t  agg;
    char        unit[8];
    float       q;                   /* quantile for histogram/summary panels,
                                      * 0 => not a quantile panel */
    /*
     * Seconds of observations the quantile is taken over. 0 means all-time:
     * every observation since the exporter's process started.
     *
     * All-time is almost never what a panel wants. A long-lived process
     * accumulates enough history that one bad afternoon is permanently baked
     * in -- measured on a real inference server, the all-time p99 read 72s
     * while the last 30 seconds of traffic were at 0.6s.
     */
    uint16_t    window_s;
    float       vmin, vmax;          /* NAN => auto */
    float       warn, crit;          /* NAN => no threshold */
    bool        lower_is_worse;
    uint8_t     screen;              /* index into screens[] */
    uint8_t     col, row, w, h;
    /* A multi-series panel matches every series of its metric rather than one,
     * so its selector carries only the labels that narrow the set. */
    bool        multi;
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

cfg_panel_t *config_panel_add(void);
void         config_panel_remove(uint16_t id);
/* True if a selector is already on a screen -- the browser shows ticks. */
bool         config_has_panel(uint16_t ep_id, const char *sel);

/* Place a new panel in the first free cell of its screen, or return false if
 * the screen is full. Spans come from the renderer's natural size. */
bool config_place_panel(cfg_panel_t *p);

cfg_endpoint_t *config_endpoint_by_id(uint16_t id);
cfg_endpoint_t *config_endpoint_add(void);
void            config_endpoint_remove(uint16_t id);

/* Copy the live config to config.bak, once the UI is up and the config has
 * proven loadable. The backup is then a known-good-BOOT config rather than
 * merely the previous save. */
void config_mark_good_boot(void);
