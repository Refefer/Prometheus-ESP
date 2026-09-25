#include "ui_endpoints.h"

#include "config.h"
#include "http_util.h"
#include "lvgl_port.h"
#include "poller.h"
#include "secrets.h"
#include "webcfg.h"
#include "wifi_mgr.h"
#include "prom_text.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_setup.h"
#include "ui_widgets.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "endpoints";

static lv_obj_t *s_root;
static lv_obj_t *s_body;          /* the active tab's widgets live here */
static lv_obj_t *s_tab_ep, *s_tab_dev;
static void (*s_on_close)(void);

/* Endpoints tab. */
static lv_obj_t *s_ep_dd, *s_new_btn, *s_del_btn;
static lv_obj_t *s_url_btn, *s_url_lbl, *s_name_btn, *s_poll_lbl, *s_result;

/* Device tab. */
static lv_obj_t *s_theme_dd, *s_rot_sw, *s_dwell_dd, *s_net_lbl;
static lv_obj_t *s_tok_lbl, *s_push_lbl;

/*
 * The endpoint list as edited, committed whole on Save and dropped on Cancel.
 * An entry with id 0 is new and gets its id when saved; ids in s_removed are
 * deleted then. Editing a scratch copy rather than the live config is what
 * lets Cancel mean cancel across several endpoints at once.
 */
typedef struct {
    uint16_t id;
    char     url[CFG_URL_MAX];
    char     name[CFG_NAME_MAX];
    int      poll_s;
} ep_edit_t;

static ep_edit_t s_edit[CFG_MAX_ENDPOINTS];
static int       s_edit_n;
static int       s_sel;
static uint16_t  s_removed[CFG_MAX_ENDPOINTS];
static int       s_removed_n;
static bool      s_dev_tab;

static volatile bool s_testing;
static char          s_test_url[CFG_URL_MAX];

static const int k_dwell_s[] = { 10, 15, 20, 30, 60, 120, 300 };
#define N_DWELL ((int)(sizeof(k_dwell_s) / sizeof(k_dwell_s[0])))

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
    if (s_testing || s_edit_n == 0) return;
    const char *url = s_edit[s_sel].url;
    if (url[0] == '\0') {
        label_set_if_changed(s_result, "enter a URL first");
        text_color_if_changed(s_result, COL_WARN);
        return;
    }
    s_testing = true;
    label_set_if_changed(s_result, "testing...");
    text_color_if_changed(s_result, COL_DIM);
    /* A copy the task owns: the entry can be edited or deleted while the
     * probe is in flight. 8KB: TLS handshake plus the streaming parser. */
    strncpy(s_test_url, url, sizeof(s_test_url) - 1);
    s_test_url[sizeof(s_test_url) - 1] = '\0';
    if (xTaskCreate(test_task, "ep_test", 8192, s_test_url, 4, NULL) != pdPASS) {
        s_testing = false;
        label_set_if_changed(s_result, "out of memory");
    }
}

/* --------------------------------------------------------- config push */

static void show_push(void)
{
    if (s_push_lbl == NULL || s_tok_lbl == NULL) return;
    char ip[16] = "";
    wifi_mgr_info(ip, sizeof(ip), NULL);
    label_set_fmt_if_changed(s_push_lbl, "POST  http://%s/config",
                             ip[0] ? ip : "(offline)");

    char tok[SECRETS_TOKEN_MAX] = "";
    secrets_get_token(tok, sizeof(tok));
    /* Shown in full rather than masked: this is the device's own screen, and
     * anyone reading it is already standing in front of the panel. Masking it
     * would only mean copying it somewhere less safe. */
    label_set_if_changed(s_tok_lbl, tok[0] ? tok : "(disabled)");
}

static void token_done(const char *text, void *user)
{
    (void)user;
    if (text == NULL) return;
    secrets_set_token(text);
    show_push();
}

static void token_cb(lv_event_t *e)
{
    (void)e;
    char tok[SECRETS_TOKEN_MAX] = "";
    secrets_get_token(tok, sizeof(tok));
    ui_kbd_req_t req = {
        .title = "Config push token",
        .label = "Sent as the X-Auth header",
        .value = tok,
        .hint  = "Empty disables the push endpoint entirely.",
        .kind  = KB_URL,   /* hex and punctuation without a mode switch */
        .done  = token_done,
    };
    ui_kbd_edit(&req);
}

