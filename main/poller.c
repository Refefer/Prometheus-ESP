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

/* Enough samples to span the longest offered window at the fastest poll: 15
 * minutes at 2s would need 450, so the ring is capped and the effective
 * window is simply as much history as it holds. */
#define RATE_WIN_MAX 24

/*
 * The watch list, rebuilt from the stored panels.
 *
 * A watch is a panel's worth of terms plus the state needed to present them.
 * Every panel has at least one term; a derived panel has two or more and an
 * op that combines them.
 */
typedef struct {
    uint16_t     panel_id;
    char         label[POLLER_NAME_MAX];
    fmt_mode_t   fmt;
    char         unit[8];
    panel_op_t   op;
    bool         multi;
    uint8_t      n_terms;
    term_rt_t    terms[CFG_MAX_TERMS];

    fmt_state_t  fmt_state;

    /* Multi-series children, keyed by label so a baseline survives a series
     * appearing or disappearing between scrapes. */
    char         child_key[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    rate_state_t child_rate[POLLER_MAX_CHILDREN];
    uint8_t      n_child;
    uint8_t      order[POLLER_MAX_CHILDREN];
    uint8_t      resort_in;

    /* Per-scrape child accumulation (terms[0] only). */
    uint8_t      sc_n_child;
    uint8_t      sc_n_matched;
    char         sc_child_label[POLLER_MAX_CHILDREN][POLLER_CHILD_LABEL];
    double       sc_child_value[POLLER_MAX_CHILDREN];
} watch_rt_t;

/* PSRAM: ~85KB of term state has no business in the internal heap. */
static watch_rt_t *s_watch;
static int         s_watch_n;

static SemaphoreHandle_t s_mux;
static poller_snap_t     s_snap;
static char              s_url[160];
static uint32_t          s_cfg_gen;
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
 * Reduce one term to a single number, applying its aggregation.
 *
 * Each term is rated independently, so rate(sum(x)) is what happens here --
 * the reduction collapses the matching set first, then the rate is taken of
 * that total. For counters that is the right order: summing rates and rating
 * sums agree, but rating the sum survives a series appearing mid-window.
 */
static double term_value(term_rt_t *tm, int64_t t, bool *warming, bool *restarted)
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
        /* Windowed rate: difference against the oldest sample still inside
         * the window, so a counter that only updates on its exporter's log
         * interval reads steadily instead of alternating zero and spike. */
        if (tm->win_n > 0 &&
            v < tm->win_v[(tm->win_head + TERM_WIN_MAX - 1) % TERM_WIN_MAX]) {
            tm->win_n = 0; tm->win_valid = false;
            *restarted = true;
        }
        tm->win_v[tm->win_head] = v;
        tm->win_t[tm->win_head] = t;
        tm->win_head = (uint8_t)((tm->win_head + 1) % TERM_WIN_MAX);
        if (tm->win_n < TERM_WIN_MAX) tm->win_n++;

        int64_t cutoff = t - (int64_t)tm->window_s * 1000;
        int best = -1;
        for (int k = 0; k < tm->win_n; k++) {
            uint8_t idx = (uint8_t)((tm->win_head + TERM_WIN_MAX - 1 - k) % TERM_WIN_MAX);
            best = idx;
            if (tm->win_t[idx] <= cutoff) break;
        }
        if (best >= 0) {
            int64_t dt = t - tm->win_t[best];
            /* Needs a real span before the number means anything. */
            if (dt >= 2000) {
                tm->win_last  = (float)((v - tm->win_v[best]) * 1000.0 / (double)dt);
                tm->win_valid = true;
            }
        }
        if (tm->win_valid) return tm->win_last;
        *warming = true;
        return NAN;
    }

    float r = 0;
    rate_status_t rc = prom_rate_step(&tm->rate, prom_num(v), t,
                                      (int64_t)s_interval_s * 3000, &r);
    if (rc == RATE_OK)      return r;
    if (rc == RATE_RESET)   { *restarted = true; return NAN; }
    if (rc == RATE_WARMING) { *warming = true; }
    return NAN;
}

