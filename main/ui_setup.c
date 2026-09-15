#include "ui_setup.h"

#include "lvgl_port.h"
#include "secrets.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_mgr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "setup";

#define MAX_SCAN_APS 24

static lv_obj_t *s_root;
static lv_obj_t *s_step_page;
static void (*s_on_close)(void);

/* WiFi step widgets */
static lv_obj_t *s_dd_networks;
static lv_obj_t *s_lbl_apinfo;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_btn_join;
static lv_obj_t *s_spinner;

static char s_ssid[SECRETS_SSID_MAX];
static char s_pass[SECRETS_PASS_MAX];
static bool s_pass_entered;

static volatile bool s_scan_busy;
static wifi_ap_record_t s_aps[MAX_SCAN_APS];
static uint16_t s_ap_n;

static lv_timer_t *s_join_timer;
static uint32_t    s_join_deadline;

static void build_welcome(void);
static void build_wifi(void);
static void build_done(void);

/* ------------------------------------------------------------------ helpers */

static void clear_page(void)
{
    if (s_step_page) lv_obj_del(s_step_page);
    s_step_page = lv_obj_create(s_root);
    lv_obj_set_size(s_step_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_step_page, 0, 0);
    lv_obj_set_style_bg_color(s_step_page, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_step_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_step_page, 0, 0);
    lv_obj_set_style_radius(s_step_page, 0, 0);
    lv_obj_set_style_pad_all(s_step_page, 0, 0);
    lv_obj_clear_flag(s_step_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Modals and the dashboard both rely on gestures stopping at their own
     * root rather than bubbling to the screen. */
    lv_obj_clear_flag(s_step_page, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_dd_networks = s_lbl_apinfo = s_lbl_status = NULL;
    s_btn_join = s_spinner = NULL;
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    ui_setup_close();
}

/*
 * Every step carries the exit. A wizard you can only leave by finishing it is
 * a trap when it was opened by a mis-tap, and this one is reachable from a
 * button on the dashboard.
 */
static void add_close_button(void)
{
    lv_obj_t *x = make_btn(s_step_page, LV_SYMBOL_CLOSE, close_cb, NULL);
    lv_obj_set_size(x, 56, 40);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -GRID_MX, 12);
}

static lv_obj_t *step_heading(const char *step, const char *title)
{
    lv_obj_t *s = make_label(s_step_page, FONT_S, COL_ACCENT);
    lv_label_set_text(s, step);
    lv_obj_set_pos(s, PAD_L + 20, 40);

    lv_obj_t *t = make_label(s_step_page, FONT_XL, COL_TEXT);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, PAD_L + 20, 62);
    return t;
}

/* ------------------------------------------------------------------ welcome */

static void welcome_next_cb(lv_event_t *e) { (void)e; build_wifi(); }

static void build_welcome(void)
{
    clear_page();
    add_close_button();

    lv_obj_t *t = make_label(s_step_page, FONT_XL, COL_TEXT);
    lv_label_set_text(t, "Prometheus Panel");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *sub = make_label(s_step_page, FONT_L, COL_ACCENT);
    lv_label_set_text(sub, "Let's get you connected.");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 140);

    lv_obj_t *body = make_label(s_step_page, FONT_M, COL_DIM);
    lv_label_set_text(body,
                      "This takes about a minute:\n\n"
                      "   1.  Join your Wi-Fi\n"
                      "   2.  Point at a metrics endpoint\n"
                      "   3.  Pick what to show");
    lv_obj_set_style_text_line_space(body, 6, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 200);

    lv_obj_t *next = make_btn_accent(s_step_page, "Start  " LV_SYMBOL_RIGHT,
                                     welcome_next_cb, NULL);
    lv_obj_set_size(next, 200, BTN_H);
    lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, -PAD_L - 20, -30);
}

/* --------------------------------------------------------------- wifi step */

static void set_status(const char *msg, severity_t sev)
{
    if (s_lbl_status == NULL) return;
    label_set_if_changed(s_lbl_status, msg ? msg : "");
    text_color_if_changed(s_lbl_status, sev == SEV_OK ? COL_DIM : app_theme_sev(sev));
}