static void regen_cb(lv_event_t *e)
{
    (void)e;
    char tok[SECRETS_TOKEN_MAX] = "";
    secrets_new_token(tok, sizeof(tok));
    show_push();
    ui_toast("New token - update anything that pushes config", SEV_WARN, 3000);
}


/* ------------------------------------------------------------- editing */

static ep_edit_t *cur(void) { return s_edit_n ? &s_edit[s_sel] : NULL; }

/* What the selector calls an entry: its name, or the URL until it has one. */
static const char *entry_label(const ep_edit_t *e)
{
    if (e->name[0]) return e->name;
    if (e->url[0])  return e->url;
    return "(new endpoint)";
}

static void refresh_selector(void)
{
    if (s_ep_dd == NULL) return;
    char opts[CFG_MAX_ENDPOINTS * (CFG_NAME_MAX + 4) + 32];
    size_t w = 0;
    opts[0] = '\0';
    for (int i = 0; i < s_edit_n && w < sizeof(opts) - 1; i++) {
        /* Dropdown options are newline-separated, so a name must not carry
         * one; URLs are cut to fit rather than overflowing the buffer. */
        char one[CFG_NAME_MAX + 1];
        strncpy(one, entry_label(&s_edit[i]), sizeof(one) - 1);
        one[sizeof(one) - 1] = '\0';
        for (char *c = one; *c; c++) if (*c == '\n') *c = ' ';
        w += (size_t)snprintf(opts + w, sizeof(opts) - w, "%s%s", i ? "\n" : "", one);
    }
    lv_dropdown_set_options(s_ep_dd, s_edit_n ? opts : "(none yet)");
    lv_dropdown_set_selected(s_ep_dd, (uint16_t)s_sel);

    if (s_edit_n >= CFG_MAX_ENDPOINTS) lv_obj_add_state(s_new_btn, LV_STATE_DISABLED);
    else                               lv_obj_clear_state(s_new_btn, LV_STATE_DISABLED);
    if (s_edit_n == 0) lv_obj_add_state(s_del_btn, LV_STATE_DISABLED);
    else               lv_obj_clear_state(s_del_btn, LV_STATE_DISABLED);
}

/* Show the selected entry in the fields below the selector. */
static void refresh_fields(void)
{
    if (s_url_lbl == NULL) return;
    const ep_edit_t *e = cur();
    label_set_if_changed(s_url_lbl, e && e->url[0] ? e->url : "tap to enter a URL");
    lv_obj_t *nl = lv_obj_get_child(s_name_btn, 0);
    if (nl) label_set_if_changed(nl, e && e->name[0] ? e->name : "tap to name it");
    label_set_fmt_if_changed(s_poll_lbl, "every %ds", e ? e->poll_s : 10);
    label_set_if_changed(s_result, "not tested");
    text_color_if_changed(s_result, COL_DIM);

    /* Nothing to edit until an endpoint exists. */
    lv_obj_t *need[] = { s_url_btn, s_name_btn,
                         s_poll_lbl ? lv_obj_get_parent(s_poll_lbl) : NULL };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (need[i] == NULL) continue;
        if (e) lv_obj_clear_state(need[i], LV_STATE_DISABLED);
        else   lv_obj_add_state(need[i], LV_STATE_DISABLED);
    }
}

static void select_cb(lv_event_t *e)
{
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= s_edit_n) return;
    s_sel = sel;
    refresh_fields();
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    if (s_edit_n >= CFG_MAX_ENDPOINTS) return;
    ep_edit_t *n = &s_edit[s_edit_n];
    memset(n, 0, sizeof(*n));
    n->poll_s = config_get()->device.poll_default_s ? config_get()->device.poll_default_s : 10;
    s_sel = s_edit_n++;
    refresh_selector();
    refresh_fields();
}

static void delete_cb(lv_event_t *e)
{
    (void)e;
    ep_edit_t *d = cur();
    if (d == NULL) return;

    /*
     * Refused while a screen reads it, rather than taking the screens with it
     * or leaving them pointing at nothing: every screen must name a real
     * endpoint, and quietly repointing them would show one exporter's tiles
     * against another's metrics.
     */
    if (d->id != 0) {
        uint8_t scr[CFG_MAX_SCREENS];
        int n = config_endpoint_screens(d->id, scr, CFG_MAX_SCREENS);
        if (n > 0) {
            char list[40] = "";
            size_t w = 0;
            for (int i = 0; i < n && i < CFG_MAX_SCREENS && w < sizeof(list) - 6; i++) {
                w += (size_t)snprintf(list + w, sizeof(list) - w, "%s%u",
                                      i ? ", " : "", (unsigned)scr[i] + 1);
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "%s %s shows '%s' -- switch %s to another "
                     "endpoint first", n > 1 ? "Screens" : "Screen", list,
                     entry_label(d), n > 1 ? "them" : "it");
            ui_toast(msg, SEV_WARN, 4000);
            return;
        }
        s_removed[s_removed_n++] = d->id;
    }

    for (int i = s_sel; i + 1 < s_edit_n; i++) s_edit[i] = s_edit[i + 1];
    s_edit_n--;
    if (s_sel >= s_edit_n) s_sel = s_edit_n ? s_edit_n - 1 : 0;
    refresh_selector();
    refresh_fields();
}

