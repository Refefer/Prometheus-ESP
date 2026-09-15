#include "poller.h"

#include "http_util.h"
#include "prom_math.h"
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
 * Milestone scope: a fixed watch list rather than the configured selection.
 * The point is to prove the whole pipeline end to end -- HTTP, streaming
 * parse, counter rates, histogram quantiles, formatting -- against live
 * changing data. The metric store replaces this list; the task structure,
 * scheduling and generation-counter handoff around it stay.
 */
typedef enum { W_GAUGE, W_RATE, W_QUANTILE } watch_kind_t;

typedef struct {
    const char  *label;
    const char  *metric;      /* exact sample name, or family for W_QUANTILE */
    const char  *lbl_key;     /* optional single-label filter */
    const char  *lbl_val;
    watch_kind_t kind;
    fmt_mode_t   fmt;
    const char  *unit;
    double       q;           /* W_QUANTILE only */
} watch_t;

static const watch_t k_watch[] = {
    { "Gen tok/s",  "sglang:gen_throughput",           NULL, NULL,
      W_GAUGE, FMT_SI, "tok/s", 0 },
    { "Running",    "sglang:num_running_reqs",         NULL, NULL,
      W_GAUGE, FMT_RAW, "req", 0 },
    { "Queued",     "sglang:num_queue_reqs",           NULL, NULL,
      W_GAUGE, FMT_RAW, "req", 0 },
    /* token_usage is a confirmed 0..1 ratio. cache_hit_rate reads 0.0 right
     * now and its HELP does not say whether it is a ratio or a percentage,
     * so it is deliberately not on this list -- guessing the scale would put
     * a number on the wall that is wrong by 100x. */
    { "KV used",    "sglang:token_usage",              NULL, NULL,
      W_GAUGE, FMT_PCT_01, "", 0 },
    { "KV memory",  "sglang:kv_cache_memory_usage_gb", NULL, NULL,
      W_GAUGE, FMT_RAW, "GB", 0 },
    { "Decode",     "sglang:realtime_tokens_total",    "mode", "decode",
      W_RATE, FMT_RATE_SI, "tok", 0 },
    /* Both latency families are split by is_streaming; without the filter the
     * two distributions would be summed into one that never existed. */
    { "TTFT p99",   "sglang:time_to_first_token_seconds", "is_streaming", "true",
      W_QUANTILE, FMT_DURATION, "", 0.99 },
    { "E2E p99",    "sglang:e2e_request_latency_seconds", "is_streaming", "true",
      W_QUANTILE, FMT_DURATION, "", 0.99 },
};
#define WATCH_N ((int)(sizeof(k_watch) / sizeof(k_watch[0])))

/* Per-watch persistent state across scrapes. */
typedef struct {
    rate_state_t rate;
    fmt_state_t  fmt;
} watch_state_t;

/* Per-scrape accumulation. */
typedef struct {
    bool         seen;
    prom_value_t value;
    /* histogram buckets for the W_QUANTILE watch */
    int          nb;
    double       le[PROM_MAX_BUCKETS];
    double       cum[PROM_MAX_BUCKETS];
} watch_scratch_t;

static watch_state_t   s_state[WATCH_N];
static watch_scratch_t s_scratch[WATCH_N];

static SemaphoreHandle_t s_mux;
static poller_snap_t     s_snap;
static char              s_url[160];
static int               s_interval_s = 10;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* The parser takes its allocator by injection so the host tests can use plain
 * malloc while the device forces PSRAM -- without this,
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 would put the 4KB line buffer in
 * the scarce internal heap. */
static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)   { heap_caps_free(p); }

/* --------------------------------------------------------------- matching */

static bool labels_match(const prom_sample_t *s, const watch_t *w)
{
    if (w->lbl_key == NULL) return true;
    size_t klen = strlen(w->lbl_key), vlen = strlen(w->lbl_val);
    for (uint8_t i = 0; i < s->n_labels; i++) {
        if (s->labels[i].key_len == klen &&
            memcmp(s->labels[i].key, w->lbl_key, klen) == 0) {
            return s->labels[i].val_len == vlen &&
                   memcmp(s->labels[i].val, w->lbl_val, vlen) == 0;
        }
    }
    return false;
}

static bool name_is(const prom_sample_t *s, const char *want)
{
    size_t n = strlen(want);
    return s->name_len == n && memcmp(s->name, want, n) == 0;
}

