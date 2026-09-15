/*
 * Prometheus Panel -- boot.
 *
 * M0: bring up the panel, the flash partitions and the metric parser, and put
 * the real memory numbers on screen. The PSRAM readout is not a demo leftover
 * -- internal SRAM is the scarce resource on this board and PSRAM has to hold
 * two 768KB framebuffers plus the series store, so the budget is worth
 * watching from the very first build rather than discovering later.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <math.h>
#include "lvgl.h"

#include "lvgl_port.h"
#include "config.h"
#include "poller.h"
#include "prom_text.h"
#include "secrets.h"
#include "storage.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_endpoints.h"
#include "ui_setup.h"
#include "ui_theme.h"
#include "ui_tile.h"
#include "ui_widgets.h"
#include "waveshare_rgb_lcd_port.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

static char       s_smoke_result[96];

/* ------------------------------------------------- PSRAM-preferring malloc */

/*
 * cJSON trees for PromQL responses are large and transient. Without this hook
 * they land in internal SRAM, where the first real query panics. LVGL gets the
 * same treatment through the LV_MEM_CUSTOM_ALLOC compile options in the root
 * CMakeLists.txt.
 */
static void *psram_prefer_malloc(size_t sz)
{
    return heap_caps_malloc_prefer(sz, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_8BIT);
}

/* The parser takes its allocator by injection precisely so this board can
 * force PSRAM: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 would otherwise put
 * the 4KB line buffer in the internal heap. */
static void *prom_alloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}
static void prom_dealloc(void *p) { heap_caps_free(p); }

/* -------------------------------------------------------- parser smoke test */

/* A miniature exposition body covering one gauge, one counter and one
 * histogram family, so a successful boot proves the whole parse path links
 * and runs on-target, not merely on the host. */
static const char k_smoke[] =
    "# HELP node_load1 1m load average.\n"
    "# TYPE node_load1 gauge\n"
    "node_load1 0.84\n"
    "# TYPE node_cpu_seconds_total counter\n"
    "node_cpu_seconds_total{cpu=\"0\",mode=\"idle\"} 891442.11\n"
    "# TYPE lat_seconds histogram\n"
    "lat_seconds_bucket{le=\"0.005\"} 10\n"
    "lat_seconds_bucket{le=\"+Inf\"} 80\n"
    "lat_seconds_sum 1.25\n"
    "lat_seconds_count 80\n";

typedef struct { uint32_t samples; double load1; } smoke_ctx_t;

static bool smoke_sample(void *ctx, const prom_sample_t *s)
{
    smoke_ctx_t *c = ctx;
    c->samples++;
    if (s->name_len == 10 && memcmp(s->name, "node_load1", 10) == 0 &&
        prom_is_num(s->value)) {
        c->load1 = s->value.num;
    }
    return true;
}

static void run_parser_smoke(char *out, size_t cap)
{
    const prom_text_sink_t sink = { NULL, NULL, smoke_sample };
    smoke_ctx_t ctx = { 0, 0.0 };

    prom_text_parser_t *p = prom_text_new(&sink, &ctx, prom_alloc, prom_dealloc);
    if (p == NULL) { snprintf(out, cap, "parser: alloc failed"); return; }

    /* Feed it in 7-byte chunks so the on-device run exercises the same
     * mid-token chunk boundaries the host tests cover. */
    for (size_t off = 0; off < sizeof(k_smoke) - 1; off += 7) {
        size_t n = (sizeof(k_smoke) - 1) - off;
        if (n > 7) n = 7;
        prom_text_feed(p, k_smoke + off, n);
    }
    prom_text_stats_t st;
    prom_text_finish(p, &st);
    prom_text_free(p);

    snprintf(out, cap, "parser: %u samples, load1=%.2f, errors=%u",
             (unsigned)st.samples, ctx.load1,
             (unsigned)(st.err_bad_value + st.err_bad_label + st.err_too_long));
    ESP_LOGI(TAG, "%s", out);
}

/* ----------------------------------------------------------------- boot UI */


static void reopen_setup_cb(lv_event_t *e)
{
    (void)e;
    ui_setup_open();
}

static lv_obj_t *s_hdr_title;

static void rebuild_dashboard(void)
{
    /* The editor overlay is gone and the tiles underneath are intact, so only
     * the header caption can have changed. */
    config_t *c = config_get();
    label_set_if_changed(s_hdr_title,
                         (c->n_endpoints && c->endpoints[0].name[0])
                         ? c->endpoints[0].name : "Prometheus Panel");
}

