#include "poller.h"

#include "http_util.h"
#include "prom_math.h"
#include "poller_terms.h"
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

/*
 * The watch list, rebuilt from the stored panels.
 *
 * A watch is a panel's worth of terms plus the state needed to present them.
 * Every panel has at least one term; a derived panel has two or more and an
 * op that combines them.
 */
typedef struct {
    uint16_t     panel_id;
    /* Which endpoint feeds this watch: its index in s_ep, and enough about
     * the endpoint to tell whether a reload changed where the data comes
     * from -- a baseline measured against another host is not a baseline. */
    uint8_t      ep_idx;
    uint16_t     ep_id;
    uint32_t     ep_url_hash;
    /* True when reload carried every term's state over, so the row already
     * in the snapshot is still about this watch's numbers. */
    bool         carried;
    char         label[POLLER_NAME_MAX];
    fmt_mode_t   fmt;
    char         unit[8];
    int8_t       scale;          /* pinned prefix, or FMT_PIN_AUTO */
    bool         group;          /* thousands separators */
    char         prefix[8], suffix[8];
    /*
     * The largest displayed value seen since this term was defined.
     *
     * For the gauges whose full scale nobody can look up -- concurrent
     * requests against a limit the server does not export -- the highest
     * reading so far is the only honest 100%. It never decays: "the most this
     * has ever been" is the question being asked.
     */
    double       peak;
    panel_op_t   op;
    bool         multi;
    uint8_t      n_terms;
    term_rt_t    terms[CFG_MAX_TERMS];

    fmt_state_t  fmt_state;

    /* Multi-series children, keyed by label so a baseline survives a series
     * appearing or disappearing between scrapes. */
    char         child_key[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    rate_state_t child_rate[POLLER_MAX_CHILDREN];
    /* One per row, so a multi panel's rows honour the same window its single
     * value would have. */
    win_ring_t   child_win[POLLER_MAX_CHILDREN];
    uint8_t      n_child;
    uint8_t      order[POLLER_MAX_CHILDREN];
    uint8_t      resort_in;

    /* Per-scrape child accumulation (terms[0] only). */
    uint8_t      sc_n_child;
    uint8_t      sc_n_matched;
    char         sc_child_label[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    double       sc_child_value[POLLER_MAX_CHILDREN];
} watch_rt_t;

/* PSRAM: two lists of ~10.8KB watches, ~260KB in all, has no business in the
 * internal heap. */
static watch_rt_t *s_watch;
static watch_rt_t *s_watch_prev;
static int         s_watch_n;

/*
 * The endpoints being polled, rebuilt with the watch list. Each keeps its own
 * schedule and backoff, so a dead exporter slows only its own screens. Index
 * i also names keep-alive slot i. PSRAM, like the watches.
 */
typedef struct {
    uint16_t id;
    char     url[CFG_URL_MAX];
    uint32_t url_hash;
    uint16_t interval_s;
    uint16_t timeout_ms;
    int64_t  next_due_ms;
    uint32_t fail_streak;
    uint8_t  n_watches;
    /* New, or its URL changed in the last reload: whatever status it had is
     * about another target. */
    bool     retargeted;
} ep_rt_t;

static ep_rt_t *s_ep;
static int      s_ep_n;
/* The endpoint being scraped right now; on_sample only feeds its watches. */
static int      s_cur_ep = -1;

static SemaphoreHandle_t s_mux;
static poller_snap_t     s_snap;
static uint32_t          s_cfg_gen;
static uint32_t          s_seq;
static TaskHandle_t      s_task;
/* Reload scratch for re-ordering snapshot rows to match the new watch list. */
static poller_metric_t  *s_rows_tmp;

/*
 * poller_snap_t is passed around by value in places and has twice now grown
 * past a task stack after an innocuous-looking field was added to
 * poller_metric_t. Fail the build rather than the device.
 */
_Static_assert(sizeof(poller_snap_t) < 12 * 1024,
               "poller_snap_t is large; every holder must be static, never a "
               "stack local -- see publish() and dashboard_tick()");

static void reload_watches(void);
/* Set from any task; acted on by the scrape loop, which owns s_watch. */
static volatile bool s_reload_req;

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
/*
 * Glob match over a label VALUE: '*' stands for any run of characters.
 *
 * Only '*' is supported, and only in values. That covers mode="prefill_*"
 * and instance="node-*", which is what selecting a set of series actually
 * needs; a full regex engine would be several times the code for cases nobody
 * has asked for.
 */
static bool glob_match(const char *pat, uint16_t plen,
                       const char *str, uint16_t slen)
{
    uint16_t p = 0, sIdx = 0, star = 0xFFFF, mark = 0;
    while (sIdx < slen) {
        if (p < plen && (pat[p] == '?' || pat[p] == str[sIdx])) { p++; sIdx++; }
        else if (p < plen && pat[p] == '*') { star = p++; mark = sIdx; }
        else if (star != 0xFFFF) { p = (uint16_t)(star + 1); sIdx = ++mark; }
        else return false;
    }
    while (p < plen && pat[p] == '*') p++;
    return p == plen;
}

static bool label_value_eq(const prom_label_t *want, const prom_label_t *got,
                           bool use_glob)
{
    if (use_glob) return glob_match(want->val, want->val_len,
                                    got->val, got->val_len);
    return want->val_len == got->val_len &&
           memcmp(want->val, got->val, want->val_len) == 0;
}

/* Every label on the term must be present on the sample and match. Extra
 * labels on the sample are fine -- a selector is a filter, not an exact-match
 * requirement. */
static bool term_matches(const prom_sample_t *s, const term_rt_t *tm)
{
    for (uint8_t i = 0; i < tm->n_labels; i++) {
        bool found = false;
        for (uint8_t j = 0; j < s->n_labels; j++) {
            if (s->labels[j].key_len != tm->labels[i].key_len) continue;
            if (memcmp(s->labels[j].key, tm->labels[i].key,
                       tm->labels[i].key_len) != 0) continue;
            found = label_value_eq(&tm->labels[i], &s->labels[j], tm->has_glob);
            break;
        }
        if (!found) return false;
    }
    return true;
}

static bool name_is(const prom_sample_t *s, const char *want, uint16_t len)
{
    return s->name_len == len && memcmp(s->name, want, len) == 0;
}

static bool base_is(const prom_sample_t *s, const char *want, uint16_t len)
{
    return s->base_len == len && memcmp(s->base_name, want, len) == 0;
}

/* Fold one matching sample into the term, per its reducer. */
static void term_accumulate(term_rt_t *tm, double v)
{
    if (!tm->seen) {
        tm->acc   = v;
        tm->n_acc = 1;
        tm->seen  = true;
        return;
    }
    tm->n_acc++;
    switch (tm->reduce) {
    case RED_SUM:
    case RED_AVG:   tm->acc += v; break;
    case RED_MIN:   if (v < tm->acc) tm->acc = v; break;
    case RED_MAX:   if (v > tm->acc) tm->acc = v; break;
    case RED_COUNT: break;                 /* n_acc is the answer */
    case RED_FIRST:
    default:        break;                 /* keep the first */
    }
}

static double term_reduced(const term_rt_t *tm)
{
    if (!tm->seen) return NAN;
    switch (tm->reduce) {
    case RED_AVG:   return tm->n_acc ? tm->acc / tm->n_acc : NAN;
    case RED_COUNT: return (double)tm->n_acc;
    default:        return tm->acc;
    }
}

/*
 * Name a child by the labels that actually distinguish it -- the ones the
 * term's filter does not already pin.
 */
static void child_label_of(const prom_sample_t *s, const term_rt_t *tm,
                           char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < s->n_labels && o + 1 < cap; i++) {
        bool pinned = false;
        for (uint8_t j = 0; j < tm->n_labels; j++) {
            if (s->labels[i].key_len == tm->labels[j].key_len &&
                memcmp(s->labels[i].key, tm->labels[j].key,
                       s->labels[i].key_len) == 0) { pinned = true; break; }
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

static bool on_sample(void *ctx, const prom_sample_t *s)
{
    (void)ctx;
    for (int i = 0; i < s_watch_n; i++) {
        watch_rt_t *w = &s_watch[i];
        if (w->ep_idx != s_cur_ep) continue;

        for (int k = 0; k < w->n_terms; k++) {
            term_rt_t *tm = &w->terms[k];
            if (!tm->active) continue;

            if (tm->q > 0.0f) {
                if (s->role != PROM_ROLE_BUCKET) continue;
                if (!base_is(s, tm->name, tm->name_len)) continue;
                if (!term_matches(s, tm)) continue;
                if (!prom_is_num(s->value)) continue;

                double bound = prom_is_num(s->le) ? s->le.num
                             : (s->le.kind == PVAL_POS_INF ? INFINITY : NAN);
                if (isnan(bound)) continue;

                /* Deduplicate by bound and SUM across matching series, so a
                 * glob over several label sets yields their combined
                 * distribution rather than whichever one came last. */
                int slot = -1;
                for (int b = 0; b < tm->nb; b++) {
                    if (tm->le[b] == bound) { slot = b; break; }
                }
                if (slot < 0) {
                    if (tm->nb >= TERM_MAX_BUCKETS) continue;
                    slot = tm->nb++;
                    tm->le[slot] = bound;
                    tm->cum[slot] = 0;
                }
                tm->cum[slot] += s->value.num;
                tm->seen = true;
                continue;
            }

            if (!name_is(s, tm->name, tm->name_len)) continue;
            if (!term_matches(s, tm)) continue;
            if (!prom_is_num(s->value)) continue;

            /* Multi applies to the first term only: showing every series of
             * a ratio's denominator separately is not a thing anyone means. */
            if (w->multi && k == 0) {
                char key[POLLER_CHILD_LABEL];
                child_label_of(s, tm, key, sizeof(key));
                int slot = -1;
                for (int c = 0; c < w->sc_n_child; c++) {
                    if (strcmp(w->sc_child_label[c], key) == 0) { slot = c; break; }
                }
                if (slot < 0) {
                    w->sc_n_matched++;
                    if (w->sc_n_child >= POLLER_MAX_CHILDREN) continue;
                    slot = w->sc_n_child++;
                    strncpy(w->sc_child_label[slot], key, POLLER_CHILD_LABEL - 1);
                }
                w->sc_child_value[slot] = s->value.num;
                tm->seen = true;
                continue;
            }

            term_accumulate(tm, s->value.num);
        }
    }
    return true;
}

static bool feed_chunk(void *ctx, const char *data, size_t len)
{
    return prom_text_feed((prom_text_parser_t *)ctx, data, len);
}

/* ------------------------------------------------------------- publishing */

/*
 * One step of a windowed rate. Returns false while there is not yet a span
 * worth dividing by.
 *
 * Baselines are stored at the window's own spacing rather than once per poll:
 * the ring holds TERM_WIN_MAX of them, so storing every poll would reach back
 * only poll_interval * 24 and a one-hour window would silently have been a
 * two-minute one. Spacing them at window_s/(TERM_WIN_MAX-1) makes the ring
 * span whatever was asked for, at the cost of resolving the window's start to
 * within one spacing. The current value is never affected -- only the
 * baseline comes from the ring -- and for short windows the spacing lands
 * below the poll interval, so every poll is stored exactly as before.
 */
static bool win_step(win_ring_t *w, double v, int64_t t, uint16_t window_s,
                     float *out, bool *restarted)
{
    uint8_t newest = (uint8_t)((w->head + TERM_WIN_MAX - 1) % TERM_WIN_MAX);
    if (w->n > 0 && v < w->v[newest]) {
        w->n = 0;
        w->valid = false;
        if (restarted) *restarted = true;
    }

    int64_t spacing = ((int64_t)window_s * 1000) / (TERM_WIN_MAX - 1);
    if (w->n == 0 || (t - w->t[newest]) >= spacing) {
        w->v[w->head] = v;
        w->t[w->head] = t;
        w->head = (uint8_t)((w->head + 1) % TERM_WIN_MAX);
        if (w->n < TERM_WIN_MAX) w->n++;
    }

    int64_t cutoff = t - (int64_t)window_s * 1000;
    int best = -1;
    for (int k = 0; k < w->n; k++) {
        uint8_t idx = (uint8_t)((w->head + TERM_WIN_MAX - 1 - k) % TERM_WIN_MAX);
        best = idx;
        if (w->t[idx] <= cutoff) break;
    }
    if (best >= 0) {
        int64_t dt = t - w->t[best];
        /* Needs a real span before the number means anything. */
        if (dt >= 2000) {
            w->last  = (float)((v - w->v[best]) * 1000.0 / (double)dt);
            w->valid = true;
        }
    }
    if (w->valid) { *out = w->last; return true; }
    return false;
}

/*
 * Reduce one term to a single number, applying its aggregation.
 *
 * Each term is rated independently, so rate(sum(x)) is what happens here --
 * the reduction collapses the matching set first, then the rate is taken of
 * that total. For counters that is the right order: summing rates and rating
 * sums agree, but rating the sum survives a series appearing mid-window.
 */
static double term_value(term_rt_t *tm, int64_t t, int64_t max_gap_ms,
                         bool *warming, bool *restarted)
{
    if (tm->q > 0.0f) {
        if (tm->nb < 2) return NAN;

        /* Sort by bound with +Inf last, then clamp: a scrape racing a
         * concurrent observation can return non-monotonic counts, and the
         * interpolation would divide by a negative denominator. */
        for (int a = 1; a < tm->nb; a++) {
            double kl = tm->le[a], kc = tm->cum[a];
            int b = a - 1;
            while (b >= 0 && tm->le[b] > kl) {
                tm->le[b + 1] = tm->le[b];
                tm->cum[b + 1] = tm->cum[b];
                b--;
            }
            tm->le[b + 1] = kl; tm->cum[b + 1] = kc;
        }
        prom_hist_repair(tm->cum, tm->nb);

        double use[TERM_MAX_BUCKETS];
        bool   ready = true;

        if (tm->window_s > 0) {
            bool same = (tm->base_nb == tm->nb);
            for (int b = 0; same && b < tm->nb; b++) {
                if (tm->base_le[b] != tm->le[b]) same = false;
            }
            if (!same) {
                ready = false;    /* bounds changed or first sight */
            } else {
                for (int b = 0; b < tm->nb; b++) {
                    use[b] = tm->cum[b] - tm->base_cum[b];
                    if (use[b] < 0) ready = false;   /* exporter restarted */
                }
                if (ready && use[tm->nb - 1] <= 0) ready = false;
            }
            if (!same || (t - tm->base_t) >= (int64_t)tm->window_s * 1000) {
                memcpy(tm->base_cum, tm->cum, sizeof(double) * (size_t)tm->nb);
                memcpy(tm->base_le,  tm->le,  sizeof(double) * (size_t)tm->nb);
                tm->base_nb = tm->nb;
                tm->base_t  = t;
            }
        } else {
            memcpy(use, tm->cum, sizeof(double) * (size_t)tm->nb);
        }

        if (!ready) { *warming = true; return NAN; }
        return prom_hist_quantile((double)tm->q, tm->le, use, tm->nb);
    }

    double v = term_reduced(tm);
    if (!isfinite(v)) return NAN;

    if (tm->agg != AGG_RATE) return v;

    if (tm->window_s > 0) {
        float r = 0;
        if (win_step(&tm->win, v, t, tm->window_s, &r, restarted)) return r;
        *warming = true;
        return NAN;
    }

    float r = 0;
    rate_status_t rc = prom_rate_step(&tm->rate, prom_num(v), t, max_gap_ms, &r);
    if (rc == RATE_OK)      return r;
    if (rc == RATE_RESET)   { *restarted = true; return NAN; }
    if (rc == RATE_WARMING) { *warming = true; }
    return NAN;
}

/* The fields of a row that describe the panel rather than its numbers. */
static void row_presentation(const watch_rt_t *w, poller_metric_t *m)
{
    m->panel_id = w->panel_id;
    m->scale    = w->scale;
    m->group    = w->group;
    m->peak     = (float)w->peak;
    m->seen_fraction = w->fmt_state.seen_fraction;
    strncpy(m->label, w->label, sizeof(m->label) - 1);
    m->label[sizeof(m->label) - 1] = '\0';
    strncpy(m->unit, w->unit, sizeof(m->unit) - 1);
    m->unit[sizeof(m->unit) - 1] = '\0';
    m->fmt = (uint8_t)w->fmt;
}

/*
 * One watch's row from the scrape that just finished. Everything that was in
 * the row before is replaced, so a row never mixes this scrape with the last.
 */
static void compute_row(watch_rt_t *w, poller_metric_t *m, int64_t t,
                        int64_t max_gap_ms)
{
    memset(m, 0, sizeof(*m));
    row_presentation(w, m);

    bool warming = false, restarted = false;
    double shown = NAN;

    /* The scrape worked; a term that matched nothing is a metric this
     * endpoint does not expose, which is a different fault from one that
     * has not warmed up. */
    bool missing = false;
    for (int k = 0; k < w->n_terms; k++) {
        if (!w->terms[k].seen) missing = true;
    }

    if (w->multi && w->sc_n_child > 0) {
        term_rt_t *t0 = &w->terms[0];
        for (int k = 0; k < w->sc_n_child; k++) {
            int slot = -1;
            for (int j = 0; j < w->n_child; j++) {
                if (strcmp(w->child_key[j], w->sc_child_label[k]) == 0) {
                    slot = j; break;
                }
            }
            if (slot < 0 && w->n_child < POLLER_MAX_CHILDREN) {
                slot = w->n_child++;
                strncpy(w->child_key[slot], w->sc_child_label[k],
                        POLLER_CHILD_LABEL - 1);
            }

            double cv = NAN;
            if (t0->agg == AGG_RATE && slot >= 0) {
                float r = 0;
                /*
                 * The same window the panel asks for. This used to be a
                 * plain per-poll rate whatever window_s said, which on a
                 * counter that steps at its exporter's log interval reads
                 * as a spike then exactly zero, over and over.
                 */
                if (t0->window_s > 0) {
                    if (win_step(&w->child_win[slot], w->sc_child_value[k],
                                 t, t0->window_s, &r, NULL)) cv = r;
                } else if (prom_rate_step(&w->child_rate[slot],
                                   prom_num(w->sc_child_value[k]), t,
                                   max_gap_ms, &r) == RATE_OK) {
                    cv = r;
                }
            } else {
                cv = w->sc_child_value[k];
            }

            strncpy(m->child_label[k], w->sc_child_label[k],
                    POLLER_CHILD_LABEL - 1);
            m->child_value[k] = isfinite(cv) ? (float)cv : 0.0f;
            if (isfinite(cv)) {
                fmt_state_t fs = {0};
                char suf[12]; bool numeric;
                fmt_style_t csy = { w->fmt, w->unit, w->scale, w->group,
                                    w->prefix, w->suffix };
                ui_fmt_value(cv, &csy, &fs,
                             m->child_num[k], sizeof(m->child_num[k]),
                             suf, sizeof(suf), &numeric);
            } else {
                strncpy(m->child_num[k], "--", sizeof(m->child_num[k]) - 1);
            }
        }
        m->n_children = w->sc_n_child;
        m->n_matched  = w->sc_n_matched ? w->sc_n_matched : w->sc_n_child;

        /* Rank by value but hold the order for about a minute: re-sorting
         * every poll makes rows leapfrog and you cannot follow one long
         * enough to read it. */
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

        double sum = 0;
        for (int k = 0; k < m->n_children; k++) sum += m->child_value[k];
        shown = sum;
    } else {
        double v[CFG_MAX_TERMS];
        for (int k = 0; k < w->n_terms; k++) {
            v[k] = term_value(&w->terms[k], t, max_gap_ms, &warming, &restarted);
        }

        switch (w->op) {
        case OP_SHARE: {
            double d = v[0] + v[1];
            /* Both idle is "nothing happened", not 0% -- a hit rate
             * reading zero on a quiet service would be read as a fault. */
            shown = (isfinite(v[0]) && isfinite(v[1]) && d != 0.0)
                  ? v[0] / d : NAN;
            break;
        }
        case OP_RATIO:
            shown = (isfinite(v[0]) && isfinite(v[1]) && v[1] != 0.0)
                  ? v[0] / v[1] : NAN;
            break;
        case OP_DIFF:
            shown = (isfinite(v[0]) && isfinite(v[1])) ? v[0] - v[1] : NAN;
            break;
        case OP_SUM: {
            double acc = 0;
            bool any = false;
            for (int k = 0; k < w->n_terms; k++) {
                if (isfinite(v[k])) { acc += v[k]; any = true; }
            }
            shown = any ? acc : NAN;
            break;
        }
        case OP_NONE:
        default:
            shown = v[0];
            break;
        }

        /* Histogram distribution, for the tile that draws one. */
        term_rt_t *t0 = &w->terms[0];
        if (w->op == OP_NONE && t0->q > 0.0f && t0->nb >= 2) {
            m->p50 = (float)prom_hist_quantile(0.50, t0->le, t0->cum, t0->nb);
            m->p90 = (float)prom_hist_quantile(0.90, t0->le, t0->cum, t0->nb);
            m->p99 = (float)prom_hist_quantile(0.99, t0->le, t0->cum, t0->nb);
            double total = t0->cum[t0->nb - 1];
            if (total > 0) {
                int keep = t0->nb < POLLER_MAX_BUCKETS ? t0->nb
                                                       : POLLER_MAX_BUCKETS;
                double prev = 0;
                for (int b = 0; b < keep; b++) {
                    bool last = (b == keep - 1);
                    double cum = last ? total : t0->cum[b];
                    m->bucket_le[b]    = last ? (float)INFINITY : (float)t0->le[b];
                    m->bucket_share[b] = (float)((cum - prev) / total);
                    prev = cum;
                }
                m->n_buckets = (uint8_t)keep;
                m->has_hist  = true;
            }
        }
    }

    m->restarted = restarted;

    if (isfinite(shown)) {
        bool numeric = true;
        fmt_style_t vsy = { w->fmt, w->unit, w->scale, w->group,
                            w->prefix, w->suffix };
        ui_fmt_value(shown, &vsy, &w->fmt_state,
                     m->num, sizeof(m->num),
                     m->suffix, sizeof(m->suffix), &numeric);
        /*
         * The DISPLAYED quantity, in every mode.
         *
         * Percent was already converted here and a per-hour rate was not,
         * so a chart of an hourly panel plotted per-second numbers while
         * the headline above it read per-hour -- the same series, an axis
         * 3600x out. Anything reading m->value (charts, gauge and bar
         * ranges, the peak) wants what the tile says, not what the
         * formatter will later turn it into.
         */
        double disp = shown;
        if (w->fmt == FMT_PCT_01)         disp = shown * 100.0;
        else if (w->fmt == FMT_RATE_HOUR) disp = shown * 3600.0;
        m->value = (float)disp;
        if (isfinite(m->value) && m->value > w->peak) w->peak = m->value;
        m->peak = (float)w->peak;
        m->seen_fraction = w->fmt_state.seen_fraction;
        m->numeric_only = numeric;
        m->state = MS_OK;
        m->seq = ++s_seq;
    } else if (missing) {
        m->state = MS_MISSING;
    } else if (warming || restarted) {
        m->state = MS_WARMING;
    } else {
        m->state = MS_NO_DATA;
    }
}

static const char *state_word(const poller_metric_t *m)
{
    switch (m->state) {
    case MS_OK:      return m->num;
    case MS_WARMING: return m->restarted ? "restarted" : "warming";
    case MS_MISSING: return "missing";
    default:         return "--";
    }
}

/*
 * Commit one endpoint's scrape.
 *
 * Only that endpoint's rows are recomputed; every other row is carried as it
 * stands, because another endpoint's numbers did not change just because this
 * one was scraped. Rows are kept in watch order (reload_watches re-orders the
 * snapshot to match), so row i is watch i throughout.
 *
 * `ei` < 0 publishes a status to every endpoint without scraping any -- the
 * "waiting for wi-fi" case.
 */
static void publish_ep(int ei, bool ok, const char *status, uint32_t latency_ms,
                       const prom_text_stats_t *st, uint64_t bytes)
{
    /*
     * Static, not a stack local: poller_snap_t carries every watch slot and
     * grew past this task's 8KB stack once already. Only the poller task
     * ever runs this.
     */
    static poller_snap_t next;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return;
    next = s_snap;
    xSemaphoreGive(s_mux);

    int64_t t = now_ms();

    for (int i = 0; i < s_watch_n && i < POLLER_MAX_WATCH; i++) {
        watch_rt_t      *w = &s_watch[i];
        poller_metric_t *m = &next.m[i];
        if (ei >= 0 && w->ep_idx != ei) continue;
        if (ok && ei >= 0) {
            int64_t gap = (int64_t)s_ep[ei].interval_s * 3000;
            compute_row(w, m, t, gap);
        } else {
            memset(m, 0, sizeof(*m));
            row_presentation(w, m);
            m->state = MS_NO_DATA;
        }
    }
    next.n = s_watch_n;

    if (ei >= 0 && ok && esp_log_level_get(TAG) >= ESP_LOG_INFO) {
        char line[256]; size_t w = 0;
        line[0] = '\0';
        for (int i = 0; i < s_watch_n && w < sizeof(line) - 1; i++) {
            if (s_watch[i].ep_idx != ei) continue;
            const poller_metric_t *m = &next.m[i];
            bool v = m->state == MS_OK;
            int n = snprintf(line + w, sizeof(line) - w, "%s=%s%s%s  ",
                             m->label, state_word(m),
                             v && m->suffix[0] ? " " : "",
                             v ? m->suffix : "");
            if (n < 0) break;
            w += (size_t)n;
        }
        if (line[0]) ESP_LOGI(TAG, "[%s] %s", s_ep[ei].url, line);
    }

    for (int e = 0; e < next.n_ep; e++) {
        if (ei >= 0 && e != ei) continue;
        poller_ep_status_t *es = &next.ep[e];
        es->ok = ok;
        strncpy(es->status, status, sizeof(es->status) - 1);
        es->status[sizeof(es->status) - 1] = '\0';
        if (ei < 0) continue;              /* a status, not a scrape */
        es->latency_ms  = latency_ms;
        es->samples     = st ? st->samples : 0;
        es->body_bytes  = bytes;
        es->fail_streak = s_ep[e].fail_streak;
        if (ok) es->last_ok_ms = t;
    }

    if (xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        next.generation = s_snap.generation + 1;
        s_snap = next;
        xSemaphoreGive(s_mux);
    }
}

/* -------------------------------------------------------------------- task */

/* Scrape one endpoint, commit it, and schedule its next turn. */
static void scrape_ep(int ei, prom_text_parser_t *parser)
{
    ep_rt_t *ep = &s_ep[ei];

    /* Clear only this endpoint's per-scrape accumulation; baselines and rings
     * carry over, which is the whole point of them. */
    for (int i = 0; i < s_watch_n; i++) {
        watch_rt_t *w = &s_watch[i];
        if (w->ep_idx != ei) continue;
        w->sc_n_child = w->sc_n_matched = 0;
        for (int k = 0; k < w->n_terms; k++) {
            term_rt_t *tm = &w->terms[k];
            tm->acc = 0; tm->n_acc = 0; tm->seen = false; tm->nb = 0;
        }
    }
    prom_text_reset(parser);

    s_cur_ep = ei;
    http_result_t res;
    http_get_stream(ei, ep->url, NULL, feed_chunk, parser,
                    ep->timeout_ms ? ep->timeout_ms : 8000, &res);
    prom_text_stats_t st;
    prom_text_finish(parser, &st);
    s_cur_ep = -1;

    bool ok = res.klass == HTTP_ERR_NONE && st.samples > 0;
    ep->fail_streak = ok ? 0 : ep->fail_streak + 1;

    if (ok) {
        publish_ep(ei, true, "ok", res.duration_ms, &st, res.bytes);
        /* Heap alongside size: the whole claim of a streaming parser is
         * that these two numbers are unrelated. */
        ESP_LOGI(TAG, "scrape ok: %s %u samples, %u bytes, %ums  "
                      "SRAM %uK PSRAM %uK", ep->url,
                 (unsigned)st.samples, (unsigned)res.bytes,
                 (unsigned)res.duration_ms,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else if (res.klass == HTTP_ERR_NONE) {
        /* 200 with nothing parseable almost always means the URL points
         * at a web page rather than an exporter. */
        publish_ep(ei, false, "no metrics found", res.duration_ms, &st, res.bytes);
        ESP_LOGW(TAG, "%s: 200 but 0 samples from %u bytes", ep->url,
                 (unsigned)res.bytes);
    } else {
        publish_ep(ei, false, http_err_text(res.klass), res.duration_ms, &st,
                   res.bytes);
    }

    /* Per-endpoint backoff, so one exporter that is down is asked less often
     * without slowing anyone else's screens. */
    uint32_t wait_ms = (uint32_t)ep->interval_s * 1000;
    if (ep->fail_streak > 0) {
        uint32_t shift = ep->fail_streak > 5 ? 5 : ep->fail_streak;
        wait_ms <<= shift;
        if (wait_ms > 300000) wait_ms = 300000;
    }
    ep->next_due_ms = now_ms() + wait_ms;
}

/*
 * Sleep until `ms` from now at most, waking early on a reload request. Capped
 * so Wi-Fi and config changes are noticed within a second whatever the poll
 * interval is.
 */
static void wait_or_wake(int64_t ms)
{
    if (ms > 1000) ms = 1000;
    if (ms < 1) ms = 1;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

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
        /* A pushed config lands on the HTTP task; picking it up here means the
         * watch list is only ever rewritten by the task that reads it. */
        uint32_t g = config_generation();
        if (g != s_cfg_gen || s_reload_req) {
            s_cfg_gen = g;
            s_reload_req = false;
            reload_watches();
        }

        /*
         * WiFi down is not an endpoint failure: skip entirely rather than
         * burning the failure streaks, so a brief outage does not leave every
         * endpoint in a long backoff once the link returns.
         */
        if (!wifi_mgr_is_connected()) {
            publish_ep(-1, false, "waiting for wi-fi", 0, NULL, 0);
            wait_or_wake(1000);
            continue;
        }

        int pick = -1;
        for (int e = 0; e < s_ep_n; e++) {
            if (s_ep[e].n_watches == 0) continue;   /* no screen uses it */
            if (pick < 0 || s_ep[e].next_due_ms < s_ep[pick].next_due_ms) pick = e;
        }

        if (pick < 0) {
            /*
             * Rate-limited, but present: an idle device that logs nothing at
             * all is indistinguishable over serial from a hung one, and this
             * is the state a factory-fresh panel sits in.
             */
            static int quiet;
            if (quiet++ % 30 == 0) {
                ESP_LOGW(TAG, "%s", s_ep_n ? "no tiles on any endpoint yet"
                                           : "no endpoint configured -- set "
                                             "one with the gear button");
            }
            wait_or_wake(1000);
            continue;
        }

        int64_t due_in = s_ep[pick].next_due_ms - now_ms();
        if (due_in > 0) {
            wait_or_wake(due_in);
            continue;
        }
        scrape_ep(pick, parser);
    }
}

/* ------------------------------------------------------------------ public */

esp_err_t poller_start(void)
{
    if (s_mux) return ESP_OK;                 /* already running */
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_ERR_NO_MEM;

    reload_watches();          /* before the task exists, so no race */

    /* Core 0, above the LVGL task's priority 2 on core 1: network work
     * preempts nothing that draws. 8KB covers TLS handshake depth with room. */
    BaseType_t rc = xTaskCreatePinnedToCore(poller_task, "poller", 8192, NULL,
                                            5, &s_task, 0);
    return rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void poller_snapshot(poller_snap_t *out)
{
    if (out == NULL) return;

    /*
     * The UI is built before the poller task starts, so this must be safe
     * with no mutex yet: a zeroed snapshot lays out as "no data" everywhere.
     */
    if (s_mux == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_mux);
    }
}

const poller_ep_status_t *poller_ep_status(const poller_snap_t *snap,
                                           uint16_t ep_id)
{
    if (snap == NULL) return NULL;
    for (int e = 0; e < snap->n_ep && e < CFG_MAX_ENDPOINTS; e++) {
        if (snap->ep[e].ep_id == ep_id) return &snap->ep[e];
    }
    return NULL;
}

uint16_t poller_panel_id(int idx)
{
    return (idx >= 0 && idx < s_watch_n) ? s_watch[idx].panel_id : 0;
}

/* A label value containing '*' is matched as a glob rather than compared. */
static bool selector_has_glob(const prom_label_t *l, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        if (memchr(l[i].val, '*', l[i].val_len) != NULL) return true;
    }
    return false;
}

/*
 * Rebuild the watch list from the stored panels, preserving rate baselines
 * for terms that survive the edit.
 *
 * Preserving them matters: without it, ticking one new metric would reset
 * every counter on the screen to "warming up" and blank the rates for a full
 * window, which looks exactly like a fault.
 */
/*
 * Rebuilds the watch list. Poller task only.
 *
 * The public entry below just raises a flag, because this walks every panel
 * and rewrites s_watch in place -- doing that from the LVGL task while a
 * scrape is committing on core 0 is a data race with no symptom until a tile
 * shows another metric's numbers.
 */
/*
 * Do these two terms produce the same series of numbers?
 *
 * Everything that selects data or shapes it: the selector, how the matched
 * set is reduced, whether it is a rate, over what span, and which quantile.
 * Nothing about presentation -- retitling a tile does not invalidate its
 * baseline, and this is what makes that true.
 */
static bool term_def_same(const term_rt_t *a, const term_rt_t *b)
{
    return strcmp(a->sel, b->sel) == 0 &&
           a->reduce   == b->reduce &&
           a->agg      == b->agg &&
           a->window_s == b->window_s &&
           a->q        == b->q;
}

/*
 * Moves accumulated state from the outgoing watch list to the incoming one.
 *
 * A save rewrites every panel, including the eleven that did not change, and
 * rebuilding their state from nothing means a counter with no baseline reads
 * "warming up" and an hour-long window starts its hour again. So state moves
 * whenever the term that produced it is byte-for-byte the term being built --
 * same selector, same reduce, agg, window and quantile. Anything else and the
 * baseline would be about a different series or a different span, which is
 * worse than starting over, because it is wrong rather than merely absent.
 *
 * Terms are matched by definition rather than by position, so removing the
 * first of three terms does not shift the other two onto each other's
 * baselines.
 */
static bool carry_state(watch_rt_t *w, watch_rt_t *old)
{
    bool taken[CFG_MAX_TERMS] = { false };
    bool all = true;

    for (int k = 0; k < w->n_terms; k++) {
        int from = -1;
        for (int j = 0; j < old->n_terms; j++) {
            if (taken[j]) continue;
            if (term_def_same(&old->terms[j], &w->terms[k])) { from = j; break; }
        }
        if (from < 0) { all = false; continue; }
        taken[from] = true;

        term_rt_t *src = &old->terms[from], *dst = &w->terms[k];
        dst->rate = src->rate;

        dst->win = src->win;

        memcpy(dst->base_cum, src->base_cum, sizeof(dst->base_cum));
        memcpy(dst->base_le,  src->base_le,  sizeof(dst->base_le));
        dst->base_nb = src->base_nb;
        dst->base_t  = src->base_t;
    }

    if (!all) return false;

    /*
     * Presentation state, which only makes sense when every term survived:
     * the prefix hysteresis is about the combined number, and the frozen row
     * order of a multi tile is about the set the terms produce.
     */
    w->fmt_state = old->fmt_state;
    /* The peak belongs to the series, so it survives a rename or a resize and
     * starts again when the terms change. */
    w->peak = old->peak;
    if (w->multi == old->multi) {
        memcpy(w->child_key,  old->child_key,  sizeof(w->child_key));
        memcpy(w->child_rate, old->child_rate, sizeof(w->child_rate));
        memcpy(w->child_win,  old->child_win,  sizeof(w->child_win));
        memcpy(w->order,      old->order,      sizeof(w->order));
        w->n_child   = old->n_child;
        w->resort_in = old->resort_in;
    }
    return true;
}

static uint32_t url_hash(const char *u)
{
    uint32_t h = 2166136261u;                     /* FNV-1a */
    for (; *u; u++) { h ^= (uint8_t)*u; h *= 16777619u; }
    return h;
}

/*
 * Rebuild the endpoint table from config. Returns false if it cannot be
 * allocated. Endpoints that keep their id keep their schedule and backoff;
 * one whose URL or timeout changed has its keep-alive slot dropped, because
 * the cached connection belongs to the old target.
 */
static bool reload_endpoints(const config_t *c)
{
    static ep_rt_t *prev;
    if (s_ep == NULL) {
        s_ep = heap_caps_calloc(CFG_MAX_ENDPOINTS, sizeof(ep_rt_t), MALLOC_CAP_SPIRAM);
        prev = heap_caps_calloc(CFG_MAX_ENDPOINTS, sizeof(ep_rt_t), MALLOC_CAP_SPIRAM);
        if (s_ep == NULL || prev == NULL) {
            ESP_LOGE(TAG, "cannot allocate the endpoint table");
            return false;
        }
    }
    int prev_n = s_ep_n;
    memcpy(prev, s_ep, sizeof(ep_rt_t) * CFG_MAX_ENDPOINTS);
    memset(s_ep, 0, sizeof(ep_rt_t) * CFG_MAX_ENDPOINTS);
    s_ep_n = 0;

    int64_t now = now_ms();
    for (int e = 0; e < c->n_endpoints && s_ep_n < CFG_MAX_ENDPOINTS; e++) {
        const cfg_endpoint_t *ce = &c->endpoints[e];
        ep_rt_t *ep = &s_ep[s_ep_n];
        ep->id = ce->id;
        strncpy(ep->url, ce->url, sizeof(ep->url) - 1);
        ep->url_hash   = url_hash(ep->url);
        ep->interval_s = ce->poll_s ? ce->poll_s
                       : (c->device.poll_default_s ? c->device.poll_default_s : 10);
        ep->timeout_ms = ce->timeout_ms ? ce->timeout_ms : 8000;
        ep->next_due_ms = now;
        ep->retargeted  = true;

        for (int j = 0; j < prev_n; j++) {
            if (prev[j].id != ep->id) continue;
            if (prev[j].url_hash == ep->url_hash) {
                /* Same target: keep its place in the schedule and its
                 * backoff, so a save does not hammer a dead exporter. */
                ep->next_due_ms = prev[j].next_due_ms;
                ep->fail_streak = prev[j].fail_streak;
                ep->retargeted  = false;
            }
            break;
        }
        /* Slot i now belongs to this endpoint; any other target's socket in
         * it would be reused against the wrong host. */
        if (s_ep_n >= prev_n || prev[s_ep_n].url_hash != ep->url_hash ||
            prev[s_ep_n].timeout_ms != ep->timeout_ms) {
            http_drop_slot(s_ep_n);
        }
        s_ep_n++;
    }
    for (int e = s_ep_n; e < prev_n; e++) http_drop_slot(e);
    return true;
}

static int ep_index(uint16_t id)
{
    for (int e = 0; e < s_ep_n; e++) if (s_ep[e].id == id) return e;
    return -1;
}

static void reload_watches(void)
{
    const config_t *c = config_get();

    /*
     * Two lists, swapped rather than one rewritten in place.
     *
     * The outgoing list has to stay readable while the new one is built,
     * because that is where the baselines come from, and it is far too large
     * to copy onto the calling task's stack -- an earlier version copied a
     * subset for exactly that reason and lost everything it left behind.
     * PSRAM is the resource we have.
     */
    if (s_watch == NULL) {
        s_watch = heap_caps_calloc(POLLER_MAX_WATCH, sizeof(watch_rt_t),
                                   MALLOC_CAP_SPIRAM);
        s_watch_prev = heap_caps_calloc(POLLER_MAX_WATCH, sizeof(watch_rt_t),
                                        MALLOC_CAP_SPIRAM);
        s_rows_tmp = heap_caps_calloc(POLLER_MAX_WATCH, sizeof(poller_metric_t),
                                      MALLOC_CAP_SPIRAM);
        if (s_watch == NULL || s_watch_prev == NULL || s_rows_tmp == NULL) {
            ESP_LOGE(TAG, "cannot allocate the watch list");
            s_watch_n = 0;
            return;
        }
    }
    if (!reload_endpoints(c)) { s_watch_n = 0; return; }

    watch_rt_t *prev = s_watch;
    int prev_n = s_watch_n;
    s_watch = s_watch_prev;
    s_watch_prev = prev;

    memset(s_watch, 0, sizeof(watch_rt_t) * POLLER_MAX_WATCH);
    s_watch_n = 0;

    for (int i = 0; i < c->n_panels && s_watch_n < POLLER_MAX_WATCH; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (p->n_terms == 0 || p->terms[0].sel[0] == '\0') continue;
        /*
         * Every panel, not just the visible screen's.
         *
         * A counter needs a baseline before it can show a rate, so a screen
         * whose watches only start when you swipe to it greets you with a row
         * of "warming up" every time. Watching them all costs a pass over
         * their own endpoint's scrape; with auto-rotate it is also what makes
         * each page land already drawn.
         */
        int ei = ep_index(config_panel_ep(p));
        if (ei < 0) continue;              /* no endpoint to read it from */

        watch_rt_t *w = &s_watch[s_watch_n];
        w->panel_id = p->id;
        w->ep_idx   = (uint8_t)ei;
        w->ep_id    = s_ep[ei].id;
        w->ep_url_hash = s_ep[ei].url_hash;
        w->op       = p->op;
        w->multi    = p->multi;
        w->fmt      = p->fmt;
        w->scale    = p->scale;
        w->group    = p->group;
        strncpy(w->prefix, p->prefix, sizeof(w->prefix) - 1);
        strncpy(w->suffix, p->suffix, sizeof(w->suffix) - 1);
        strncpy(w->unit, p->unit, sizeof(w->unit) - 1);

        bool bad = false;
        for (int k = 0; k < p->n_terms && k < CFG_MAX_TERMS; k++) {
            const cfg_term_t *ct = &p->terms[k];
            if (ct->sel[0] == '\0') continue;
            term_rt_t *tm = &w->terms[w->n_terms];
            tm->reduce   = ct->reduce;
            tm->agg      = ct->agg;
            tm->window_s = ct->window_s;
            tm->q        = ct->q;
            strncpy(tm->sel, ct->sel, sizeof(tm->sel) - 1);
            strncpy(tm->scratch, ct->sel, sizeof(tm->scratch) - 1);
            if (!prom_parse_selector(tm->scratch, tm->scratch, sizeof(tm->scratch),
                                     &tm->name, &tm->name_len,
                                     tm->labels, 8, &tm->n_labels)) {
                ESP_LOGW(TAG, "unparseable selector, skipping panel: %s", ct->sel);
                bad = true;
                break;
            }
            tm->has_glob = selector_has_glob(tm->labels, tm->n_labels);
            tm->active   = true;
            w->n_terms++;
        }
        if (bad || w->n_terms == 0) { memset(w, 0, sizeof(*w)); continue; }

        /* An operator with only one operand is not an operator. */
        if (w->op != OP_NONE && w->n_terms < 2) w->op = OP_NONE;

        if (p->title[0]) {
            strncpy(w->label, p->title, sizeof(w->label) - 1);
        } else {
            size_t n = w->terms[0].name_len < sizeof(w->label) - 1
                     ? w->terms[0].name_len : sizeof(w->label) - 1;
            memcpy(w->label, w->terms[0].name, n);
            w->label[n] = '\0';
        }

        bool found = false;
        for (int j = 0; j < prev_n; j++) {
            if (prev[j].panel_id != w->panel_id) continue;
            found = true;
            /* Same terms read from a different place are different series:
             * a baseline from the old host would be wrong, not just old. */
            if (prev[j].ep_id == w->ep_id &&
                prev[j].ep_url_hash == w->ep_url_hash) {
                w->carried = carry_state(w, &prev[j]);
            }
            break;
        }
        /* Something new to read, so its endpoint is asked now rather than at
         * its next turn -- a tile just placed should not sit empty for a whole
         * poll interval. */
        if (!found || !w->carried) s_ep[ei].next_due_ms = now_ms();

        s_ep[ei].n_watches++;
        s_watch_n++;
    }

    /*
     * Bring the snapshot into the new watch order. A row whose watch carried
     * everything over keeps its numbers; anything else starts as "no data"
     * until its endpoint is next scraped. Endpoint statuses are re-keyed the
     * same way, by id.
     */
    if (s_mux && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < s_watch_n; i++) {
            const watch_rt_t *w = &s_watch[i];
            poller_metric_t *m = &s_rows_tmp[i];
            int from = -1;
            if (w->carried) {
                for (int j = 0; j < s_snap.n && j < POLLER_MAX_WATCH; j++) {
                    if (s_snap.m[j].panel_id == w->panel_id) { from = j; break; }
                }
            }
            if (from >= 0) {
                *m = s_snap.m[from];
            } else {
                memset(m, 0, sizeof(*m));
                m->state = MS_NO_DATA;
            }
            row_presentation(w, m);
        }
        memcpy(s_snap.m, s_rows_tmp, sizeof(poller_metric_t) * (size_t)s_watch_n);
        s_snap.n = s_watch_n;

        poller_ep_status_t old[CFG_MAX_ENDPOINTS];
        int old_n = s_snap.n_ep;
        memcpy(old, s_snap.ep, sizeof(old));
        memset(s_snap.ep, 0, sizeof(s_snap.ep));
        for (int e = 0; e < s_ep_n; e++) {
            poller_ep_status_t *es = &s_snap.ep[e];
            es->ep_id = s_ep[e].id;
            if (!s_ep[e].retargeted) {
                for (int j = 0; j < old_n; j++) {
                    if (old[j].ep_id == es->ep_id) { *es = old[j]; break; }
                }
            }
            if (s_ep[e].n_watches == 0) {
                es->ok = false;
                strncpy(es->status, "idle: no tiles use it", sizeof(es->status) - 1);
            } else if (es->status[0] == '\0' ||
                       strcmp(es->status, "idle: no tiles use it") == 0) {
                strncpy(es->status, "starting", sizeof(es->status) - 1);
            }
        }
        s_snap.n_ep = (uint8_t)s_ep_n;
        s_snap.generation++;
        xSemaphoreGive(s_mux);
    }

    ESP_LOGI(TAG, "watching %d panels across %d endpoints", s_watch_n, s_ep_n);
}


uint32_t poller_generation(void)
{
    return s_snap.generation;   /* a torn read only costs one extra repaint */
}

/*
 * Asks for a rebuild from whichever task noticed the config changed.
 *
 * A flag rather than the work itself: the scrape loop owns s_watch, and the
 * UI calling in would rewrite it mid-scrape. Tiles find their numbers by
 * panel id, so the one cycle of lag shows a tile as warming up rather than
 * showing it the wrong metric.
 */
void poller_reload(void)
{
    s_reload_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}
