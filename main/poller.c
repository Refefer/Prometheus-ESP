#include "poller.h"

#include "http_util.h"
#include "prom_math.h"
#include "prom_ident.h"
#include "prom_text.h"
#include "ui_fmt.h"
#include "wifi_mgr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "poller";

/* Enough samples to span the longest offered window at the fastest poll: 15
 * minutes at 2s would need 450, so the ring is capped and the effective
 * window is simply as much history as it holds. */
#define RATE_WIN_MAX 24

/*
 * The watch list, rebuilt from the stored panels.
 *
 * Each entry keeps its own copy of the selector text because the parsed label
 * pointers alias it -- prom_parse_selector unescapes in place, so the scratch
 * must outlive the parse.
 */
typedef struct {
    uint16_t     panel_id;
    char         label[POLLER_NAME_MAX];
    char         scratch[CFG_SEL_MAX];
    const char  *name;
    uint16_t     name_len;
    prom_label_t labels[8];
    uint8_t      n_labels;
    agg_mode_t   agg;
    fmt_mode_t   fmt;
    char         unit[8];
    float        q;              /* >0 => derive this quantile from buckets */
    bool         active;

    bool         multi;

    /* Second operand for a derived panel. Its own scratch, because
     * prom_parse_selector unescapes in place and the label pointers alias it. */
    /*
     * Baseline for a windowed quantile: the bucket counts as of window_s ago,
     * and when they were taken. One snapshot rather than a ring, so the
     * effective window drifts between window_s and window_s + one poll --
     * which is well inside the noise of the thing being measured and costs
     * 256 bytes instead of kilobytes.
     */
    uint16_t     window_s;

    /*
     * Sample ring for a windowed rate.
     *
     * A counter that only updates on its exporter's log interval steps rather
     * than flows: polled faster than it updates, most polls see no change and
     * the rate reads zero, with an occasional spike. Measured on a real
     * inference server, prompt_tokens_total sat at 0 for four polls then
     * jumped 97,000 tokens.
     *
     * Differencing against the oldest sample still inside the window smooths
     * across those steps and updates every poll -- which is what rate(x[5m])
     * means, and why it exists.
     */
    double       win_v[RATE_WIN_MAX];
    int64_t      win_t[RATE_WIN_MAX];
    uint8_t      win_n, win_head;
    float        win_last;
    bool         win_valid;

    double       base_cum[PROM_MAX_BUCKETS];
    double       base_le[PROM_MAX_BUCKETS];
    int          base_nb;
    int64_t      base_t;

    panel_op_t   op;
    char         scratch_b[CFG_SEL_MAX];
    const char  *name_b;
    uint16_t     name_b_len;
    prom_label_t labels_b[8];
    uint8_t      n_labels_b;
    rate_state_t rate_b;

    rate_state_t rate;
    fmt_state_t  fmt_state;

    /* Per-child state for a multi-series watch, keyed by the child's label so
     * a baseline survives series appearing and disappearing between scrapes. */
    char         child_key[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    rate_state_t child_rate[POLLER_MAX_CHILDREN];
    uint8_t      n_child;
    uint8_t      order[POLLER_MAX_CHILDREN];  /* display order, held steady */
    uint8_t      resort_in;                   /* publishes until a re-rank */
} watch_rt_t;

/* Per-scrape accumulation. */
typedef struct {
    bool   seen;
    bool   seen_b;
    prom_value_t value_b;
    prom_value_t value;
    int    nb;
    double le[PROM_MAX_BUCKETS];
    double cum[PROM_MAX_BUCKETS];

    uint8_t n_child;
    uint8_t n_matched;
    char    child_label[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    double  child_value[POLLER_MAX_CHILDREN];
} watch_scratch_t;

static watch_rt_t      s_watch[POLLER_MAX_WATCH];
static int             s_watch_n;
static watch_scratch_t s_scratch[POLLER_MAX_WATCH];

static SemaphoreHandle_t s_mux;
static poller_snap_t     s_snap;
static char              s_url[160];
static int               s_interval_s = 10;

/*
 * poller_snap_t is passed around by value in places and has twice now grown
 * past a task stack after an innocuous-looking field was added to
 * poller_metric_t. Fail the build rather than the device.
 */
_Static_assert(sizeof(poller_snap_t) < 12 * 1024,
               "poller_snap_t is large; every holder must be static, never a "
               "stack local -- see publish() and dashboard_tick()");

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* The parser takes its allocator by injection so the host tests can use plain
 * malloc while the device forces PSRAM -- without this,
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 would put the 4KB line buffer in
 * the scarce internal heap. */
static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)   { heap_caps_free(p); }

/* --------------------------------------------------------------- matching */

/* Every label on the watch must be present on the sample with the same value.
 * Extra labels on the sample are fine -- a selector is a filter, not an
 * exact-match requirement. */
static bool labels_match_n(const prom_sample_t *s, const prom_label_t *want,
                           uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        bool found = false;
        for (uint8_t j = 0; j < s->n_labels; j++) {
            if (s->labels[j].key_len != want[i].key_len) continue;
            if (memcmp(s->labels[j].key, want[i].key, want[i].key_len) != 0) continue;
            found = (s->labels[j].val_len == want[i].val_len) &&
                    memcmp(s->labels[j].val, want[i].val, want[i].val_len) == 0;
            break;
        }
        if (!found) return false;
    }
    return true;
}