static void endpoints_cb(lv_event_t *e)
{
    (void)e;
    ui_endpoints_open(rebuild_dashboard);
}

/*
 * A real tile grid, laid out with the production geometry from ui_layout.h so
 * this milestone validates the grid arithmetic on the actual panel as well as
 * the data path. The watch list behind it is still fixed -- the metric store
 * and the browser replace that; the layout and the refresh path stay.
 */
/*
 * The dashboard.
 *
 * Twelve grid cells filled by eight metrics using spans: a 2x2 chart, a 2x1
 * sparkline, a gauge, a bar and four stat tiles. The watch list behind it is
 * still fixed -- the metric browser replaces that -- but everything from the
 * tile shell down is the production path.
 */
static const tile_spec_t k_tiles[POLLER_MAX_WATCH] = {
    /* title comes from the poller; kind, position, span, range, thresholds */
    { NULL, TILE_CHART,  0, 0, 2, 2, NAN, NAN, NAN,  NAN, false },  /* Gen tok/s */
    { NULL, TILE_STAT,   0, 2, 1, 1, NAN, NAN, NAN,  NAN, false },  /* Running   */
    { NULL, TILE_STAT,   1, 2, 1, 1, NAN, NAN, 8.0f, 20.0f, false },/* Queued    */
    { NULL, TILE_GAUGE,  2, 1, 1, 1, 0.0f, 100.0f, 80.0f, 95.0f, false }, /* KV used */
    { NULL, TILE_BAR,    3, 1, 1, 1, 0.0f, 16.0f, NAN, NAN, false },/* KV memory */
    { NULL, TILE_SPARK,  2, 0, 2, 1, NAN, NAN, NAN,  NAN, false },  /* Decode    */
    { NULL, TILE_STAT,   2, 2, 1, 1, NAN, NAN, NAN,  NAN, false },  /* TTFT p99  */
    { NULL, TILE_STAT,   3, 2, 1, 1, NAN, NAN, NAN,  NAN, false },  /* E2E p99   */
};

static tile_inst_t *s_tiles[POLLER_MAX_WATCH];
static tile_spec_t  s_specs[POLLER_MAX_WATCH];
static lv_obj_t    *s_hdr_status;
static lv_obj_t    *s_ftr_left;
static lv_obj_t    *s_ftr_right;
static uint32_t     s_seen_gen = UINT32_MAX;

static void build_dashboard(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    s_hdr_title = make_label(scr, FONT_L, COL_TEXT);
    config_t *c = config_get();
    lv_label_set_text(s_hdr_title, (c->n_endpoints && c->endpoints[0].name[0])
                                   ? c->endpoints[0].name : "Prometheus Panel");
    lv_obj_set_pos(s_hdr_title, GRID_MX, 8);

    s_hdr_status = make_label(scr, FONT_S, COL_DIM);
    lv_obj_set_pos(s_hdr_status, 300, 14);

    /* Two buttons rather than one menu: with exactly two destinations, a menu
     * is an extra tap and an extra thing to discover. */
    lv_obj_t *wifi = make_btn(scr, LV_SYMBOL_WIFI, reopen_setup_cb, NULL);
    lv_obj_set_size(wifi, 52, 30);
    lv_obj_set_pos(wifi, SCR_W - 110 - GRID_MX, 4);

    lv_obj_t *gear = make_btn(scr, LV_SYMBOL_SETTINGS, endpoints_cb, NULL);
    lv_obj_set_size(gear, 52, 30);
    lv_obj_set_pos(gear, SCR_W - 52 - GRID_MX, 4);

    lv_obj_t *div = make_divider(scr, SCR_W);
    lv_obj_set_pos(div, 0, HEADER_H - 1);

    /* Titles come from the poller's watch table, which is static and valid
     * before the task starts -- the dashboard is built first. */
    for (int i = 0; i < POLLER_MAX_WATCH; i++) {
        s_specs[i] = k_tiles[i];
        s_specs[i].title = poller_label(i);
        s_tiles[i] = tile_create(scr, &s_specs[i]);
    }

    lv_obj_t *fdiv = make_divider(scr, SCR_W);
    lv_obj_set_pos(fdiv, 0, FOOTER_Y);

    s_ftr_left = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_left, GRID_MX, FOOTER_Y + 7);

    s_ftr_right = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_right, SCR_W - 330, FOOTER_Y + 7);
}

