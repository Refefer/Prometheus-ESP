/*
 * The layout picker.
 *
 * A layout is the presentation half of the configuration -- the tiles and
 * where they sit -- stored under a name. Endpoint definitions are deliberately
 * not part of one: the same URL serves entirely different metrics depending
 * on what is running behind it, so switching what you are looking at must not
 * change what you are connected to. Each screen does keep the id of the
 * endpoint it reads, so a layout brings its screen-to-endpoint mapping along.
 */

#include "ui_layouts.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

#include "config.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui_layouts";

/*
 * The arrangement you are looking at when you switch away, if it was never
 * saved. Every panel on this device is placed by hand, so activating a layout
 * over an unnamed arrangement would throw away work with no way back. One
 * reused filename, announced in a toast, costs a few KB of flash and makes
 * the switch reversible.
 */
#define AUTOSAVE_NAME "autosave"

#define ROW_H        46
#define LIST_Y       112
#define LIST_H       268

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_hint;
static void (*s_on_close)(void);

static char s_names[CFG_MAX_LAYOUTS][CFG_LAYOUT_NAME_MAX];
static int  s_n;

/* The delete button that is armed, and the timer that disarms it. `armed` is
 * an index into s_names rather than a widget pointer, because the list is
 * rebuilt underneath it. */
static int          s_armed = -1;
static lv_timer_t  *s_disarm;

/* The keyboard sheet keeps the pointer it is handed rather than copying the
 * string, so the initial value has to outlive the call that opens it. */
static char s_kbd_value[CFG_LAYOUT_NAME_MAX];

static void refresh_list(void);

static void close_overlay(void)
{
    if (s_root == NULL) return;
    if (s_disarm) { lv_timer_del(s_disarm); s_disarm = NULL; }
    s_armed = -1;
    lv_obj_del(s_root);
    s_root = NULL;
    s_list = s_hint = NULL;
    if (s_on_close) s_on_close();
}

static void close_cb(lv_event_t *e) { (void)e; close_overlay(); }

/* ------------------------------------------------------------- activation */

static void activate(const char *name)
{
    const char *active = config_active_layout();
    bool same = (strcmp(active, name) == 0);

    if (!same && active[0] == '\0' && config_get()->n_panels > 0) {
        if (config_layout_save(AUTOSAVE_NAME) == ESP_OK) {
            ui_toast("Saved what was on screen as '" AUTOSAVE_NAME "'",
                     SEV_OK, 2600);
        }
    }

    esp_err_t rc = config_layout_load(name);
    if (rc != ESP_OK) {
        ui_toast("That layout would not load", SEV_CRIT, 2600);
        ESP_LOGW(TAG, "activate '%s': %s", name, esp_err_to_name(rc));
        return;
    }
    config_flush();
    /* The poller and the dashboard both watch the config generation, so
     * neither needs telling; closing rebuilds the tiles. */
    close_overlay();
}

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    activate(s_names[i]);
}

/* ----------------------------------------------------------------- delete */

static void disarm_cb(lv_timer_t *t)
{
    (void)t;
    s_disarm = NULL;
    s_armed  = -1;
    refresh_list();
}

static void del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;

    if (s_armed != i) {
        /* First tap arms, second deletes. A single tap on a 40px target next
         * to the row you meant to activate is too easy to hit by accident,
         * and there is no undo for a deleted file. */
        s_armed = i;
        if (s_disarm) lv_timer_del(s_disarm);
        s_disarm = lv_timer_create(disarm_cb, 4000, NULL);
        lv_timer_set_repeat_count(s_disarm, 1);
        refresh_list();
        return;
    }

    if (s_disarm) { lv_timer_del(s_disarm); s_disarm = NULL; }
    s_armed = -1;

    char name[CFG_LAYOUT_NAME_MAX];
    strncpy(name, s_names[i], sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    if (config_layout_delete(name) == ESP_OK) {
        config_flush();
        ui_toast("Deleted", SEV_OK, 1600);
    } else {
        ui_toast("Could not delete that layout", SEV_CRIT, 2400);
    }
    refresh_list();
}

/* ------------------------------------------------------------------- save */

static void name_validate(const char *text, void *user, char *msg, size_t cap,
                          severity_t *sev)
{
    (void)user;
    if (text == NULL || text[0] == '\0') {
        snprintf(msg, cap, "Give it a name");
        *sev = SEV_STALE;
        return;
    }
    for (const char *p = text; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (!ok) {
            snprintf(msg, cap, "Letters, digits, - and _ only");
            *sev = SEV_CRIT;
            return;
        }
    }
    if (strlen(text) >= CFG_LAYOUT_NAME_MAX - 6) {
        snprintf(msg, cap, "Too long");
        *sev = SEV_CRIT;
        return;
    }

    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_names[i], text) == 0) {
            snprintf(msg, cap, "Replaces the saved '%s'", text);
            *sev = SEV_WARN;
            return;
        }
    }
    snprintf(msg, cap, "New layout");
    *sev = SEV_OK;
}

static void name_done(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == '\0') return;   /* cancelled */

    esp_err_t rc = config_layout_save(text);
    if (rc != ESP_OK) {
        ui_toast(rc == ESP_ERR_INVALID_ARG ? "Letters, digits, - and _ only"
                                           : "Could not save that layout",
                 SEV_CRIT, 2600);
        return;
    }
    config_flush();
    ui_toast("Saved", SEV_OK, 1600);
    refresh_list();
}