/* Render the selected AP's signal and security, so a mistyped password is
 * distinguishable from a network that is simply out of range. */
static void show_ap_info(void)
{
    if (s_lbl_apinfo == NULL || s_dd_networks == NULL) return;
    uint16_t idx = lv_dropdown_get_selected(s_dd_networks);
    if (idx >= s_ap_n) { label_set_if_changed(s_lbl_apinfo, ""); return; }

    const wifi_ap_record_t *ap = &s_aps[idx];
    const char *sec = (ap->authmode == WIFI_AUTH_OPEN)        ? "open"
                    : (ap->authmode == WIFI_AUTH_WEP)         ? "WEP"
                    : (ap->authmode == WIFI_AUTH_WPA2_PSK)    ? "WPA2"
                    : (ap->authmode == WIFI_AUTH_WPA3_PSK)    ? "WPA3"
                    : (ap->authmode == WIFI_AUTH_WPA_WPA2_PSK)? "WPA/WPA2"
                    : (ap->authmode == WIFI_AUTH_WPA2_WPA3_PSK)? "WPA2/WPA3"
                    : "secured";
    int bars = ap->rssi > -55 ? 4 : ap->rssi > -67 ? 3 : ap->rssi > -78 ? 2 : 1;
    const char *glyph = bars >= 4 ? "****" : bars == 3 ? "***-"
                      : bars == 2 ? "**--" : "*---";
    label_set_fmt_if_changed(s_lbl_apinfo, "%s   %d dBm   %s",
                             glyph, (int)ap->rssi, sec);
}

static void network_pick_cb(lv_event_t *e)
{
    (void)e;
    uint16_t idx = lv_dropdown_get_selected(s_dd_networks);
    if (idx < s_ap_n) {
        strncpy(s_ssid, (char *)s_aps[idx].ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
    }
    show_ap_info();
}

/*
 * The scan blocks for seconds, so it runs in a throwaway task and writes back
 * under the LVGL lock. Re-check the widget still exists inside the lock: the
 * user can leave this step while the scan is in flight.
 */
static void scan_task(void *arg)
{
    (void)arg;
    uint16_t n = MAX_SCAN_APS;
    esp_err_t err = wifi_mgr_scan(s_aps, &n);
    /* Count and signal only -- SSIDs go on the screen, not into a serial log
     * that tends to get pasted into issues. */
    ESP_LOGI(TAG, "scan: %s, %u networks, strongest %d dBm",
             esp_err_to_name(err), (unsigned)n, n ? (int)s_aps[0].rssi : 0);

    if (lvgl_port_lock(-1)) {
        if (s_dd_networks != NULL) {
            if (err != ESP_OK || n == 0) {
                s_ap_n = 0;
                lv_dropdown_set_options(s_dd_networks, "No networks found");
                set_status("Nothing found. Move closer to the access point "
                           "and scan again.", SEV_WARN);
            } else {
                s_ap_n = n;
                char opts[MAX_SCAN_APS * (33 + 1)] = "";
                size_t w = 0;
                for (uint16_t i = 0; i < n; i++) {
                    const char *ssid = (const char *)s_aps[i].ssid;
                    if (i > 0 && w + 1 < sizeof(opts)) opts[w++] = '\n';
                    for (const char *p = ssid; *p && w + 1 < sizeof(opts); p++) {
                        opts[w++] = *p;
                    }
                }
                opts[w] = '\0';
                lv_dropdown_set_options(s_dd_networks, opts);
                lv_dropdown_set_selected(s_dd_networks, 0);
                strncpy(s_ssid, (char *)s_aps[0].ssid, sizeof(s_ssid) - 1);
                show_ap_info();
                set_status(n == 1 ? "1 network found." : "", SEV_OK);
                if (n > 1) label_set_fmt_if_changed(s_lbl_status,
                                                    "%u networks found.", (unsigned)n);
            }
        }
        lvgl_port_unlock();
    }
    s_scan_busy = false;
    vTaskDelete(NULL);
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_busy) return;
    s_scan_busy = true;
    lv_dropdown_set_options(s_dd_networks, "Scanning...");
    set_status("Scanning for networks...", SEV_OK);
    /* 4KB is what esp32flight uses for the same job and has proven enough. */
    xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL);
}