static void publish(bool ok, const char *status, uint32_t latency_ms,
                    const prom_text_stats_t *st, uint64_t bytes)
{
    /*
     * Static, not a stack local: poller_snap_t carries every watch slot and
     * grew past this task's 8KB stack once already. publish() only ever runs
     * on the poller task.
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
        watch_rt_t      *w = &s_watch[i];
        poller_metric_t *m = &next.m[i];
        strncpy(m->label, w->label, sizeof(m->label) - 1);
        strncpy(m->unit, w->unit, sizeof(m->unit) - 1);
        m->fmt = (uint8_t)w->fmt;

        if (!ok) continue;

        bool warming = false, restarted = false;
        double shown = NAN;

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
                    if (prom_rate_step(&w->child_rate[slot],
                                       prom_num(w->sc_child_value[k]), t,
                                       (int64_t)s_interval_s * 3000, &r) == RATE_OK) {
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
                    ui_fmt_value(cv, w->fmt, w->unit, &fs,
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
                v[k] = term_value(&w->terms[k], t, &warming, &restarted);
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

        m->warming   = warming;
        m->restarted = restarted;

        if (isfinite(shown)) {
            bool numeric = true;
            ui_fmt_value(shown, w->fmt, w->unit, &w->fmt_state,
                         m->num, sizeof(m->num),
                         m->suffix, sizeof(m->suffix), &numeric);
            m->value = (w->fmt == FMT_PCT_01) ? (float)(shown * 100.0)
                                              : (float)shown;
            m->numeric_only = numeric;
            m->valid = true;
        }
    }

    if (esp_log_level_get(TAG) >= ESP_LOG_INFO && ok) {
        char line[256]; size_t w = 0;
        line[0] = '\0';
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
        watch_rt_t *w = &s_watch[i];
        memset(&w->fmt_state, 0, sizeof(w->fmt_state));
        for (int k = 0; k < w->n_terms; k++) {
            term_rt_t *tm = &w->terms[k];
            memset(&tm->rate, 0, sizeof(tm->rate));
            tm->win_n = tm->win_head = 0; tm->win_valid = false;
            tm->base_nb = 0;
        }
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
        /* A pushed config lands on the HTTP task; picking it up here means the
         * watch list is only ever rewritten by the task that reads it. */
        uint32_t g = config_generation();
        if (g != s_cfg_gen) {
            s_cfg_gen = g;
            poller_reload();
        }

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

        /* Clear only the per-scrape accumulation; baselines and rings carry
         * over, which is the whole point of them. */
        for (int i = 0; i < s_watch_n; i++) {
            watch_rt_t *w = &s_watch[i];
            w->sc_n_child = w->sc_n_matched = 0;
            for (int k = 0; k < w->n_terms; k++) {
                term_rt_t *tm = &w->terms[k];
                tm->acc = 0; tm->n_acc = 0; tm->seen = false; tm->nb = 0;
            }
        }
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
void poller_reload(void)
{
    const config_t *c = config_get();

    if (s_watch == NULL) {
        s_watch = heap_caps_calloc(POLLER_MAX_WATCH, sizeof(watch_rt_t),
                                   MALLOC_CAP_SPIRAM);
        if (s_watch == NULL) {
            ESP_LOGE(TAG, "cannot allocate the watch list");
            s_watch_n = 0;
            return;
        }
    }

    /* Carry only the baselines: copying whole watches would be tens of KB on
     * whichever stack called us. */
    struct { uint16_t id; rate_state_t rate[CFG_MAX_TERMS]; fmt_state_t fmt; }
        prev[POLLER_MAX_WATCH];
    int prev_n = s_watch_n;
    for (int i = 0; i < prev_n; i++) {
        prev[i].id  = s_watch[i].panel_id;
        prev[i].fmt = s_watch[i].fmt_state;
        for (int k = 0; k < CFG_MAX_TERMS; k++) prev[i].rate[k] = s_watch[i].terms[k].rate;
    }

    memset(s_watch, 0, sizeof(watch_rt_t) * POLLER_MAX_WATCH);
    s_watch_n = 0;

    for (int i = 0; i < c->n_panels && s_watch_n < POLLER_MAX_WATCH; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (p->n_terms == 0 || p->terms[0].sel[0] == '\0') continue;
        /* Must use the SAME predicate as the dashboard's tile builder: slot i
         * here is tile i there, and a mismatch silently pairs a tile with
         * another metric's numbers. */
        if (p->screen != 0) continue;

        watch_rt_t *w = &s_watch[s_watch_n];
        w->panel_id = p->id;
        w->op       = p->op;
        w->multi    = p->multi;
        w->fmt      = p->fmt;
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

        for (int j = 0; j < prev_n; j++) {
            if (prev[j].id != w->panel_id) continue;
            w->fmt_state = prev[j].fmt;
            for (int k = 0; k < w->n_terms; k++) w->terms[k].rate = prev[j].rate[k];
            break;
        }

        s_watch_n++;
    }

    ESP_LOGI(TAG, "watching %d panels", s_watch_n);
}


uint32_t poller_generation(void)
{
    return s_snap.generation;   /* a torn read only costs one extra repaint */
}
