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

#define CFG_SCHEMA_VERSION   2
#define CFG_MAX_TERMS        4
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
/*
 * How the several series a term matches collapse into one number.
 *
 * This is the "sum by()" of the config: a term selects a set with its
 * selector (label values may be globs) and a reducer turns that set into a
 * scalar. RED_FIRST preserves the old behaviour of binding to whichever
 * series appeared first, which is occasionally what you want and is never
 * what you want by accident.
 */
typedef enum {
    RED_SUM = 0,
    RED_AVG,
    RED_MIN,
    RED_MAX,
    RED_COUNT,
    RED_FIRST,
} reduce_t;

/*
 * One operand of a panel.
 *
 * Each term is independently aggregated and independently rated, so
 * rate(sum(a)) and sum(rate(a)) are both expressible and the panel's two
 * sides can use different windows if that is genuinely what is meant.
 */
typedef struct {
    char      sel[CFG_SEL_MAX];  /* metric{label="value"}; * in a value globs */
    uint8_t   reduce;            /* reduce_t */
    uint8_t   agg;               /* agg_mode_t: AGG_LAST or AGG_RATE */
    uint16_t  window_s;          /* 0 = one poll interval (or all-time for q) */
    float     q;                 /* >0: histogram quantile instead of a value */
} cfg_term_t;

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
    /*
     * Operands. A plain panel has one term; a derived panel has two and an
     * op. Schema 1 carried sel/sel_b/op/q/agg/window_s directly on the panel
     * and is migrated into this shape on load.
     */
    cfg_term_t  terms[CFG_MAX_TERMS];
    uint8_t     n_terms;
    panel_op_t  op;

    /* Mirrors terms[0] so the touch UI and the browser can keep working in
     * terms of "the panel's metric" without unpacking the array. */
    char        sel[CFG_SEL_MAX];
    char        title[CFG_TITLE_MAX];/* "" => derive from the metric name */
    tile_kind_t kind;
    fmt_mode_t  fmt;
    char        unit[8];
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

/*
 * Replace the whole configuration from a JSON document.
 *
 * Parsed into a scratch copy and only swapped in if it is whole: a push that
 * half-applied would leave the panel matching neither the old config nor the
 * new one, which is worse than rejecting it. On failure nothing changes and
 * `err` explains why in terms of the document, not of C.
 *
 * Safe to call from the HTTP task: it takes no LVGL lock and does not touch
 * the poller's state directly -- both notice via config_generation().
 */
esp_err_t config_apply_json(const char *json, size_t len,
                            char *err, size_t err_cap);

/*
 * Bumped whenever the configuration changes from any source. The poller and
 * the dashboard both poll it rather than being called back, so a push from
 * the HTTP task never runs UI code or rewrites the watch list underneath the
 * task that is reading it.
 */
uint32_t config_generation(void);

/* True when the last load fell back to defaults. */
bool config_was_reset(void);

/*
 * terms[0], created if the panel has none. Every panel has at least one term,
 * so the UI can treat this as "the panel's metric" without unpacking the
 * array -- and a pushed config with four terms still works underneath it.
 */
cfg_term_t *config_term0(cfg_panel_t *p);

cfg_panel_t *config_panel_add(void);
void         config_panel_remove(uint16_t id);
/* True if a selector is already on a screen -- the browser shows ticks. */
bool         config_has_panel(uint16_t ep_id, const char *sel);

/* Place a new panel in the first free cell of its screen, or return false if
 * the screen is full. Spans come from the renderer's natural size. */
bool config_place_panel(cfg_panel_t *p);

/*
 * Move a panel one cell in a direction.
 *
 * Moves into free space, and SWAPS with a single neighbour of the same span
 * rather than refusing -- reordering two tiles is the common case, and making
 * the user empty a cell first to do it would be tedious. Returns false when
 * neither is possible, so the caller can say why.
 */
bool config_nudge_panel(cfg_panel_t *p, int dcol, int drow);

/*
 * Can this panel sit with its top-left at (col,row)?
 *
 * The panel is excluded from the occupancy test, so "where it already is"
 * counts as free -- otherwise a tile could never be told to stay put, and
 * every overlap check would have to special-case itself.
 */
bool config_panel_fits(const cfg_panel_t *p, int col, int row);

/*
 * Place a panel at (col,row), swapping with a single same-size occupant if
 * there is one. Returns false if it neither fits nor swaps cleanly.
 */
bool config_move_panel(cfg_panel_t *p, int col, int row);

cfg_endpoint_t *config_endpoint_by_id(uint16_t id);
cfg_endpoint_t *config_endpoint_add(void);
void            config_endpoint_remove(uint16_t id);

/* Copy the live config to config.bak, once the UI is up and the config has
 * proven loadable. The backup is then a known-good-BOOT config rather than
 * merely the previous save. */
void config_mark_good_boot(void);