static void url_done(const char *text, void *user)
{
    (void)user;
    ep_edit_t *e = cur();
    if (text == NULL || e == NULL) return;
    strncpy(e->url, text, sizeof(e->url) - 1);
    e->url[sizeof(e->url) - 1] = '\0';
    refresh_selector();       /* an unnamed entry is listed by its URL */
    refresh_fields();
}

static void url_cb(lv_event_t *e)
{
    (void)e;
    ep_edit_t *c = cur();
    if (c == NULL) return;
    ui_kbd_req_t req = {
        .title = "Endpoint URL",
        .label = "URL",
        .value = c->url,
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
    ep_edit_t *e = cur();
    if (text == NULL || e == NULL) return;
    strncpy(e->name, text, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    refresh_selector();
    lv_obj_t *l = s_name_btn ? lv_obj_get_child(s_name_btn, 0) : NULL;
    if (l) label_set_if_changed(l, e->name[0] ? e->name : "tap to name it");
}

static void name_cb(lv_event_t *e)
{
    (void)e;
    ep_edit_t *c = cur();
    if (c == NULL) return;
    ui_kbd_req_t req = {
        .title = "Endpoint name",
        .label = "Shown in the header of every screen that reads it",
        .value = c->name,
        .kind  = KB_TEXT,
        .done  = name_done,
    };
    ui_kbd_edit(&req);
}

static void poll_cb(lv_event_t *e)
{
    (void)e;
    ep_edit_t *c = cur();
    if (c == NULL) return;
    /* Tap to cycle: a slider or a numeric field for five discrete values is
     * more work to hit accurately than a button that steps through them. */
    static const int steps[] = { 2, 5, 10, 15, 30, 60 };
    int n = (int)(sizeof(steps) / sizeof(steps[0]));
    int i = 0;
    while (i < n && steps[i] != c->poll_s) i++;
    c->poll_s = steps[(i + 1) % n];
    label_set_fmt_if_changed(s_poll_lbl, "every %ds", c->poll_s);
}

/* ------------------------------------------------------------- device */

/* Recorded now, applied when the sheet closes: applying a palette rebuilds
 * the whole tree, and this sheet is part of the tree. */
static void theme_cb(lv_event_t *e)
{
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= THEME_COUNT) return;
    config_t *c = config_get();
    strncpy(c->device.theme, app_theme_slug((theme_id_t)sel),
            sizeof(c->device.theme) - 1);
    config_touch();
}

/*
 * Auto-rotate takes effect as it is changed, like the theme: it is a device
 * setting, not part of the endpoint edit that Cancel throws away.
 */
static void rotate_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    config_get()->device.rotate_enabled = on;
    if (s_dwell_dd) {
        if (on) lv_obj_clear_state(s_dwell_dd, LV_STATE_DISABLED);
        else    lv_obj_add_state(s_dwell_dd, LV_STATE_DISABLED);
    }
    config_touch();
}

static void dwell_cb(lv_event_t *e)
{
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    /* Past the list is the custom value a push set; choosing it keeps it. */
    if (sel >= N_DWELL) return;
    config_get()->device.rotate_dwell_s = (uint16_t)k_dwell_s[sel];
    config_touch();
}

/*
 * Hands the screen to the WiFi wizard.
 *
 * Closes first rather than stacking a second full-screen sheet on the first:
 * two overlapping modals means two things that both think they own the
 * screen, and whichever closes last wins.
 */
static void close_overlay(void);
static void wifi_cb(lv_event_t *e)
{
    (void)e;
    void (*after)(void) = s_on_close;
    close_overlay();
    ui_setup_open(after);
}

/* ------------------------------------------------------------- tabs */

