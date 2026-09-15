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

    rate_state_t rate;
    fmt_state_t  fmt_state;
} watch_rt_t;

/* Per-scrape accumulation. */
typedef struct {
    bool   seen;
    prom_value_t value;
    int    nb;
    double le[PROM_MAX_BUCKETS];
    double cum[PROM_MAX_BUCKETS];
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
static bool labels_match(const prom_sample_t *s, const watch_rt_t *w)
{
    for (uint8_t i = 0; i < w->n_labels; i++) {
        bool found = false;
        for (uint8_t j = 0; j < s->n_labels; j++) {
            if (s->labels[j].key_len != w->labels[i].key_len) continue;
            if (memcmp(s->labels[j].key, w->labels[i].key,
                       w->labels[i].key_len) != 0) continue;
            found = (s->labels[j].val_len == w->labels[i].val_len) &&
                    memcmp(s->labels[j].val, w->labels[i].val,
                           w->labels[i].val_len) == 0;
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

        if (!name_is(s, w->name, w->name_len) || !labels_match(s, w)) continue;
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

        if (w->q > 0.0f) {
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
                shown = prom_hist_quantile((double)w->q, sc->le, sc->cum, sc->nb);

                /* Also hand the distribution up, so a histogram can be shown
                 * as one rather than reduced to a single number. */
                m->p50 = (float)prom_hist_quantile(0.50, sc->le, sc->cum, sc->nb);
                m->p90 = (float)prom_hist_quantile(0.90, sc->le, sc->cum, sc->nb);
                m->p99 = (float)prom_hist_quantile(0.99, sc->le, sc->cum, sc->nb);

                double total = sc->cum[sc->nb - 1];
                if (total > 0) {
                    /* Merge down to what a tile can actually draw: keep the
                     * first N-1 bounds and lump everything above into the
                     * last bar, which keeps the shares summing to 1. */
                    int keep = sc->nb < POLLER_MAX_BUCKETS ? sc->nb
                                                           : POLLER_MAX_BUCKETS;
                    double prev = 0;
                    for (int b = 0; b < keep; b++) {
                        bool last = (b == keep - 1);
                        double cum = last ? total : sc->cum[b];
                        m->bucket_le[b]    = last ? (float)INFINITY
                                                  : (float)sc->le[b];
                        m->bucket_share[b] = (float)((cum - prev) / total);
                        prev = cum;
                    }
                    m->n_buckets = (uint8_t)keep;
                    m->has_hist  = true;
                }
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
    struct { uint16_t id; rate_state_t rate; fmt_state_t fmt; }
        prev[POLLER_MAX_WATCH];
    int prev_n = s_watch_n;
    for (int i = 0; i < prev_n; i++) {
        prev[i].id   = s_watch[i].panel_id;
        prev[i].rate = s_watch[i].rate;
        prev[i].fmt  = s_watch[i].fmt_state;
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
