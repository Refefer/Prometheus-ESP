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
#include "ui_browser.h"
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


static void rebuild_dashboard(void);   /* defined with the dashboard below */

static void reopen_setup_cb(lv_event_t *e)
{
    (void)e;
    ui_setup_open();
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
 * Tiles come from the stored panels, and the poller's watch slots are in the
 * same order, so slot i is panel i. Nothing about which metrics appear is
 * compiled in any more.
 */
static tile_inst_t *s_tiles[CFG_MAX_PANELS];
static tile_spec_t  s_specs[CFG_MAX_PANELS];
static int          s_tile_n;
static lv_obj_t    *s_hdr_title;
static lv_obj_t    *s_hdr_status;
static lv_obj_t    *s_ftr_left;
static lv_obj_t    *s_ftr_right;
static lv_obj_t    *s_empty;
static uint32_t     s_seen_gen = UINT32_MAX;

static void build_tiles(lv_obj_t *scr)
{
    for (int i = 0; i < s_tile_n; i++) {
        if (s_tiles[i]) { tile_destroy(s_tiles[i]); s_tiles[i] = NULL; }
    }
    s_tile_n = 0;

    const config_t *c = config_get();
    for (int i = 0; i < c->n_panels && s_tile_n < CFG_MAX_PANELS; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (p->sel[0] == '\0') continue;
        if (p->screen != 0) continue;      /* one screen for now */

        tile_spec_t *sp = &s_specs[s_tile_n];
        memset(sp, 0, sizeof(*sp));
        sp->title = p->title[0] ? p->title : p->sel;
        sp->kind  = p->kind;
        sp->col   = p->col;  sp->row = p->row;
        sp->w     = p->w ? p->w : 1;
        sp->h     = p->h ? p->h : 1;
        sp->vmin  = p->vmin; sp->vmax = p->vmax;
        sp->warn  = p->warn; sp->crit = p->crit;
        sp->lower_is_worse = p->lower_is_worse;
        s_tiles[s_tile_n] = tile_create(scr, sp);
        s_tile_n++;
    }

    /* An empty dashboard has to say why, or it reads as a fault. */
    hidden_if_changed(s_empty, s_tile_n > 0);
}

static void browser_closed(void);

static void browse_cb(lv_event_t *e)
{
    (void)e;
    ui_browser_open(browser_closed);
}

static void build_dashboard(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    const config_t *c = config_get();

    s_hdr_title = make_label(scr, FONT_L, COL_TEXT);
    lv_label_set_text(s_hdr_title, (c->n_endpoints && c->endpoints[0].name[0])
                                   ? c->endpoints[0].name : "Prometheus Panel");
    lv_obj_set_pos(s_hdr_title, GRID_MX, 8);

    s_hdr_status = make_label(scr, FONT_S, COL_DIM);
    lv_obj_set_pos(s_hdr_status, 300, 14);

    lv_obj_t *wifi = make_btn(scr, LV_SYMBOL_WIFI, reopen_setup_cb, NULL);
    lv_obj_set_size(wifi, 52, 30);
    lv_obj_set_pos(wifi, SCR_W - 168 - GRID_MX, 4);

    lv_obj_t *list = make_btn(scr, LV_SYMBOL_LIST, browse_cb, NULL);
    lv_obj_set_size(list, 52, 30);
    lv_obj_set_pos(list, SCR_W - 110 - GRID_MX, 4);

    lv_obj_t *gear = make_btn(scr, LV_SYMBOL_SETTINGS, endpoints_cb, NULL);
    lv_obj_set_size(gear, 52, 30);
    lv_obj_set_pos(gear, SCR_W - 52 - GRID_MX, 4);

    lv_obj_t *div = make_divider(scr, SCR_W);
    lv_obj_set_pos(div, 0, HEADER_H - 1);

    s_empty = make_label(scr, FONT_L, COL_DIM);
    lv_label_set_text(s_empty,
                      "No metrics selected\n\n"
                      LV_SYMBOL_LIST "  Browse metrics to choose what to show");
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);

    build_tiles(scr);

    lv_obj_t *fdiv = make_divider(scr, SCR_W);
    lv_obj_set_pos(fdiv, 0, FOOTER_Y);

    s_ftr_left = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_left, GRID_MX, FOOTER_Y + 7);

    s_ftr_right = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_right, SCR_W - 330, FOOTER_Y + 7);
}

static void rebuild_dashboard(void)
{
    const config_t *c = config_get();
    label_set_if_changed(s_hdr_title,
                         (c->n_endpoints && c->endpoints[0].name[0])
                         ? c->endpoints[0].name : "Prometheus Panel");
}

static void browser_closed(void)
{
    /* Selections changed: rebuild both sides of the mapping, in this order,
     * so the poller's slot i still corresponds to tile i. */
    poller_reload();
    build_tiles(lv_scr_act());
    s_seen_gen = UINT32_MAX;     /* force a repaint from the next snapshot */
}

/*
 * Runs in the LVGL task. Compares the poller's generation with != rather than
 * > so a uint32 wrap is a non-event, and still repaints the age readout when
 * no new data arrived, because "updated 40s ago" has to keep counting.
 */
static void dashboard_tick(lv_timer_t *timer)
{
    (void)timer;

    /*
     * Liveness beat from the LVGL task itself. The poller logs from core 0,
     * so a wedged UI task looks identical over serial to a healthy one --
     * data flowing, screen frozen. This distinguishes the two.
     */
    static int beat;
    if (beat++ % 20 == 0) {
        ESP_LOGI(TAG, "ui alive: tiles=%d seen_gen=%u stack_hw=%u",
                 s_tile_n, (unsigned)s_seen_gen,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    /*
     * Static, not a stack local, and taken ONCE.
     *
     * poller_snap_t carries every watch slot -- ~2.5KB at 24 panels -- and
     * this used to declare two of them on a 6KB LVGL task stack, which
     * overflowed the moment the watch limit was raised from 8. It only ever
     * runs on the LVGL task, so a static is safe and free.
     */
    static poller_snap_t snap;
    poller_snapshot(&snap);

    if (snap.generation != s_seen_gen) {
        s_seen_gen = snap.generation;
        for (int i = 0; i < snap.n && i < s_tile_n; i++) {
            const poller_metric_t *m = &snap.m[i];
            tile_data_t d = {
                .valid        = m->valid,
                .warming      = m->warming,
                .restarted    = m->restarted,
                .value        = m->value,
                .num          = m->num,
                .suffix       = m->suffix,
                .numeric_only = m->numeric_only,
                .has_hist     = m->has_hist,
                .n_buckets    = m->n_buckets,
                .bucket_le    = m->bucket_le,
                .bucket_share = m->bucket_share,
                .p50          = m->p50,
                .p90          = m->p90,
                .p99          = m->p99,
                .fmt          = (fmt_mode_t)m->fmt,
                .unit         = m->unit,
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

    /* The age readout has to keep counting even when no new data arrived, so
     * it is refreshed outside the generation check -- but from the same
     * snapshot. */
    if (snap.last_ok_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - snap.last_ok_ms) / 1000);
        label_set_fmt_if_changed(s_ftr_left, "updated %ds ago   %s",
                                 age, snap.status);
    } else {
        label_set_fmt_if_changed(s_ftr_left, "%s", snap.status);
    }
    text_color_if_changed(s_ftr_left, snap.ok ? COL_DIM : COL_WARN);
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