static void clear_tab_widgets(void)
{
    s_ep_dd = s_new_btn = s_del_btn = NULL;
    s_url_btn = s_url_lbl = s_name_btn = s_poll_lbl = s_result = NULL;
    s_theme_dd = s_rot_sw = s_dwell_dd = s_net_lbl = NULL;
    s_tok_lbl = s_push_lbl = NULL;
}

static lv_obj_t *caption(const char *text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = make_label(s_body, FONT_S, COL_DIM);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void build_endpoints_tab(void)
{
    const lv_coord_t x0 = GRID_MX + 8, full = SCR_W - 2 * (GRID_MX + 8);

    caption("Endpoint", x0, 0);
    s_ep_dd = make_dropdown(s_body, "", select_cb, NULL);
    lv_obj_set_size(s_ep_dd, 320, FIELD_H);
    lv_obj_set_pos(s_ep_dd, x0, 22);

    s_new_btn = make_btn(s_body, LV_SYMBOL_PLUS "  New", new_cb, NULL);
    lv_obj_set_size(s_new_btn, 140, FIELD_H);
    lv_obj_set_pos(s_new_btn, x0 + 340, 22);

    s_del_btn = make_btn(s_body, LV_SYMBOL_TRASH "  Delete", delete_cb, NULL);
    lv_obj_set_size(s_del_btn, 140, FIELD_H);
    lv_obj_set_pos(s_del_btn, x0 + 500, 22);

    caption("URL", x0, 84);
    s_url_btn = make_btn(s_body, "", url_cb, NULL);
    lv_obj_set_size(s_url_btn, full, FIELD_H);
    lv_obj_set_pos(s_url_btn, x0, 106);
    s_url_lbl = lv_obj_get_child(s_url_btn, 0);
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_url_lbl, full - 24);

    caption("Name", x0, 168);
    s_name_btn = make_btn(s_body, "", name_cb, NULL);
    lv_obj_set_size(s_name_btn, 320, FIELD_H);
    lv_obj_set_pos(s_name_btn, x0, 190);

    caption("Poll interval", x0 + 340, 168);
    lv_obj_t *pbtn = make_btn(s_body, "", poll_cb, NULL);
    lv_obj_set_size(pbtn, 180, FIELD_H);
    lv_obj_set_pos(pbtn, x0 + 340, 190);
    s_poll_lbl = lv_obj_get_child(pbtn, 0);

    lv_obj_t *tbtn = make_btn(s_body, LV_SYMBOL_REFRESH "  Test", test_cb, NULL);
    lv_obj_set_size(tbtn, 160, BTN_H);
    lv_obj_set_pos(tbtn, x0, 256);

    /* Elided rather than wrapped: two lines would run into the buttons. */
    s_result = make_label(s_body, FONT_M, COL_DIM);
    lv_obj_set_width(s_result, full - 180);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_result, x0 + 180, 268);

    refresh_selector();
    refresh_fields();
}