static bool labels_match(const prom_sample_t *s, const watch_rt_t *w)
{
    return labels_match_n(s, w->labels, w->n_labels);
}

/*
 * Name a child by the labels that actually distinguish it -- the ones the
 * watch's filter does not already pin. For node_cpu_seconds_total filtered to
 * nothing that yields "0 idle"; filtered to cpu="0" it yields just "idle",
 * which is what the reader needs and all a 185px row can hold.
 */
static void child_label_of(const prom_sample_t *s, const watch_rt_t *w,
                           char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < s->n_labels && o + 1 < cap; i++) {
        bool pinned = false;
        for (uint8_t j = 0; j < w->n_labels; j++) {
            if (s->labels[i].key_len == w->labels[j].key_len &&
                memcmp(s->labels[i].key, w->labels[j].key,
                       w->labels[i].key_len) == 0) { pinned = true; break; }
        }
        if (pinned) continue;
        if (o > 0 && o + 1 < cap) out[o++] = ' ';
        size_t n = s->labels[i].val_len;
        if (o + n >= cap) n = cap - o - 1;
        memcpy(out + o, s->labels[i].val, n);
        o += n;
    }
    out[o] = '\0';
    if (out[0] == '\0') strncpy(out, "value", cap - 1);
}

static bool name_is(const prom_sample_t *s, const char *want, uint16_t len)
{
    return s->name_len == len && memcmp(s->name, want, len) == 0;
}

static bool base_is(const prom_sample_t *s, const char *want, uint16_t len)
{
    return s->base_len == len && memcmp(s->base_name, want, len) == 0;
}

static bool on_sample(void *ctx, const prom_sample_t *s)
{
    (void)ctx;
    for (int i = 0; i < s_watch_n; i++) {
        const watch_rt_t *w = &s_watch[i];
        if (!w->active) continue;

        if (w->q > 0.0f) {
            if (s->role != PROM_ROLE_BUCKET) continue;
            if (!base_is(s, w->name, w->name_len)) continue;
            /*
             * The label filter applies to buckets as much as to plain
             * samples: a family split across label sets (an LLM server splits
             * latency by is_streaming) would otherwise have its buckets summed
             * into a distribution that never existed.
             */
            if (!labels_match(s, w)) continue;
            if (!prom_is_num(s->value)) continue;

            watch_scratch_t *sc = &s_scratch[i];
            double bound = prom_is_num(s->le) ? s->le.num
                         : (s->le.kind == PVAL_POS_INF ? INFINITY : NAN);
            if (isnan(bound)) continue;

            /* Deduplicate by bound, last value wins: a buggy exporter that
             * repeats a family would otherwise fill the array with duplicates
             * until the real +Inf bucket no longer fits. */
            int slot = -1;
            for (int b = 0; b < sc->nb; b++) {
                if (sc->le[b] == bound) { slot = b; break; }
            }
            if (slot < 0) {
                if (sc->nb >= PROM_MAX_BUCKETS) continue;
                slot = sc->nb++;
                sc->le[slot] = bound;
            }
            sc->cum[slot] = s->value.num;
            sc->seen = true;
            continue;
        }

        /* The second operand is matched independently, so the two sides can
         * be different label sets of the same metric or different metrics
         * entirely. */
        if (w->op != OP_NONE && w->name_b != NULL &&
            name_is(s, w->name_b, w->name_b_len) &&
            labels_match_n(s, w->labels_b, w->n_labels_b)) {
            s_scratch[i].value_b = s->value;
            s_scratch[i].seen_b  = true;
        }

        if (!name_is(s, w->name, w->name_len) || !labels_match(s, w)) continue;

        if (w->multi) {
            watch_scratch_t *sc = &s_scratch[i];
            if (!prom_is_num(s->value)) continue;
            char key[POLLER_CHILD_LABEL];
            child_label_of(s, w, key, sizeof(key));

            int slot = -1;
            for (int k = 0; k < sc->n_child; k++) {
                if (strcmp(sc->child_label[k], key) == 0) { slot = k; break; }
            }
            if (slot < 0) {
                sc->n_matched++;
                if (sc->n_child >= POLLER_MAX_CHILDREN) continue;
                slot = sc->n_child++;
                strncpy(sc->child_label[slot], key, POLLER_CHILD_LABEL - 1);
            }
            sc->child_value[slot] = s->value.num;
            sc->seen = true;
            continue;
        }

        s_scratch[i].value = s->value;
        s_scratch[i].seen  = true;
    }
    return true;
}

