/*
 * The scrape task.
 *
 * Owns the network side entirely: no LVGL call appears anywhere in this file
 * or in anything it calls. Results reach the UI through a snapshot guarded by
 * a short-held mutex plus a generation counter that an lv_timer polls.
 *
 * That is deliberate rather than stylistic. The alternative -- workers calling
 * LVGL directly under the LVGL lock -- means holding the rendering lock across
 * hundreds of widget writes while data is flowing, and it hands worker code
 * widget pointers that a screen rebuild can invalidate underneath it. Here the
 * worker knows only numbers.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define POLLER_MAX_WATCH CFG_MAX_PANELS
#define POLLER_NAME_MAX  40
/* Buckets carried to the UI for display. A histogram with more is merged down
 * to this many -- past about a dozen bars a 380px tile cannot render them
 * distinguishably anyway. */
#define POLLER_MAX_BUCKETS 16
/* Rows a multi-series tile can show. Past about six, a 185px-wide tile is
 * illegible and the reader wants the detail view instead. */
#define POLLER_MAX_CHILDREN 6
#define POLLER_ROWS_MAX     POLLER_MAX_CHILDREN
#define POLLER_CHILD_LABEL  20
#define POLLER_TEXT_MAX  24

typedef struct {
    /*
     * Which panel these numbers belong to.
     *
     * The dashboard used to pair snapshot slot i with tile i, which required
     * the poller and the tile builder to filter panels by exactly the same
     * predicate -- and a mismatch showed one metric's numbers under another
     * metric's title, silently. With screens the two lists genuinely differ:
     * the poller watches every panel so a screen is populated the moment you
     * swipe to it, while the tiles cover only the screen on display.
     */
    uint16_t panel_id;
    char   label[POLLER_NAME_MAX];   /* display name */
    char   num[POLLER_TEXT_MAX];     /* formatted value */
    char   suffix[POLLER_TEXT_MAX];  /* unit */
    float  value;                    /* the displayed quantity, unformatted --
                                      * charts, gauges and bars need a number,
                                      * not a string */
    bool   numeric_only;             /* num[] is safe for the digit faces */
    bool   valid;                    /* false while warming up or absent */

    /* Histogram/summary extras, valid when has_hist. The UI does no maths on
     * these: shares are already normalised and quantiles already derived. */
    bool    has_hist;
    uint8_t n_buckets;
    float   bucket_le[POLLER_MAX_BUCKETS];    /* upper bound; INFINITY last */
    float   bucket_share[POLLER_MAX_BUCKETS]; /* non-cumulative, 0..1 */
    float   p50, p90, p99;
    uint8_t fmt;                     /* fmt_mode_t, for rendering quantiles */
    char    unit[8];
    /* The panel's pinned prefix, carried so a chart's axis and a histogram's
     * quantiles cannot disagree with the number above them. */
    int8_t  scale;
    bool    group;
    float   peak;     /* largest value seen; the auto full-scale for a gauge */
    /* True once a fractional reading has been seen. Carried so a caption
     * formats to the same precision as the value above it. */
    bool    seen_fraction;

    /* Multi-series extras: one row per matching series, ranked by value. */
    uint8_t n_children;
    uint8_t n_matched;               /* total matches, may exceed n_children */
    char    child_label[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    float   child_value[POLLER_MAX_CHILDREN];
    char    child_num[POLLER_MAX_CHILDREN][16];
    uint8_t child_order[POLLER_MAX_CHILDREN];
    bool   warming;                  /* counter with no baseline yet */
    bool   restarted;                /* counter reset seen on this scrape */
} poller_metric_t;

typedef struct {
    poller_metric_t m[POLLER_MAX_WATCH];
    int      n;
    uint32_t generation;    /* bumped on every commit; compare with != */
    bool     ok;            /* last scrape succeeded */
    char     status[56];    /* human-readable state or failure reason */
    uint32_t latency_ms;
    uint32_t samples;       /* samples parsed in the last scrape */
    uint64_t body_bytes;
    uint32_t fail_streak;
    int64_t  last_ok_ms;    /* monotonic */
} poller_snap_t;

esp_err_t poller_start(const char *url, int interval_s);

/* Retarget a running poller. Safe from the LVGL task: the scrape loop picks
 * the new URL up on its next cycle, and the cached connection is dropped so
 * the next request cannot reuse a socket to the old host. */
void poller_set_endpoint(const char *url, int interval_s);

/* The URL currently being polled (empty when unconfigured). */
const char *poller_url(void);

/* Copies the current snapshot. Safe from the LVGL task; holds the mutex for
 * microseconds. Safe to call before poller_start(), which yields a zeroed
 * snapshot rather than crashing -- the UI is built before the poller runs. */
void poller_snapshot(poller_snap_t *out);

/* Ask for the watch list to be rebuilt from the stored panels. Safe from any
 * task: it raises a flag and the scrape loop does the work, so s_watch is
 * only ever written by the task that reads it. */
void poller_reload(void);

/* The panel id backing a watch slot, so the UI can map a tile to its config. */
uint16_t poller_panel_id(int idx);

/* Cheap change check for the UI timer, no lock taken. */
uint32_t poller_generation(void);