static void build_device_tab(void)
{
    const lv_coord_t x0 = GRID_MX + 8;
    const config_t *c = config_get();

    /* Theme. The palettes have existed since the first UI commit and nothing
     * ever offered them, so the device has only ever been one colour. */
    caption("Theme", x0, 0);
    s_theme_dd = make_dropdown(s_body, app_theme_options(), theme_cb, NULL);
    lv_obj_set_size(s_theme_dd, 240, FIELD_H);
    lv_obj_set_pos(s_theme_dd, x0, 22);
    lv_dropdown_set_selected(s_theme_dd, (uint16_t)app_theme_id());

    caption("Auto-rotate screens", x0 + 290, 0);
    s_rot_sw = make_switch(s_body, c->device.rotate_enabled, rotate_cb, NULL);
    lv_obj_set_pos(s_rot_sw, x0 + 290, 32);

    caption("Every", x0 + 380, 0);
    char opts[96];
    size_t w = 0;
    int sel = -1;
    for (int i = 0; i < N_DWELL; i++) {
        int v = k_dwell_s[i];
        w += (size_t)snprintf(opts + w, sizeof(opts) - w, "%s%d %s",
                              i ? "\n" : "", v >= 60 ? v / 60 : v,
                              v >= 60 ? "min" : "s");
        if (v == c->device.rotate_dwell_s) sel = i;
    }
    /* A dwell pushed over HTTP that the list does not offer is shown as it is,
     * not snapped to a neighbour the user never chose. */
    if (sel < 0) {
        snprintf(opts + w, sizeof(opts) - w, "\n%u s",
                 (unsigned)c->device.rotate_dwell_s);
        sel = N_DWELL;
    }
    s_dwell_dd = make_dropdown(s_body, opts, dwell_cb, NULL);
    lv_obj_set_size(s_dwell_dd, 160, FIELD_H);
    lv_obj_set_pos(s_dwell_dd, x0 + 380, 22);
    lv_dropdown_set_selected(s_dwell_dd, (uint16_t)sel);
    if (!c->device.rotate_enabled) lv_obj_add_state(s_dwell_dd, LV_STATE_DISABLED);

    lv_obj_t *hint = make_label(s_body, FONT_S, COL_DIM);
    lv_label_set_text(hint, "Pages through screens with tiles.\n"
                            "A touch pauses it for one dwell.");
    lv_obj_set_pos(hint, x0 + 560, 20);

    /* Wi-Fi, reachable from here as well as from the header: this is the
     * sheet people look in when something is not connecting. */
    lv_obj_t *wbtn = make_btn(s_body, LV_SYMBOL_WIFI "  Wi-Fi setup", wifi_cb, NULL);
    lv_obj_set_size(wbtn, 210, BTN_H);
    lv_obj_set_pos(wbtn, x0, 96);

    /* The address, which used to sit in the header. It is a thing you need
     * once, while setting the device up -- which is here. */
    s_net_lbl = make_label(s_body, FONT_S, COL_DIM);
    lv_obj_set_pos(s_net_lbl, x0 + 230, 108);
    {
        char ip[16] = ""; int8_t rssi = 0;
        wifi_mgr_info(ip, sizeof(ip), &rssi);
        if (ip[0]) lv_label_set_text_fmt(s_net_lbl, "%s   %d dBm", ip, (int)rssi);
        else       lv_label_set_text(s_net_lbl, "not connected");
    }

    /* config push */
    lv_obj_t *pdiv = make_divider(s_body, SCR_W - 2 * (GRID_MX + 8));
    lv_obj_set_pos(pdiv, x0, 160);

    caption("Config push", x0, 172);
    s_push_lbl = make_label(s_body, FONT_M, COL_TEXT);
    lv_obj_set_pos(s_push_lbl, x0, 196);

    caption("X-Auth", x0 + 400, 172);
    lv_obj_t *tokbtn = make_btn(s_body, "", token_cb, NULL);
    lv_obj_set_size(tokbtn, 230, 40);
    lv_obj_set_pos(tokbtn, x0 + 400, 190);
    s_tok_lbl = lv_obj_get_child(tokbtn, 0);
    lv_obj_set_style_text_font(s_tok_lbl, FONT_M, 0);

    lv_obj_t *rb = make_btn(s_body, LV_SYMBOL_REFRESH, regen_cb, NULL);
    lv_obj_set_size(rb, 56, 40);
    lv_obj_set_pos(rb, x0 + 640, 190);

    show_push();
}

static void show_tab(bool device)
{
    s_dev_tab = device;
    lv_obj_clean(s_body);
    clear_tab_widgets();
    if (device) build_device_tab();
    else        build_endpoints_tab();

    bg_color_if_changed(s_tab_ep,  device ? COL_PANEL_ALT : COL_ACCENT);
    bg_color_if_changed(s_tab_dev, device ? COL_ACCENT : COL_PANEL_ALT);
    ui_check_overlaps(s_body, device ? "settings/device" : "settings/endpoints");
}

static void tab_cb(lv_event_t *e)
{
    bool device = lv_event_get_user_data(e) != NULL;
    if (device != s_dev_tab) show_tab(device);
}

/* ------------------------------------------------------------- open/close */

static void close_overlay(void)
{
    if (s_root == NULL) return;
    lv_obj_del(s_root);
    s_root = s_body = s_tab_ep = s_tab_dev = NULL;
    clear_tab_widgets();
    if (s_on_close) s_on_close();
}

static void cancel_cb(lv_event_t *e) { (void)e; close_overlay(); }