static void save_as_cb(lv_event_t *e)
{
    (void)e;
    if (s_n >= CFG_MAX_LAYOUTS) {
        ui_toast("That is all the layouts this holds -- delete one first",
                 SEV_WARN, 3000);
        return;
    }

    s_kbd_value[0] = '\0';
    ui_kbd_req_t req = {
        .title    = "Save layout",
        .label    = "Name",
        .value    = s_kbd_value,
        .hint     = "Tiles, positions and which endpoint each screen reads.",
        .kind     = KB_TEXT,
        .validate = name_validate,
        .done     = name_done,
    };
    ui_kbd_edit(&req);
}

static void update_cb(lv_event_t *e)
{
    (void)e;
    const char *active = config_active_layout();
    if (active[0] == '\0') return;
    if (config_layout_save(active) != ESP_OK) {
        ui_toast("Could not save that layout", SEV_CRIT, 2400);
        return;
    }
    config_flush();
    ui_toast("Updated", SEV_OK, 1600);
    refresh_list();
}

/* ------------------------------------------------------------------- list */

static void add_row(int i, const char *name, bool active)
{
    lv_obj_t *row = make_panel(s_list);
    lv_obj_set_size(row, SCR_W - 2 * GRID_MX - 14, ROW_H - 6);
    lv_obj_set_pos(row, 0, i * ROW_H);
    lv_obj_set_style_bg_color(row, active ? COL_PANEL_ALT : COL_PANEL, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    if (active) {
        lv_obj_set_style_border_color(row, COL_ACCENT, 0);
        lv_obj_set_style_border_width(row, 1, 0);
    }

    if (active) {
        lv_obj_t *dot = make_dot(row, 10, COL_ACCENT);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
    }

    lv_obj_t *lbl = make_label(row, FONT_M, active ? COL_TEXT : COL_DIM);
    lv_label_set_text(lbl, name);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 34, 0);

    int n = config_layout_panel_count(name);
    lv_obj_t *cnt = make_label(row, FONT_S, COL_DIM);
    if (n < 0) lv_label_set_text(cnt, "unreadable");
    else       lv_label_set_text_fmt(cnt, "%d tile%s", n, n == 1 ? "" : "s");
    lv_obj_align(cnt, LV_ALIGN_LEFT_MID, 300, 0);

    if (active) {
        lv_obj_t *on = make_label(row, FONT_S, COL_ACCENT);
        lv_label_set_text(on, "on screen");
        lv_obj_align(on, LV_ALIGN_LEFT_MID, 420, 0);
    }

    bool armed = (s_armed == i);
    lv_obj_t *del = make_btn(row, armed ? "Delete?" : LV_SYMBOL_TRASH,
                             del_cb, (void *)(intptr_t)i);
    lv_obj_set_size(del, armed ? 104 : 56, 32);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -8, 0);
    if (armed) {
        lv_obj_set_style_bg_color(del, COL_CRIT, 0);
        lv_obj_set_style_text_color(del, COL_BG, 0);
    }
}

static void refresh_list(void)
{
    if (s_list == NULL) return;
    lv_obj_clean(s_list);

    s_n = config_layout_list(s_names, CFG_MAX_LAYOUTS);
    if (s_armed >= s_n) s_armed = -1;

    const char *active = config_active_layout();
    for (int i = 0; i < s_n; i++)
        add_row(i, s_names[i], strcmp(s_names[i], active) == 0);

    lv_obj_set_style_pad_bottom(s_list, 8, 0);

    if (s_hint) {
        if (s_n == 0) {
            label_set_if_changed(s_hint,
                "Nothing saved yet. Arrange the tiles you want, then "
                "Save as -- and the arrangement comes back with one tap.");
        } else if (active[0]) {
            label_set_fmt_if_changed(s_hint,
                "Showing '%s'. Tap another to switch; endpoint settings "
                "stay as they are.", active);
        } else {
            label_set_if_changed(s_hint,
                "The tiles on screen are not saved under any name yet.");
        }
    }
}

/* ------------------------------------------------------------------- open */

void ui_layouts_open(void (*on_close)(void))
{
    if (s_root) return;
    s_on_close = on_close;
    s_armed = -1;

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, SCR_W, SCR_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Swipes inside the picker are for its list, not for paging the
     * dashboard underneath. */
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *close = make_btn(s_root, LV_SYMBOL_CLOSE, close_cb, NULL);
    lv_obj_set_size(close, 52, 34);
    lv_obj_set_pos(close, GRID_MX, 8);

    lv_obj_t *title = make_label(s_root, FONT_XL, COL_TEXT);
    lv_label_set_text(title, "Layouts");
    lv_obj_set_pos(title, GRID_MX + 64, 8);

    s_hint = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_set_pos(s_hint, GRID_MX, 56);
    lv_obj_set_width(s_hint, SCR_W - 2 * GRID_MX);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);

    s_list = lv_obj_create(s_root);
    lv_obj_set_size(s_list, SCR_W - 2 * GRID_MX, LIST_H);
    lv_obj_set_pos(s_list, GRID_MX, LIST_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    /* Vertical only: a horizontal drag here would fight the page gesture. */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_CHAIN);

    const char *active = config_active_layout();
    if (active[0]) {
        char txt[48];
        snprintf(txt, sizeof(txt), LV_SYMBOL_SAVE "  Update '%s'", active);
        lv_obj_t *up = make_btn(s_root, txt, update_cb, NULL);
        lv_obj_set_size(up, 260, 44);
        lv_obj_set_pos(up, GRID_MX, SCR_H - 60);
    }

    lv_obj_t *sa = make_btn_accent(s_root, LV_SYMBOL_PLUS "  Save as",
                                   save_as_cb, NULL);
    lv_obj_set_size(sa, 200, 44);
    lv_obj_set_pos(sa, SCR_W - GRID_MX - 200, SCR_H - 60);

    refresh_list();
}

bool ui_layouts_is_open(void) { return s_root != NULL; }