/*
 * Runs in the LVGL task. Compares the poller's generation with != rather than
 * > so a uint32 wrap is a non-event, and still repaints the age readout when
 * no new data arrived, because "updated 40s ago" has to keep counting.
 */
static void dashboard_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_tiles[0] == NULL) return;

    poller_snap_t snap;
    poller_snapshot(&snap);

    if (snap.generation != s_seen_gen) {
        s_seen_gen = snap.generation;
        for (int i = 0; i < snap.n && i < POLLER_MAX_WATCH; i++) {
            const poller_metric_t *m = &snap.m[i];
            tile_data_t d = {
                .valid        = m->valid,
                .warming      = m->warming,
                .restarted    = m->restarted,
                .value        = m->value,
                .num          = m->num,
                .suffix       = m->suffix,
                .numeric_only = m->numeric_only,
            };
            tile_update(s_tiles[i], &d);
        }

        char ip[16] = ""; int8_t rssi = 0;
        wifi_mgr_info(ip, sizeof(ip), &rssi);
        label_set_fmt_if_changed(s_hdr_status, "%s   %d dBm", ip, (int)rssi);
        text_color_if_changed(s_hdr_status,
                              wifi_mgr_is_connected() ? COL_OK : COL_CRIT);

        label_set_fmt_if_changed(s_ftr_right,
                                 "%u samples  %u KB  %u ms   SRAM %uK  PSRAM %uK",
                                 (unsigned)snap.samples,
                                 (unsigned)(snap.body_bytes / 1024),
                                 (unsigned)snap.latency_ms,
                                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    poller_snap_t s2;
    poller_snapshot(&s2);
    if (s2.last_ok_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - s2.last_ok_ms) / 1000);
        label_set_fmt_if_changed(s_ftr_left, "updated %ds ago   %s", age, s2.status);
    } else {
        label_set_fmt_if_changed(s_ftr_left, "%s", s2.status);
    }
    text_color_if_changed(s_ftr_left, s2.ok ? COL_DIM : COL_WARN);
}

/* -------------------------------------------------------------------- main */

void app_main(void)
{
    /* First thing in the log after any spontaneous restart: why it happened
     * (4=panic 5=int_wdt 6=task_wdt 9=brownout) */
    ESP_LOGW(TAG, "reset reason: %d", (int)esp_reset_reason());

    cJSON_Hooks hooks = { .malloc_fn = psram_prefer_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "config storage unavailable; settings will not persist");
    }

    /* Panel first, backlight second: the init leaves the backlight off so the
     * framebuffer is already valid before anything is lit, which is what stops
     * the white flash at boot. */
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());
    waveshare_rgb_lcd_bl_on();

    run_parser_smoke(s_smoke_result, sizeof(s_smoke_result));



    /* Radio up before the UI: with no credentials stored the station still
     * starts, which is exactly what the setup wizard's scan needs. */
    if (wifi_mgr_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi stack failed to start");
    }

    if (lvgl_port_lock(-1)) {
        /*
         * Inside the lock: config_load creates the debounced-flush lv_timer,
         * and the dashboard reads the endpoint name for its header.
         */
        if (config_load() != ESP_OK && config_was_reset()) {
            ESP_LOGW(TAG, "starting from factory defaults");
        }
        ui_kbd_init();
        if (secrets_have_wifi()) {
            build_dashboard();
            lv_timer_create(dashboard_tick, 250, NULL);
        } else {
            /* First boot: land straight in setup rather than showing a
             * dashboard that cannot possibly have data. */
            ui_setup_open();
        }
        /* Created regardless of which screen is up: the heartbeat is the only
         * way to see this device's state over serial, since the native-USB
         * console loses everything printed before a host attaches. */
        lvgl_port_unlock();
    }

    if (secrets_have_wifi()) {
        const config_t *c = config_get();
        const char *url = c->n_endpoints ? c->endpoints[0].url : "";
        int interval = c->n_endpoints && c->endpoints[0].poll_s
                     ? c->endpoints[0].poll_s : c->device.poll_default_s;
        poller_start(url, interval);
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "no endpoint configured; use the gear button");
        }
    }

    ESP_LOGI(TAG, "boot complete: SRAM %u KB free, PSRAM %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