static void save_cb(lv_event_t *e)
{
    (void)e;
    /* A new entry left completely blank is not an endpoint anyone asked for
     * -- it is the empty form a fresh device opens on, and refusing to save
     * a theme change because of it would be absurd. */
    for (int i = 0; i < s_edit_n; ) {
        const ep_edit_t *d = &s_edit[i];
        if (d->id == 0 && d->url[0] == '\0' && d->name[0] == '\0') {
            for (int j = i; j + 1 < s_edit_n; j++) s_edit[j] = s_edit[j + 1];
            s_edit_n--;
        } else {
            i++;
        }
    }
    if (s_sel >= s_edit_n) s_sel = s_edit_n ? s_edit_n - 1 : 0;

    for (int i = 0; i < s_edit_n; i++) {
        if (s_edit[i].url[0] != '\0') continue;
        /* Point at the one that needs fixing rather than just refusing. */
        if (s_dev_tab) show_tab(false);
        s_sel = i;
        refresh_selector();
        refresh_fields();
        label_set_if_changed(s_result, "enter a URL first");
        text_color_if_changed(s_result, COL_WARN);
        return;
    }

    for (int i = 0; i < s_removed_n; i++) {
        if (!config_endpoint_remove(s_removed[i])) {
            /* A push bound a screen to it since the sheet opened. */
            ui_toast("An endpoint in use by a screen was kept", SEV_WARN, 3000);
        }
    }

    for (int i = 0; i < s_edit_n; i++) {
        const ep_edit_t *d = &s_edit[i];
        cfg_endpoint_t *ep = d->id ? config_endpoint_by_id(d->id) : NULL;
        if (ep == NULL) ep = config_endpoint_add();
        if (ep == NULL) break;
        strncpy(ep->url, d->url, sizeof(ep->url) - 1);
        ep->url[sizeof(ep->url) - 1] = '\0';
        strncpy(ep->name, d->name[0] ? d->name : "endpoint", sizeof(ep->name) - 1);
        ep->name[sizeof(ep->name) - 1] = '\0';
        ep->poll_s  = (uint16_t)d->poll_s;
        ep->enabled = true;
    }

    /* The first endpoint ever made: screens that existed before it had
     * nothing to point at, and now they have. */
    config_bind_unbound_screens(config_default_ep());

    config_touch();
    config_flush();          /* leaving a settings screen flushes immediately;
                              * the write itself happens on a worker. The
                              * poller notices the new generation itself. */
    close_overlay();
}

void ui_endpoints_open(void (*on_close)(void))
{
    if (s_root) return;
    s_on_close = on_close;

    const config_t *c = config_get();
    s_edit_n = 0;
    s_removed_n = 0;
    for (int i = 0; i < c->n_endpoints && i < CFG_MAX_ENDPOINTS; i++) {
        ep_edit_t *d = &s_edit[s_edit_n++];
        memset(d, 0, sizeof(*d));
        d->id = c->endpoints[i].id;
        strncpy(d->url, c->endpoints[i].url, sizeof(d->url) - 1);
        strncpy(d->name, c->endpoints[i].name, sizeof(d->name) - 1);
        d->poll_s = c->endpoints[i].poll_s ? c->endpoints[i].poll_s
                                           : c->device.poll_default_s;
    }
    s_sel = 0;
    /* A fresh device opens straight onto a blank endpoint to fill in, which
     * is the only thing it can usefully do. */
    if (s_edit_n == 0) {
        memset(&s_edit[0], 0, sizeof(s_edit[0]));
        s_edit[0].poll_s = c->device.poll_default_s ? c->device.poll_default_s : 10;
        s_edit_n = 1;
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
    lv_label_set_text(title, "Settings");
    lv_obj_set_pos(title, GRID_MX + 8, 16);

    /* Two plain buttons rather than an lv_tabview: a tabview's content pane
     * scrolls sideways, and a scrollable ancestor swallows gestures. */
    s_tab_ep = make_btn(s_root, "Endpoints", tab_cb, NULL);
    lv_obj_set_size(s_tab_ep, 150, 40);
    lv_obj_set_pos(s_tab_ep, GRID_MX + 200, 14);
    s_tab_dev = make_btn(s_root, "Device", tab_cb, (void *)1);
    lv_obj_set_size(s_tab_dev, 150, 40);
    lv_obj_set_pos(s_tab_dev, GRID_MX + 360, 14);

    s_body = lv_obj_create(s_root);
    lv_obj_set_size(s_body, SCR_W, SCR_H - 72 - BTN_H - 32);
    lv_obj_set_pos(s_body, 0, 72);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = make_btn(s_root, "Cancel", cancel_cb, NULL);
    lv_obj_set_size(cancel, 160, BTN_H);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, GRID_MX + 8, -24);

    lv_obj_t *save = make_btn_accent(s_root, LV_SYMBOL_OK "  Save", save_cb, NULL);
    lv_obj_set_size(save, 200, BTN_H);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -(GRID_MX + 8), -24);

    s_dev_tab = true;          /* so the first show_tab builds the body */
    show_tab(false);
}

bool ui_endpoints_is_open(void) { return s_root != NULL; }