static void pass_done_cb(const char *text, void *user)
{
    (void)user;
    if (text == NULL) return;               /* cancelled */
    strncpy(s_pass, text, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
    s_pass_entered = true;
    set_status(s_pass[0] ? "Password set. Tap Join." : "Password cleared.", SEV_OK);
}

static void pass_cb(lv_event_t *e)
{
    (void)e;
    ui_kbd_req_t req = {
        .title = "Wi-Fi password",
        .label = s_ssid[0] ? s_ssid : "Password",
        .value = s_pass,
        .hint  = "Tap the eye to check it before joining.",
        .kind  = KB_PASSWORD,
        .done  = pass_done_cb,
    };
    ui_kbd_edit(&req);
}

static void join_tick(lv_timer_t *t)
{
    (void)t;
    wifi_state_t st = wifi_mgr_state();

    if (st == WIFI_ST_CONNECTED) {
        lv_timer_del(s_join_timer); s_join_timer = NULL;
        secrets_set_wifi(s_ssid, s_pass_entered ? s_pass : NULL);
        build_done();
        return;
    }

    if (st == WIFI_ST_FAILED) {
        lv_timer_del(s_join_timer); s_join_timer = NULL;
        if (s_spinner) { lv_obj_del(s_spinner); s_spinner = NULL; }
        if (s_btn_join) lv_obj_clear_state(s_btn_join, LV_STATE_DISABLED);
        /* The driver's actual reason, not a generic failure -- "wrong
         * password" and "network not found" need different fixes. */
        char msg[96];
        snprintf(msg, sizeof(msg), "Could not join: %s", wifi_mgr_fail_reason());
        set_status(msg, SEV_CRIT);
        return;
    }

    if (lv_tick_get() > s_join_deadline) {
        lv_timer_del(s_join_timer); s_join_timer = NULL;
        if (s_spinner) { lv_obj_del(s_spinner); s_spinner = NULL; }
        if (s_btn_join) lv_obj_clear_state(s_btn_join, LV_STATE_DISABLED);
        const char *why = wifi_mgr_fail_reason();
        char msg[96];
        snprintf(msg, sizeof(msg), "Timed out%s%s",
                 why[0] ? ": " : ".", why[0] ? why : "");
        set_status(msg, SEV_CRIT);
    }
}

static void join_cb(lv_event_t *e)
{
    (void)e;
    if (s_ssid[0] == '\0') { set_status("Pick a network first.", SEV_WARN); return; }

    set_status("Joining...", SEV_OK);
    lv_obj_add_state(s_btn_join, LV_STATE_DISABLED);

    if (s_spinner == NULL) {
        s_spinner = lv_spinner_create(s_step_page, 900, 60);
        lv_obj_set_size(s_spinner, 32, 32);
        lv_obj_set_pos(s_spinner, PAD_L + 20, 330);
        lv_obj_set_style_arc_color(s_spinner, COL_ACCENT, LV_PART_INDICATOR);
    }

    wifi_mgr_connect(s_ssid, s_pass_entered ? s_pass : NULL);

    s_join_deadline = lv_tick_get() + 20000;
    if (s_join_timer == NULL) s_join_timer = lv_timer_create(join_tick, 250, NULL);
}

static void back_cb(lv_event_t *e) { (void)e; build_welcome(); }

static void build_wifi(void)
{
    clear_page();
    add_close_button();
    step_heading("STEP 1 OF 2", "Wi-Fi");

    lv_obj_t *cap = make_label(s_step_page, FONT_S, COL_DIM);
    lv_label_set_text(cap, "Network");
    lv_obj_set_pos(cap, PAD_L + 20, 130);

    s_dd_networks = make_dropdown(s_step_page, "Scanning...", network_pick_cb, NULL);
    lv_obj_set_width(s_dd_networks, 480);
    lv_obj_set_pos(s_dd_networks, PAD_L + 20, 154);

    lv_obj_t *rescan = make_btn(s_step_page, LV_SYMBOL_REFRESH, rescan_cb, NULL);
    lv_obj_set_size(rescan, 90, FIELD_H);
    lv_obj_set_pos(rescan, PAD_L + 20 + 480 + 10, 154);

    s_lbl_apinfo = make_label(s_step_page, FONT_S, COL_DIM);
    lv_obj_set_pos(s_lbl_apinfo, PAD_L + 20, 210);

    lv_obj_t *pcap = make_label(s_step_page, FONT_S, COL_DIM);
    lv_label_set_text(pcap, "Password");
    lv_obj_set_pos(pcap, PAD_L + 20, 244);

    lv_obj_t *pbtn = make_btn(s_step_page, "Tap to enter password", pass_cb, NULL);
    lv_obj_set_size(pbtn, 480, FIELD_H);
    lv_obj_set_pos(pbtn, PAD_L + 20, 268);

    s_lbl_status = make_label(s_step_page, FONT_S, COL_DIM);
    lv_obj_set_width(s_lbl_status, SCR_W - 2 * (PAD_L + 20) - 60);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_status, PAD_L + 20 + 44, 334);

    lv_obj_t *back = make_btn(s_step_page, LV_SYMBOL_LEFT "  Back", back_cb, NULL);
    lv_obj_set_size(back, 140, BTN_H);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, PAD_L + 20, -30);

    s_btn_join = make_btn_accent(s_step_page, "Join  " LV_SYMBOL_RIGHT, join_cb, NULL);
    lv_obj_set_size(s_btn_join, 200, BTN_H);
    lv_obj_align(s_btn_join, LV_ALIGN_BOTTOM_RIGHT, -PAD_L - 20, -30);

    /* Kick a scan immediately -- the first thing a new device needs is to
     * show the user their own network name. */
    rescan_cb(NULL);
}

