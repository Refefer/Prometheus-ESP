/*
 * Tile renderers behind a vtable.
 *
 * A tile is a shell (card, title, freshness dot, tap and long-press handling)
 * plus a renderer-owned body. The shell owns everything common so no renderer
 * reimplements it, and the vtable means the metric browser can offer widget
 * types without the dashboard knowing what they are.
 *
 * Renderers declare the smallest span they work in and a fallback kind for
 * when they are forced below it -- a chart with axes is illegible at 185x128
 * and should quietly become a sparkline rather than draw unreadably.
 */
#pragma once

#include "lvgl.h"
#include "ui_fmt.h"
#include "ui_theme.h"

typedef enum {
    TILE_STAT = 0,   /* big number, optional delta */
    TILE_SPARK,      /* number + sparkline */
    TILE_CHART,      /* line chart with axes */
    TILE_BAR,        /* value against a range */
    TILE_GAUGE,      /* arc against a range */
    TILE_STATUS,     /* up/down */
    TILE_HIST,       /* bucket distribution + tail quantiles */
    TILE_MULTI,      /* one metric across several label sets */
    TILE_KIND_COUNT,
} tile_kind_t;

/* What a tile shows and where. */
typedef struct {
    uint16_t    panel_id;     /* so a tap can open the right settings */
    const char *title;
    tile_kind_t kind;
    uint8_t     screen;           /* which page it belongs to */
    uint8_t     col, row, w, h;   /* grid position and span */
    float       vmin, vmax;       /* BAR/GAUGE range; NAN = auto from history */
    float       warn, crit;       /* NAN = no threshold */
    bool        lower_is_worse;   /* thresholds compare the other way */
    uint8_t     ramp;             /* ramp_t: how the indicator is coloured */
    /* config_panel_fingerprint of the panel behind this tile. A tile whose
     * fingerprint changed is showing a different series, so its accumulated
     * history is no longer about what it is about to display. */
    uint32_t    data_fp;
} tile_spec_t;

/* One refresh's worth of already-resolved data. Renderers do no maths. */
typedef struct {
    bool        valid;
    bool        warming;
    bool        restarted;
    float       value;        /* for charts and ranges */
    const char *num;          /* formatted, digits-only when numeric_only */
    const char *suffix;
    bool        numeric_only; /* safe for the large digit faces */

    /* Histogram extras; has_hist is false for every other metric kind. */
    bool         has_hist;
    uint8_t      n_buckets;
    const float *bucket_le;
    const float *bucket_share;
    float        p50, p90, p99;
    fmt_mode_t   fmt;         /* how to render the quantiles */
    const char  *unit;
    int8_t       scale;       /* pinned prefix, or FMT_PIN_AUTO */
    bool         group;       /* thousands separators */
    float        peak;        /* largest seen; the auto full scale */
    bool         seen_fraction;  /* has this series ever been fractional? */

    /* Multi-series extras. */
    uint8_t        n_children;
    uint8_t        n_matched;     /* may exceed n_children; the tile says "+N" */
    const char   (*child_label)[20];
    const float   *child_value;
    const char   (*child_num)[16];
    const uint8_t *child_order;
} tile_data_t;

typedef struct tile_inst tile_inst_t;

typedef struct {
    const char *name;
    uint8_t     min_w, min_h;
    tile_kind_t fallback;
    void (*build)  (tile_inst_t *t, lv_obj_t *body);
    void (*update) (tile_inst_t *t, const tile_data_t *d);
    void (*destroy)(tile_inst_t *t);
} tile_vt_t;

#define TILE_HIST_MAX 120       /* 10 minutes at a 5s poll */

struct tile_inst {
    const tile_vt_t   *vt;
    const tile_spec_t *spec;
    lv_obj_t *shell, *body, *title_lbl, *dot;
    void     *priv;
    /* Per-tile history, appended on each refresh. Charts own their own copy
     * rather than snapshotting a shared store: the snapshot would have to be
     * copied out under a lock on every repaint, and only the visible tiles
     * ever need it. */
    float     hist[TILE_HIST_MAX];
    uint16_t  hist_n;
    severity_t last_sev;
};

const tile_vt_t *tile_vt(tile_kind_t k);

/*
 * Rebind an existing tile to a new spec, in place.
 *
 * Only the fields that can change without changing the widget: title and
 * grid position. The caller must already have established that the kind, the
 * span and the data fingerprint are unchanged -- anything else and the tile
 * has to be rebuilt, which is what discards its history.
 */
void tile_adopt(tile_inst_t *t, const tile_spec_t *spec);

/*
 * Show or hide a tile without destroying it.
 *
 * Tiles exist for every screen, not just the one on display, so paging is a
 * visibility change rather than a teardown. A hidden tile keeps accumulating
 * history and takes no input, so swiping to a page finds its charts already
 * drawn instead of empty.
 */
void tile_set_visible(tile_inst_t *t, bool on);

/* Called when a tile is tapped. Set once at startup. */
void tile_set_tap_handler(void (*cb)(uint16_t panel_id));

/* Build a tile into `parent` at its spec's grid position. */
tile_inst_t *tile_create(lv_obj_t *parent, const tile_spec_t *spec);
void         tile_update(tile_inst_t *t, const tile_data_t *d);
void         tile_destroy(tile_inst_t *t);
