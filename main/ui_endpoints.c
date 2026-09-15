#include "ui_endpoints.h"

#include "config.h"
#include "http_util.h"
#include "lvgl_port.h"
#include "poller.h"
#include "prom_text.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "endpoints";

static lv_obj_t *s_root;
static lv_obj_t *s_url_btn, *s_url_lbl, *s_name_btn, *s_poll_lbl, *s_result;
static void (*s_on_close)(void);

static char s_url[CFG_URL_MAX];
static char s_name[CFG_NAME_MAX];
static int  s_poll_s = 10;

static volatile bool s_testing;

/*
 * One-tap insertions. A URL typed character by character on a touchscreen is
 * the single most tedious thing in a no-laptop design; these turn a typical
 * exporter URL into about eight taps.
 */
static const char *const k_url_chips[] = {
    "http://", "192.168.", ".local", ":9090", ":9100", ":12345",
    "/metrics", "/api/v1/query", NULL
};

/* ------------------------------------------------------------- testing */

typedef struct { uint32_t samples; } probe_ctx_t;

static bool probe_sample(void *ctx, const prom_sample_t *s)
{
    (void)s;
    ((probe_ctx_t *)ctx)->samples++;
    return true;
}

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)   { heap_caps_free(p); }

static bool probe_chunk(void *ctx, const char *d, size_t n)
{
    return prom_text_feed((prom_text_parser_t *)ctx, d, n);
}

/*
 * The Test button is worth as much as any input trick: it turns typing a URL
 * blind into a feedback loop, and it distinguishes "wrong host" from "wrong
 * path" from "that is a web page, not an exporter" before anything is saved.
 */
