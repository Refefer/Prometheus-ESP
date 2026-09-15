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
#define POLLER_TEXT_MAX  24

typedef struct {
    char   label[POLLER_NAME_MAX];   /* display name */
    char   num[POLLER_TEXT_MAX];     /* formatted value */
    char   suffix[POLLER_TEXT_MAX];  /* unit */
    float  value;                    /* the displayed quantity, unformatted --
                                      * charts, gauges and bars need a number,
                                      * not a string */
    bool   numeric_only;             /* num[] is safe for the digit faces */
    bool   valid;                    /* false while warming up or absent */
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

/* Rebuild the watch list from the stored panels. Safe from the LVGL task:
 * the scrape loop picks the new list up on its next cycle. */
void poller_reload(void);

/* The panel id backing a watch slot, so the UI can map a tile to its config. */
uint16_t poller_panel_id(int idx);

/* Cheap change check for the UI timer, no lock taken. */
uint32_t poller_generation(void);