static bool feed_chunk(void *ctx, const char *data, size_t len)
{
    return prom_text_feed((prom_text_parser_t *)ctx, data, len);
}

/* ------------------------------------------------------------- publishing */

static void publish(bool ok, const char *status, uint32_t latency_ms,
                    const prom_text_stats_t *st, uint64_t bytes)
{
    /*
     * Static, not a stack local.
     *
     * poller_snap_t carries every watch slot and grew to ~6KB when histogram
     * buckets were added to each metric; this task has an 8KB stack. publish()
     * is only ever called from the poller task, so a static is safe and costs
     * nothing. The same growth caught dashboard_tick on the LVGL task.
     */
    static poller_snap_t next;
    memset(&next, 0, sizeof(next));
    next.n          = s_watch_n;
    next.ok         = ok;
    next.latency_ms = latency_ms;
    next.samples    = st ? st->samples : 0;
    next.body_bytes = bytes;
    strncpy(next.status, status, sizeof(next.status) - 1);

    int64_t t = now_ms();

    for (int i = 0; i < s_watch_n; i++) {
        watch_rt_t      *w  = &s_watch[i];
        watch_scratch_t *sc = &s_scratch[i];
        poller_metric_t *m  = &next.m[i];
        strncpy(m->label, w->label, sizeof(m->label) - 1);
        strncpy(m->unit, w->unit, sizeof(m->unit) - 1);
        m->fmt = (uint8_t)w->fmt;

        if (!ok || !sc->seen) {
            m->valid = false;
            continue;
        }

        double shown = NAN;

        if (w->op != OP_NONE) {
            /*
             * Both operands go through the panel's aggregation before being
             * combined. For counters that means ratio-of-rates, not
             * ratio-of-totals -- a windowed hit rate rather than a lifetime
             * one, which is what rate(a)/rate(a+b) means in PromQL and what
             * anyone watching a panel actually wants.
             */
            double a = NAN, b = NAN;
            if (w->agg == AGG_RATE) {
                float ra = 0, rb = 0;
                int64_t gap = (int64_t)s_interval_s * 3000;
                if (sc->seen &&
                    prom_rate_step(&w->rate, sc->value, t, gap, &ra) == RATE_OK) {
                    a = ra;
                }
                if (sc->seen_b &&
                    prom_rate_step(&w->rate_b, sc->value_b, t, gap, &rb) == RATE_OK) {
                    b = rb;
                }
                if (!isfinite(a) || !isfinite(b)) m->warming = true;
            } else {
                if (sc->seen   && prom_is_num(sc->value))   a = sc->value.num;
                if (sc->seen_b && prom_is_num(sc->value_b)) b = sc->value_b.num;
            }

            if (isfinite(a) && isfinite(b)) {
                switch (w->op) {
                case OP_SHARE: {
                    double d = a + b;
                    /* Both idle is not 0% -- it is "nothing happened", and a
                     * hit rate that reads 0 when the server is quiet would be
                     * read as a fault. */
                    shown = (d != 0.0) ? a / d : NAN;
                    break;
                }
                case OP_RATIO: shown = (b != 0.0) ? a / b : NAN; break;
                case OP_DIFF:  shown = a - b; break;
                case OP_SUM:   shown = a + b; break;
                default:       shown = a;     break;
                }
            }
        } else if (w->multi) {
            /* Per-child baselines are keyed by label so a series appearing or
             * disappearing between scrapes does not shift everyone else's
             * rate onto the wrong history. */
            for (int k = 0; k < sc->n_child; k++) {
                int slot = -1;
                for (int j = 0; j < w->n_child; j++) {
                    if (strcmp(w->child_key[j], sc->child_label[k]) == 0) {
                        slot = j; break;
                    }
                }
                if (slot < 0 && w->n_child < POLLER_MAX_CHILDREN) {
                    slot = w->n_child++;
                    strncpy(w->child_key[slot], sc->child_label[k],
                            POLLER_CHILD_LABEL - 1);
                }

                double cv = NAN;
                if (w->agg == AGG_RATE && slot >= 0) {
                    float r = 0;
                    rate_status_t rc = prom_rate_step(&w->child_rate[slot],
                                                      prom_num(sc->child_value[k]),
                                                      t, (int64_t)s_interval_s * 3000,
                                                      &r);
                    if (rc == RATE_OK) cv = r;
                } else {
                    cv = sc->child_value[k];
                }

                strncpy(m->child_label[k], sc->child_label[k],
                        POLLER_CHILD_LABEL - 1);
                m->child_value[k] = isfinite(cv) ? (float)cv : 0.0f;
                if (isfinite(cv)) {
                    fmt_state_t fs = {0};
                    char suf[12];
                    bool numeric;
                    ui_fmt_value(cv, w->fmt, w->unit, &fs,
                                 m->child_num[k], sizeof(m->child_num[k]),
                                 suf, sizeof(suf), &numeric);
                } else {
                    strncpy(m->child_num[k], "--", sizeof(m->child_num[k]) - 1);
                }
            }
            m->n_children = sc->n_child;
            m->n_matched  = sc->n_matched ? sc->n_matched : sc->n_child;

            /*
             * Rank by value, but hold the order for about a minute.
             * Re-sorting every poll makes rows leapfrog continuously, which is
             * unreadable -- you cannot follow a row long enough to read it.
             */
            if (w->resort_in == 0) {
                for (int a = 1; a < m->n_children; a++) {
                    uint8_t keyi = (uint8_t)a;
                    int b = a - 1;
                    while (b >= 0 &&
                           m->child_value[w->order[b]] < m->child_value[keyi]) {
                        w->order[b + 1] = w->order[b];
                        b--;
                    }
                    w->order[b + 1] = keyi;
                }
                w->resort_in = 12;
            } else {
                w->resort_in--;
            }
            for (int a = 0; a < m->n_children; a++) {
                if (w->order[a] >= m->n_children) w->order[a] = (uint8_t)a;
                m->child_order[a] = w->order[a];
            }

            /* The tile's headline value is the total across the children. */
            double sum = 0;
            for (int k = 0; k < m->n_children; k++) sum += m->child_value[k];
            shown = sum;
        } else if (w->q > 0.0f) {
            if (sc->nb >= 2) {
                /* Sort by bound with +Inf last, then clamp: a scrape racing a
                 * concurrent observation can return non-monotonic counts, and
                 * the interpolation would divide by a negative denominator. */
                for (int a = 1; a < sc->nb; a++) {
                    double kl = sc->le[a], kc = sc->cum[a];
                    int b = a - 1;
                    while (b >= 0 && sc->le[b] > kl) {
                        sc->le[b + 1] = sc->le[b];
                        sc->cum[b + 1] = sc->cum[b];
                        b--;
                    }
                    sc->le[b + 1] = kl; sc->cum[b + 1] = kc;
                }
                prom_hist_repair(sc->cum, sc->nb);

                /*
                 * Difference against the baseline so the quantile describes
                 * the last window_s of observations rather than the process's
                 * whole life. window_s == 0 keeps the all-time behaviour.
                 */
                double use_cum[PROM_MAX_BUCKETS];
                int    use_nb = sc->nb;
                bool   ready  = true;

                if (w->window_s > 0) {
                    bool same = (w->base_nb == sc->nb);
                    for (int b = 0; same && b < sc->nb; b++) {
                        if (w->base_le[b] != sc->le[b]) same = false;
                    }

                    if (!same) {
                        /* Bucket bounds changed (or first sight): re-baseline
                         * and show nothing rather than differencing against a
                         * distribution that no longer exists. */
                        ready = false;
                    } else {
                        for (int b = 0; b < sc->nb; b++) {
                            use_cum[b] = sc->cum[b] - w->base_cum[b];
                            /* A negative delta means the exporter restarted;
                             * the whole window is discarded rather than
                             * producing a garbage quantile from it. */
                            if (use_cum[b] < 0) ready = false;
                        }
                        if (ready && use_cum[use_nb - 1] <= 0) ready = false;
                    }

                    if (!same || (t - w->base_t) >= (int64_t)w->window_s * 1000) {
                        memcpy(w->base_cum, sc->cum, sizeof(double) * sc->nb);
                        memcpy(w->base_le,  sc->le,  sizeof(double) * sc->nb);
                        w->base_nb = sc->nb;
                        w->base_t  = t;
                    }
                } else {
                    memcpy(use_cum, sc->cum, sizeof(double) * sc->nb);
                }

                if (!ready) {
                    m->warming = true;
                } else {
                    shown  = prom_hist_quantile((double)w->q, sc->le, use_cum, use_nb);
                    m->p50 = (float)prom_hist_quantile(0.50, sc->le, use_cum, use_nb);
                    m->p90 = (float)prom_hist_quantile(0.90, sc->le, use_cum, use_nb);
                    m->p99 = (float)prom_hist_quantile(0.99, sc->le, use_cum, use_nb);
                }

                double total = ready ? use_cum[use_nb - 1] : 0;
                if (total > 0) {
                    /* Merge down to what a tile can actually draw: keep the
                     * first N-1 bounds and lump everything above into the
                     * last bar, which keeps the shares summing to 1. */
                    int keep = sc->nb < POLLER_MAX_BUCKETS ? sc->nb
                                                           : POLLER_MAX_BUCKETS;
                    double prev = 0;
                    for (int b = 0; b < keep; b++) {
                        bool last = (b == keep - 1);
                        double cum = last ? total : use_cum[b];
                        m->bucket_le[b]    = last ? (float)INFINITY
                                                  : (float)sc->le[b];
                        m->bucket_share[b] = (float)((cum - prev) / total);
                        prev = cum;
                    }
                    m->n_buckets = (uint8_t)keep;
                    m->has_hist  = true;
                }
            }
        } else if (w->agg == AGG_RATE && w->window_s > 0) {
            /* Windowed rate: difference against the oldest sample still
             * inside the window, so a stepped counter reads steadily. */
            if (!prom_is_num(sc->value)) {
                w->win_n = 0; w->win_valid = false;
            } else {
                double v = sc->value.num;

                /* A counter that went backwards restarted; the ring describes
                 * a series that no longer exists. */
                if (w->win_n > 0 &&
                    v < w->win_v[(w->win_head + RATE_WIN_MAX - 1) % RATE_WIN_MAX]) {
                    w->win_n = 0; w->win_valid = false;
                    m->restarted = true;
                }

                w->win_v[w->win_head] = v;
                w->win_t[w->win_head] = t;
                w->win_head = (uint8_t)((w->win_head + 1) % RATE_WIN_MAX);
                if (w->win_n < RATE_WIN_MAX) w->win_n++;

                int64_t cutoff = t - (int64_t)w->window_s * 1000;
                int     best = -1;
                for (int k = 0; k < w->win_n; k++) {
                    uint8_t idx = (uint8_t)((w->win_head + RATE_WIN_MAX - 1 - k)
                                            % RATE_WIN_MAX);
                    best = idx;
                    if (w->win_t[idx] <= cutoff) break;
                }
                if (best >= 0) {
                    int64_t dt = t - w->win_t[best];
                    /* Need a real span before the number means anything; below
                     * that the tile says it is warming rather than showing a
                     * rate derived from two adjacent samples. */
                    if (dt >= 2000) {
                        w->win_last  = (float)((v - w->win_v[best]) * 1000.0 / (double)dt);
                        w->win_valid = true;
                    }
                }
                if (w->win_valid) shown = w->win_last;
                else              m->warming = true;
            }
        } else switch (w->agg) {
        case AGG_LAST:
            if (prom_is_num(sc->value)) shown = sc->value.num;
            break;

        case AGG_RATE: {
            float rate = 0;
            /* 3x the interval: past that, averaging across the gap produces a
             * technically correct and deeply misleading number. */
            rate_status_t rc = prom_rate_step(&w->rate, sc->value, t,
                                              (int64_t)s_interval_s * 3000, &rate);
            if (rc == RATE_OK) {
                shown = rate;
            } else if (rc == RATE_RESET) {
                /*
                 * Detected, flagged, but NOT displayed.
                 *
                 * Prometheus reset semantics are delta = v_now, which is right
                 * for a query engine but wrong for a glanceable panel: a
                 * counter that restarts at a large value yields one enormous
                 * sample, and a single such point flattens an auto-scaled
                 * chart's whole range for as long as it stays in the ring.
                 * Measured at 2.24 TiB/s against a genuine 8 MiB/s baseline.
                 *
                 * Skipping the point costs the "served N since restart"
                 * datum, which nobody reads off a sparkline, and leaves an
                 * honest one-sample break instead.
                 */
                m->restarted = true;
            } else if (rc == RATE_WARMING) {
                m->warming = true;
            }
            break;
        }

        default:
            if (prom_is_num(sc->value)) shown = sc->value.num;
            break;
        }

        if (isfinite(shown)) {
            bool numeric = true;
            ui_fmt_value(shown, w->fmt, w->unit, &w->fmt_state,
                         m->num, sizeof(m->num),
                         m->suffix, sizeof(m->suffix), &numeric);
            /* Percent modes scale for display; charts and gauges want the
             * same quantity the label shows, so they see the scaled one. */
            m->value = (w->fmt == FMT_PCT_01) ? (float)(shown * 100.0)
                                              : (float)shown;
            m->numeric_only = numeric;
            m->valid = true;
        }
    }

    if (esp_log_level_get(TAG) >= ESP_LOG_INFO && ok) {
        char line[256]; size_t w = 0;
        line[0] = '\0';          /* an empty watch list must not print the
                                  * uninitialised buffer */
        for (int i = 0; i < s_watch_n && w < sizeof(line) - 1; i++) {
            const poller_metric_t *m = &next.m[i];
            int n = snprintf(line + w, sizeof(line) - w, "%s=%s%s%s  ",
                             m->label,
                             m->valid ? m->num
                                      : (m->restarted ? "restarted"
                                                      : m->warming ? "warming" : "--"),
                             m->valid && m->suffix[0] ? " " : "",
                             m->valid ? m->suffix : "");
            if (n < 0) break;
            w += (size_t)n;
        }
        if (line[0]) ESP_LOGI(TAG, "%s", line);
    }

    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        next.generation  = s_snap.generation + 1;
        next.fail_streak = ok ? 0 : s_snap.fail_streak + 1;
        next.last_ok_ms  = ok ? t : s_snap.last_ok_ms;
        s_snap = next;
        xSemaphoreGive(s_mux);
    }
}