static bool base_is(const prom_sample_t *s, const char *want)
{
    size_t n = strlen(want);
    return s->base_len == n && memcmp(s->base_name, want, n) == 0;
}

static bool on_sample(void *ctx, const prom_sample_t *s)
{
    (void)ctx;
    for (int i = 0; i < WATCH_N; i++) {
        const watch_t *w = &k_watch[i];

        if (w->kind == W_QUANTILE) {
            /* Collect the family's cumulative buckets; the quantile is
             * derived once the whole body has been seen. */
            if (s->role != PROM_ROLE_BUCKET || !base_is(s, w->metric)) continue;
            /*
             * The label filter applies to buckets as much as to plain
             * samples. A family routinely carries several label sets -- an
             * LLM server splits its latency histogram by is_streaming, for
             * instance -- and merging their buckets produces a quantile over
             * a distribution that does not exist.
             */
            if (!labels_match(s, w)) continue;
            if (!prom_is_num(s->value)) continue;
            watch_scratch_t *sc = &s_scratch[i];
            double bound = prom_is_num(s->le) ? s->le.num
                         : (s->le.kind == PVAL_POS_INF ? INFINITY : NAN);
            if (isnan(bound)) continue;

            /*
             * Deduplicate by bound, last value wins. A well-formed scrape
             * never repeats a bucket, but a buggy exporter (or a proxy that
             * concatenated two scrapes) will, and appending blindly fills the
             * array with duplicates until the real +Inf bucket no longer
             * fits -- which quietly turns the quantile into nonsense.
             */
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

        if (!name_is(s, w->metric) || !labels_match(s, w)) continue;
        s_scratch[i].value = s->value;
        s_scratch[i].seen  = true;
    }
    return true;   /* never abort: we want the parse stats for the whole body */
}

static bool feed_chunk(void *ctx, const char *data, size_t len)
{
    return prom_text_feed((prom_text_parser_t *)ctx, data, len);
}

/* ------------------------------------------------------------- publishing */

static void publish(bool ok, const char *status, uint32_t latency_ms,
                    const prom_text_stats_t *st, uint64_t bytes)
{
    poller_snap_t next = {0};
    next.n          = WATCH_N;
    next.ok         = ok;
    next.latency_ms = latency_ms;
    next.samples    = st ? st->samples : 0;
    next.body_bytes = bytes;
    strncpy(next.status, status, sizeof(next.status) - 1);

    int64_t t = now_ms();

    for (int i = 0; i < WATCH_N; i++) {
        const watch_t   *w  = &k_watch[i];
        watch_scratch_t *sc = &s_scratch[i];
        poller_metric_t *m  = &next.m[i];
        strncpy(m->label, w->label, sizeof(m->label) - 1);

        if (!ok || !sc->seen) {
            m->valid = false;
            continue;
        }

        double shown = NAN;

        switch (w->kind) {
        case W_GAUGE:
            if (prom_is_num(sc->value)) shown = sc->value.num;
            break;

        case W_RATE: {
            float rate = 0;
            /* 3x the interval: past that, averaging across the gap produces a
             * technically correct and deeply misleading number. */
            rate_status_t rc = prom_rate_step(&s_state[i].rate, sc->value, t,
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

        case W_QUANTILE:
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
                shown = prom_hist_quantile(w->q, sc->le, sc->cum, sc->nb);
            }
            break;
        }

        if (isfinite(shown)) {
            bool numeric = true;
            ui_fmt_value(shown, w->fmt, w->unit, &s_state[i].fmt,
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
        for (int i = 0; i < WATCH_N && w < sizeof(line) - 1; i++) {
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
        ESP_LOGI(TAG, "%s", line);
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
    memset(s_state, 0, sizeof(s_state));
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
    s_snap.n = WATCH_N;
    for (int i = 0; i < WATCH_N; i++) {
        strncpy(s_snap.m[i].label, k_watch[i].label, sizeof(s_snap.m[i].label) - 1);
    }

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
        out->n = WATCH_N;
        for (int i = 0; i < WATCH_N; i++) {
            strncpy(out->m[i].label, k_watch[i].label, sizeof(out->m[i].label) - 1);
        }
        strncpy(out->status, "starting", sizeof(out->status) - 1);
        return;
    }

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_mux);
    }
}

const char *poller_label(int idx)
{
    return (idx >= 0 && idx < WATCH_N) ? k_watch[idx].label : "";
}

uint32_t poller_generation(void)
{
    return s_snap.generation;   /* a torn read only costs one extra repaint */
}
