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
    TILE_KIND_COUNT,
} tile_kind_t;

/* What a tile shows and where. */
typedef struct {
    const char *title;
    tile_kind_t kind;
    uint8_t     col, row, w, h;   /* grid position and span */
    float       vmin, vmax;       /* BAR/GAUGE range; NAN = auto from history */
    float       warn, crit;       /* NAN = no threshold */
    bool        lower_is_worse;   /* thresholds compare the other way */
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

/* Build a tile into `parent` at its spec's grid position. */
tile_inst_t *tile_create(lv_obj_t *parent, const tile_spec_t *spec);
void         tile_update(tile_inst_t *t, const tile_data_t *d);
void         tile_destroy(tile_inst_t *t);