/* -------------------------------------------------------------------- done */

static void build_done(void)
{
    clear_page();
    add_close_button();

    char ip[16] = "";
    int8_t rssi = 0;
    wifi_mgr_info(ip, sizeof(ip), &rssi);
    ESP_LOGI(TAG, "connected, ip %s rssi %d", ip, (int)rssi);

    lv_obj_t *ok = make_label(s_step_page, FONT_XL, COL_OK);
    lv_label_set_text(ok, LV_SYMBOL_OK "  Connected");
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *info = make_label(s_step_page, FONT_L, COL_TEXT);
    lv_label_set_text_fmt(info, "%s      %s      %d dBm", s_ssid, ip, (int)rssi);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 180);

    lv_obj_t *next = make_label(s_step_page, FONT_M, COL_DIM);
    lv_label_set_text(next, "The gear button sets which endpoint to poll.");
    lv_obj_set_style_text_align(next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(next, LV_ALIGN_TOP_MID, 0, 240);

    lv_obj_t *fin = make_btn_accent(s_step_page, LV_SYMBOL_OK "  Done",
                                    close_cb, NULL);
    lv_obj_set_size(fin, 200, BTN_H);
    lv_obj_align(fin, LV_ALIGN_BOTTOM_MID, 0, -40);
}

/* -------------------------------------------------------------------- open */

void ui_setup_close(void)
{
    if (s_root == NULL) return;

    /* A join can still be in flight; its timer would fire into a deleted
     * tree. */
    if (s_join_timer) { lv_timer_del(s_join_timer); s_join_timer = NULL; }

    lv_obj_del(s_root);
    s_root = NULL;
    s_step_page = NULL;
    s_dd_networks = s_lbl_apinfo = s_lbl_status = NULL;
    s_btn_join = s_spinner = NULL;

    /* The scan task checks these under the LVGL lock before touching them, so
     * clearing them is what makes an in-flight scan safe. */
    if (s_on_close) s_on_close();
}

void ui_setup_open(void (*on_close)(void))
{
    if (s_root) return;
    s_on_close = on_close;

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, SCR_W, SCR_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    secrets_get_wifi(s_ssid, sizeof(s_ssid), NULL, 0);
    build_welcome();
}

bool ui_setup_is_open(void) { return s_root != NULL; }
