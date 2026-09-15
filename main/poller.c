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
    { "Load 1m",   "node_load1",                      NULL, NULL,
      W_GAUGE, FMT_RAW, "", 0 },
    { "Mem avail", "node_memory_MemAvailable_bytes",  NULL, NULL,
      W_GAUGE, FMT_IEC, "", 0 },
    { "CPU temp",  "node_hwmon_temp_celsius",         "sensor", "temp1",
      W_GAUGE, FMT_RAW, "\xC2\xB0" "C", 0 },
    { "CPU user",  "node_cpu_seconds_total",          "mode", "user",
      W_RATE, FMT_PCT_01, "", 0 },
    /* The float32 trap in one line: this counter is ~1.2e13, and a float
     * subtraction of consecutive samples would be exactly zero. */
    { "Net rx",    "node_network_receive_bytes_total", "device", "eth0",
      W_RATE, FMT_RATE_IEC, "", 0 },
    { "Requests",  "http_requests_total",             "code", "200",
      W_RATE, FMT_RATE_SI, "", 0 },
    { "Latency p99","http_request_duration_seconds",  NULL, NULL,
      W_QUANTILE, FMT_DURATION, "", 0.99 },
    { "Target",    "up",                              NULL, NULL,
      W_GAUGE, FMT_BOOL, "", 0 },
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
            bool numeric;
            ui_fmt_value(shown, w->fmt, w->unit, &s_state[i].fmt,
                         m->num, sizeof(m->num),
                         m->suffix, sizeof(m->suffix), &numeric);
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
    if (url == NULL) return ESP_ERR_INVALID_ARG;
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
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_mux);
    }
}

uint32_t poller_generation(void)
{
    return s_snap.generation;   /* a torn read only costs one extra repaint */
}
