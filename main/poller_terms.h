/*
 * Runtime state for one term of a panel. Internal to the poller.
 *
 * Kept in PSRAM rather than .bss: four terms per panel across twelve panels
 * is ~85KB, and internal SRAM is the scarce resource on this board (task
 * stacks cannot live in PSRAM, so everything that can be moved out should be).
 */
#pragma once

#include "config.h"
#include "prom_math.h"
#include "prom_types.h"
#include "ui_fmt.h"

#include <stdint.h>

/* Buckets a term keeps for quantile work. Real histograms run 10-20; the
 * parser's 64 is a parse-time bound, not a per-term one. */
#define TERM_MAX_BUCKETS 32
/* Samples in the windowed-rate ring. */
#define TERM_WIN_MAX     24

typedef struct {
    /*
     * The selector exactly as configured.
     *
     * Kept apart from `scratch`, which prom_parse_selector rewrites in place
     * into name\0key\0value\0..., because a reload has to answer "is this
     * the same term as before" to decide whether the baselines below are
     * still about the same series. A destructively parsed buffer cannot
     * answer that.
     */
    char         sel[CFG_SEL_MAX];

    /* --- selector, parsed once at reload --- */
    char         scratch[CFG_SEL_MAX];
    const char  *name;
    uint16_t     name_len;
    prom_label_t labels[8];
    uint8_t      n_labels;
    bool         has_glob;      /* any label value contains '*' */
    bool         active;

    uint8_t      reduce;        /* reduce_t */
    uint8_t      agg;           /* agg_mode_t */
    uint16_t     window_s;
    float        q;

    /* --- accumulated during one scrape --- */
    double       acc;           /* sum, or the running min/max/first */
    uint32_t     n_acc;
    bool         seen;
    int          nb;
    double       le[TERM_MAX_BUCKETS];
    double       cum[TERM_MAX_BUCKETS];

    /*
     * --- carried between scrapes, and across a reload when the term above
     * is unchanged ---
     *
     * A one-hour window takes an hour to fill and a counter baseline is lost
     * the moment it is dropped, so throwing this away because some other
     * panel was edited means every save costs every tile its history.
     */
    rate_state_t rate;
    double       win_v[TERM_WIN_MAX];
    int64_t      win_t[TERM_WIN_MAX];
    uint8_t      win_n, win_head;
    float        win_last;
    bool         win_valid;
    double       base_cum[TERM_MAX_BUCKETS];
    double       base_le[TERM_MAX_BUCKETS];
    int          base_nb;
    int64_t      base_t;
} term_rt_t;