static void test_task(void *arg)
{
    char url[CFG_URL_MAX];
    strncpy(url, (const char *)arg, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    probe_ctx_t pc = {0};
    const prom_text_sink_t sink = { NULL, NULL, probe_sample };
    prom_text_parser_t *p = prom_text_new(&sink, &pc, psram_alloc, psram_free);

    http_result_t res = {0};
    prom_text_stats_t st = {0};
    if (p != NULL) {
        /* Slot -1: a one-shot connection, so testing never disturbs the
         * cached socket the live poller is using. */
        http_get_stream(-1, url, NULL, probe_chunk, p, 8000, &res);
        prom_text_finish(p, &st);
        prom_text_free(p);
    }

    char msg[112];
    severity_t sev;
    if (p == NULL) {
        snprintf(msg, sizeof(msg), "out of memory");
        sev = SEV_CRIT;
    } else if (res.klass != HTTP_ERR_NONE) {
        snprintf(msg, sizeof(msg), "%s%s%d%s", http_err_text(res.klass),
                 res.status ? "  (HTTP " : "", res.status,
                 res.status ? ")" : "");
        sev = SEV_CRIT;
    } else if (st.samples == 0) {
        snprintf(msg, sizeof(msg),
                 "200 OK but no metrics -- is this a /metrics endpoint?");
        sev = SEV_WARN;
    } else {
        snprintf(msg, sizeof(msg), "OK  %u series  %u KB  %u ms",
                 (unsigned)st.samples, (unsigned)(res.bytes / 1024),
                 (unsigned)res.duration_ms);
        sev = SEV_OK;
    }

    if (lvgl_port_lock(-1)) {
        /* The overlay can be closed while the probe is in flight. */
        if (s_result != NULL) {
            label_set_if_changed(s_result, msg);
            text_color_if_changed(s_result, sev == SEV_OK ? COL_OK
                                                          : app_theme_sev(sev));
        }
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "test %s -> %s", url, msg);
    s_testing = false;
    vTaskDelete(NULL);
}

static void test_cb(lv_event_t *e)
{
    (void)e;
    if (s_testing) return;
    if (s_url[0] == '\0') {
        label_set_if_changed(s_result, "enter a URL first");
        text_color_if_changed(s_result, COL_WARN);
        return;
    }
    s_testing = true;
    label_set_if_changed(s_result, "testing...");
    text_color_if_changed(s_result, COL_DIM);
    /* 8KB: TLS handshake plus the streaming parser. */
    xTaskCreate(test_task, "ep_test", 8192, s_url, 4, NULL);
}

/* ------------------------------------------------------------- editing */

static void url_done(const char *text, void *user)
{
    (void)user;
    if (text == NULL) return;
    strncpy(s_url, text, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    label_set_if_changed(s_url_lbl, s_url[0] ? s_url : "tap to enter a URL");
    label_set_if_changed(s_result, "not tested");
    text_color_if_changed(s_result, COL_DIM);
}

static void url_cb(lv_event_t *e)
{
    (void)e;
    ui_kbd_req_t req = {
        .title = "Endpoint URL",
        .label = "URL",
        .value = s_url,
        .hint  = "The chips above insert common pieces.",
        .kind  = KB_URL,
        .chips = k_url_chips,
        .done  = url_done,
    };
    ui_kbd_edit(&req);
}

static void name_done(const char *text, void *user)
{
    (void)user;
    if (text == NULL) return;
    strncpy(s_name, text, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    lv_obj_t *l = lv_obj_get_child(s_name_btn, 0);
    if (l) label_set_if_changed(l, s_name[0] ? s_name : "tap to name it");
}

static void name_cb(lv_event_t *e)
{
    (void)e;
    ui_kbd_req_t req = {
        .title = "Endpoint name",
        .label = "Shown in the header",
        .value = s_name,
        .kind  = KB_TEXT,
        .done  = name_done,
    };
    ui_kbd_edit(&req);
}

static void poll_cb(lv_event_t *e)
{
    (void)e;
    /* Tap to cycle: a slider or a numeric field for five discrete values is
     * more work to hit accurately than a button that steps through them. */
    static const int steps[] = { 2, 5, 10, 15, 30, 60 };
    int n = (int)(sizeof(steps) / sizeof(steps[0]));
    int i = 0;
    while (i < n && steps[i] != s_poll_s) i++;
    s_poll_s = steps[(i + 1) % n];
    label_set_fmt_if_changed(s_poll_lbl, "every %ds", s_poll_s);
}

static void close_overlay(void)
{
    if (s_root == NULL) return;
    lv_obj_del(s_root);
    s_root = NULL;
    s_url_btn = s_url_lbl = s_name_btn = s_poll_lbl = s_result = NULL;
    if (s_on_close) s_on_close();
}

static void cancel_cb(lv_event_t *e) { (void)e; close_overlay(); }

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (s_url[0] == '\0') {
        label_set_if_changed(s_result, "enter a URL first");
        text_color_if_changed(s_result, COL_WARN);
        return;
    }

    config_t *c = config_get();
    cfg_endpoint_t *ep = c->n_endpoints > 0 ? &c->endpoints[0]
                                            : config_endpoint_add();
    if (ep == NULL) return;

    strncpy(ep->url, s_url, sizeof(ep->url) - 1);
    strncpy(ep->name, s_name[0] ? s_name : "endpoint", sizeof(ep->name) - 1);
    ep->poll_s  = (uint16_t)s_poll_s;
    ep->enabled = true;

    config_touch();
    config_flush();          /* leaving a settings screen flushes immediately */
    poller_set_endpoint(ep->url, s_poll_s);
    close_overlay();
}

void ui_endpoints_open(void (*on_close)(void))
{
    if (s_root) return;
    s_on_close = on_close;

    config_t *c = config_get();
    if (c->n_endpoints > 0) {
        strncpy(s_url, c->endpoints[0].url, sizeof(s_url) - 1);
        strncpy(s_name, c->endpoints[0].name, sizeof(s_name) - 1);
        s_poll_s = c->endpoints[0].poll_s ? c->endpoints[0].poll_s
                                          : c->device.poll_default_s;
    }

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, SCR_W, SCR_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Swipes inside a modal must not page the dashboard behind it. */
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = make_label(s_root, FONT_XL, COL_TEXT);
    lv_label_set_text(title, "Endpoint");
    lv_obj_set_pos(title, GRID_MX + 8, 16);

    lv_obj_t *cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(cap, "URL");
    lv_obj_set_pos(cap, GRID_MX + 8, 72);

    s_url_btn = make_btn(s_root, "", url_cb, NULL);
    lv_obj_set_size(s_url_btn, SCR_W - 2 * (GRID_MX + 8), FIELD_H);
    lv_obj_set_pos(s_url_btn, GRID_MX + 8, 94);
    s_url_lbl = lv_obj_get_child(s_url_btn, 0);
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_url_lbl, SCR_W - 2 * (GRID_MX + 8) - 24);
    label_set_if_changed(s_url_lbl, s_url[0] ? s_url : "tap to enter a URL");

    lv_obj_t *ncap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(ncap, "Name");
    lv_obj_set_pos(ncap, GRID_MX + 8, 158);

    s_name_btn = make_btn(s_root, s_name[0] ? s_name : "tap to name it",
                          name_cb, NULL);
    lv_obj_set_size(s_name_btn, 300, FIELD_H);
    lv_obj_set_pos(s_name_btn, GRID_MX + 8, 180);

    lv_obj_t *pcap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(pcap, "Poll interval");
    lv_obj_set_pos(pcap, GRID_MX + 330, 158);

    lv_obj_t *pbtn = make_btn(s_root, "", poll_cb, NULL);
    lv_obj_set_size(pbtn, 180, FIELD_H);
    lv_obj_set_pos(pbtn, GRID_MX + 330, 180);
    s_poll_lbl = lv_obj_get_child(pbtn, 0);
    label_set_fmt_if_changed(s_poll_lbl, "every %ds", s_poll_s);

    lv_obj_t *tbtn = make_btn(s_root, LV_SYMBOL_REFRESH "  Test", test_cb, NULL);
    lv_obj_set_size(tbtn, 160, BTN_H);
    lv_obj_set_pos(tbtn, GRID_MX + 8, 250);

    s_result = make_label(s_root, FONT_M, COL_DIM);
    lv_obj_set_width(s_result, SCR_W - 2 * (GRID_MX + 8) - 180);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_result, GRID_MX + 180, 258);
    label_set_if_changed(s_result, "not tested");

    lv_obj_t *cancel = make_btn(s_root, "Cancel", cancel_cb, NULL);
    lv_obj_set_size(cancel, 160, BTN_H);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, GRID_MX + 8, -24);

    lv_obj_t *save = make_btn_accent(s_root, LV_SYMBOL_OK "  Save", save_cb, NULL);
    lv_obj_set_size(save, 200, BTN_H);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -(GRID_MX + 8), -24);
}

bool ui_endpoints_is_open(void) { return s_root != NULL; }