/* -------------------------------------------------------------------- task */

void poller_set_endpoint(const char *url, int interval_s)
{
    if (url == NULL) return;
    if (s_mux && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        strncpy(s_url, url, sizeof(s_url) - 1);
        s_url[sizeof(s_url) - 1] = '\0';
        s_interval_s = interval_s > 0 ? interval_s : 10;
        s_snap.fail_streak = 0;      /* a new target starts with a clean slate */
        xSemaphoreGive(s_mux);
    } else {
        strncpy(s_url, url, sizeof(s_url) - 1);
        s_interval_s = interval_s > 0 ? interval_s : 10;
    }
    /* The cached connection belongs to the old host. */
    http_drop_slot(0);
    /* Rates measured against the old target are meaningless for the new one. */
    for (int i = 0; i < s_watch_n; i++) {
        memset(&s_watch[i].rate, 0, sizeof(s_watch[i].rate));
        memset(&s_watch[i].fmt_state, 0, sizeof(s_watch[i].fmt_state));
    }
    ESP_LOGI(TAG, "endpoint set to %s every %ds", url, s_interval_s);
}

const char *poller_url(void) { return s_url; }

static void poller_task(void *arg)
{
    (void)arg;

    const prom_text_sink_t sink = { NULL, NULL, on_sample };
    /* Explicit PSRAM: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 would put the
     * 4KB line buffer in the scarce internal heap. */
    prom_text_parser_t *parser = prom_text_new(&sink, NULL,
                                               psram_alloc, psram_free);

    if (parser == NULL) {
        ESP_LOGE(TAG, "parser allocation failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /*
         * WiFi down is not an endpoint failure: skip entirely rather than
         * burning the failure streak, so a brief outage does not leave the
         * endpoint in a long backoff once the link returns.
         */
        if (!wifi_mgr_is_connected()) {
            publish(false, "waiting for wi-fi", 0, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_url[0] == '\0') {
            publish(false, "no endpoint configured", 0, NULL, 0);
            /*
             * Rate-limited, but present: an idle device that logs nothing at
             * all is indistinguishable over serial from a hung one, and this
             * is the state a factory-fresh panel sits in.
             */
            static int quiet;
            if (quiet++ % 30 == 0) {
                ESP_LOGW(TAG, "no endpoint configured -- set one with the "
                              "gear button (SRAM %uK PSRAM %uK)",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        memset(s_scratch, 0, sizeof(s_scratch));
        prom_text_reset(parser);

        http_result_t res;
        http_get_stream(0, s_url, NULL, feed_chunk, parser, 8000, &res);

        prom_text_stats_t st;
        prom_text_finish(parser, &st);

        if (res.klass == HTTP_ERR_NONE && st.samples > 0) {
            publish(true, "ok", res.duration_ms, &st, res.bytes);
            /* Heap alongside size: the whole claim of a streaming parser is
             * that these two numbers are unrelated. */
            ESP_LOGI(TAG, "scrape ok: %u samples, %u bytes, %ums  "
                          "SRAM %uK PSRAM %uK",
                     (unsigned)st.samples, (unsigned)res.bytes,
                     (unsigned)res.duration_ms,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        } else if (res.klass == HTTP_ERR_NONE) {
            /* 200 with nothing parseable almost always means the URL points
             * at a web page rather than an exporter. */
            publish(false, "no metrics found", res.duration_ms, &st, res.bytes);
            ESP_LOGW(TAG, "200 but 0 samples from %u bytes", (unsigned)res.bytes);
        } else {
            publish(false, http_err_text(res.klass), res.duration_ms, &st, res.bytes);
        }

        uint32_t wait_ms = (uint32_t)s_interval_s * 1000;
        uint32_t streak;
        if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            streak = s_snap.fail_streak;
            xSemaphoreGive(s_mux);
            if (streak > 0) {
                uint32_t shift = streak > 5 ? 5 : streak;
                wait_ms <<= shift;
                if (wait_ms > 300000) wait_ms = 300000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t poller_start(const char *url, int interval_s)
{
    if (url == NULL) url = "";
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_interval_s = interval_s > 0 ? interval_s : 10;

    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_ERR_NO_MEM;

    strncpy(s_snap.status, "starting", sizeof(s_snap.status) - 1);
    poller_reload();

    /* Core 0, above the LVGL task's priority 2 on core 1: network work
     * preempts nothing that draws. 8KB covers TLS handshake depth with room. */
    BaseType_t rc = xTaskCreatePinnedToCore(poller_task, "poller", 8192, NULL,
                                            5, NULL, 0);
    return rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void poller_snapshot(poller_snap_t *out)
{
    if (out == NULL) return;

    /*
     * The UI is built before the poller task starts, so this must be safe
     * with no mutex yet. Returning a zeroed snapshot with the labels filled
     * in lets the dashboard lay itself out before any data exists.
     */
    if (s_mux == NULL) {
        memset(out, 0, sizeof(*out));
        out->n = s_watch_n;
        for (int i = 0; i < s_watch_n; i++) {
            strncpy(out->m[i].label, s_watch[i].label, sizeof(out->m[i].label) - 1);
        }
        strncpy(out->status, "starting", sizeof(out->status) - 1);
        return;
    }

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_mux);
    }
}

uint16_t poller_panel_id(int idx)
{
    return (idx >= 0 && idx < s_watch_n) ? s_watch[idx].panel_id : 0;
}

/*
 * Rebuild the watch list from the stored panels, preserving rate baselines
 * for selectors that survive the edit.
 *
 * Preserving them matters: without it, ticking one new metric would reset
 * every counter on the screen to "warming up" and blank the rates for a full
 * poll interval, which looks exactly like a fault.
 */
void poller_reload(void)
{
    const config_t *c = config_get();

    /*
     * Preserve only what actually needs carrying across a reload.
     *
     * Copying the whole watch array would be ~10KB of stack -- each entry
     * holds a 160-byte selector scratch plus its parsed labels -- and this
     * runs on app_main's 8KB stack at boot and on the LVGL task's 6KB stack
     * when a selection changes. Both overflow. The baselines are 32 bytes an
     * entry.
     */
    struct { uint16_t id; rate_state_t rate, rate_b; fmt_state_t fmt; }
        prev[POLLER_MAX_WATCH];
    int prev_n = s_watch_n;
    for (int i = 0; i < prev_n; i++) {
        prev[i].id     = s_watch[i].panel_id;
        prev[i].rate   = s_watch[i].rate;
        prev[i].rate_b = s_watch[i].rate_b;
        prev[i].fmt    = s_watch[i].fmt_state;
    }

    memset(s_watch, 0, sizeof(s_watch));
    s_watch_n = 0;

    for (int i = 0; i < c->n_panels && s_watch_n < POLLER_MAX_WATCH; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (p->sel[0] == '\0') continue;
        /* Must use the SAME predicate as the dashboard's tile builder: slot i
         * here is tile i there, and a mismatch silently pairs a tile with
         * another metric's numbers. */
        if (p->screen != 0) continue;

        watch_rt_t *w = &s_watch[s_watch_n];
        w->panel_id = p->id;
        w->agg      = p->agg;
        w->fmt      = p->fmt;
        w->q        = p->q;
        w->window_s = p->window_s;
        w->multi    = p->multi;
        w->op       = p->op;
        if (w->op != OP_NONE && p->sel_b[0]) {
            strncpy(w->scratch_b, p->sel_b, sizeof(w->scratch_b) - 1);
            if (!prom_parse_selector(w->scratch_b, w->scratch_b,
                                     sizeof(w->scratch_b),
                                     &w->name_b, &w->name_b_len,
                                     w->labels_b, 8, &w->n_labels_b)) {
                ESP_LOGW(TAG, "unparseable second operand: %s", p->sel_b);
                w->op = OP_NONE;
            }
        } else if (w->op != OP_NONE) {
            w->op = OP_NONE;      /* an operator with nothing to operate on */
        }
        strncpy(w->unit, p->unit, sizeof(w->unit) - 1);
        strncpy(w->scratch, p->sel, sizeof(w->scratch) - 1);

        if (!prom_parse_selector(w->scratch, w->scratch, sizeof(w->scratch),
                                 &w->name, &w->name_len,
                                 w->labels, 8, &w->n_labels)) {
            ESP_LOGW(TAG, "unparseable selector, skipping: %s", p->sel);
            continue;
        }

        /* A blank title falls back to the metric name rather than showing an
         * empty tile -- the name is always better than nothing. */
        if (p->title[0]) {
            strncpy(w->label, p->title, sizeof(w->label) - 1);
        } else {
            size_t n = w->name_len < sizeof(w->label) - 1 ? w->name_len
                                                          : sizeof(w->label) - 1;
            memcpy(w->label, w->name, n);
            w->label[n] = '\0';
        }

        for (int j = 0; j < prev_n; j++) {
            if (prev[j].id == w->panel_id) {
                w->rate      = prev[j].rate;
                w->rate_b    = prev[j].rate_b;
                w->fmt_state = prev[j].fmt;
                break;
            }
        }

        w->active = true;
        s_watch_n++;
    }

    ESP_LOGI(TAG, "watching %d series", s_watch_n);
}

uint32_t poller_generation(void)
{
    return s_snap.generation;   /* a torn read only costs one extra repaint */
}
