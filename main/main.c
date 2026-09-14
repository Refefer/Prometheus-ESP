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
#include "lvgl.h"

#include "lvgl_port.h"
#include "prom_text.h"
#include "secrets.h"
#include "storage.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_setup.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "waveshare_rgb_lcd_port.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

static lv_obj_t *s_mem_label;
static lv_obj_t *s_parse_label;
static lv_obj_t *s_net_label;
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

/* Runs in the LVGL task via lv_timer, so no lock is needed. */
static void mem_tick(lv_timer_t *t)
{
    (void)t;
    size_t cfg_total = 0, cfg_used = 0;
    storage_usage(STORAGE_CFG_PATH, &cfg_total, &cfg_used);

    /*
     * Also log the numbers every few seconds. This board's console is the
     * chip's native USB, which re-enumerates on reset -- so anything printed
     * during boot is gone before a host can attach, and a heartbeat is the
     * only way to read the memory budget over serial at all.
     */
    static int tick;
    if (tick++ % 5 == 0) {
        ESP_LOGI(TAG, "SRAM %uK  PSRAM %uK (largest block %uK)  cfg %u/%uK  data %s",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(cfg_used / 1024), (unsigned)(cfg_total / 1024),
                 storage_data_ready() ? "ok" : "unavailable");
        ESP_LOGI(TAG, "%s", s_smoke_result);
    }

    if (s_net_label) {
        char ip[16] = "";
        int8_t rssi = 0;
        wifi_mgr_info(ip, sizeof(ip), &rssi);
        switch (wifi_mgr_state()) {
        case WIFI_ST_CONNECTED:
            label_set_fmt_if_changed(s_net_label, "%s   %d dBm", ip, (int)rssi);
            text_color_if_changed(s_net_label, COL_OK);
            break;
        case WIFI_ST_CONNECTING:
            label_set_if_changed(s_net_label, "connecting...");
            text_color_if_changed(s_net_label, COL_WARN);
            break;
        case WIFI_ST_FAILED:
            label_set_fmt_if_changed(s_net_label, "offline - %s",
                                     wifi_mgr_fail_reason());
            text_color_if_changed(s_net_label, COL_CRIT);
            break;
        default:
            label_set_if_changed(s_net_label, "no network configured");
            text_color_if_changed(s_net_label, COL_DIM);
            break;
        }
    }

    if (s_mem_label) {
        label_set_fmt_if_changed(
            s_mem_label,
            "SRAM free %u KB   PSRAM free %u KB   largest PSRAM block %u KB\n"
            "config %u/%u KB   data %s",
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(cfg_used / 1024), (unsigned)(cfg_total / 1024),
            storage_data_ready() ? "mounted" : "unavailable");
    }
}

static void reopen_setup_cb(lv_event_t *e)
{
    (void)e;
    ui_setup_open();
}

/* The interim status screen shown when WiFi is already configured. The
 * dashboard replaces this entirely once the tile framework lands. */
static void build_status_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    lv_obj_t *title = make_label(scr, FONT_XL, COL_TEXT);
    lv_label_set_text(title, "Prometheus Panel");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    s_net_label = make_label(scr, FONT_L, COL_ACCENT);
    lv_obj_align(s_net_label, LV_ALIGN_TOP_MID, 0, 88);

    s_parse_label = make_label(scr, FONT_M, COL_OK);
    lv_obj_align(s_parse_label, LV_ALIGN_CENTER, 0, -10);

    s_mem_label = make_label(scr, FONT_M, COL_DIM);
    lv_obj_set_style_text_align(s_mem_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mem_label, LV_ALIGN_BOTTOM_MID, 0, -56);

    lv_obj_t *b = make_btn(scr, LV_SYMBOL_SETTINGS "  Wi-Fi setup",
                           reopen_setup_cb, NULL);
    lv_obj_set_size(b, 240, BTN_H);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -8);

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
        ui_kbd_init();
        if (secrets_have_wifi()) {
            build_status_ui();
            lv_label_set_text(s_parse_label, s_smoke_result);
        } else {
            /* First boot: land straight in setup rather than showing a
             * dashboard that cannot possibly have data. */
            ui_setup_open();
        }
        /* Created regardless of which screen is up: the heartbeat is the only
         * way to see this device's state over serial, since the native-USB
         * console loses everything printed before a host attaches. */
        mem_tick(NULL);
        lv_timer_create(mem_tick, 1000, NULL);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "boot complete: SRAM %u KB free, PSRAM %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
