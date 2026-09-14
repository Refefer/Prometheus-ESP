#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------- keymaps */

/*
 * LVGL 8.4 exposes LV_KEYBOARD_MODE_USER_1..4 plus lv_keyboard_set_map(), so
 * these are a real feature rather than a workaround.
 *
 * One gotcha worth knowing: lv_keyboard_set_map stores the map in a FILE-LEVEL
 * array inside lv_keyboard.c, not per widget. That is harmless here because
 * there is exactly one keyboard in the app, but it means a second keyboard
 * would silently share these maps.
 *
 * The shift key is LV_SYMBOL_UP and is handled by our own event callback --
 * the stock "abc"/"ABC" keys would switch to the BUILT-IN maps and throw away
 * the URL layout, which is the whole point of having one.
 */
#define KB_MODE_URL_LO  LV_KEYBOARD_MODE_USER_1
#define KB_MODE_URL_UP  LV_KEYBOARD_MODE_USER_2
#define KB_MODE_PROMQL  LV_KEYBOARD_MODE_USER_3

static const char *const k_url_lo_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", ".", "/", "\n",
    LV_SYMBOL_BACKSPACE, ":", "_", "~", "?", " ", LV_SYMBOL_OK, ""
};

static const char *const k_url_up_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", ".", "/", "\n",
    LV_SYMBOL_BACKSPACE, ":", "_", "~", "?", " ", LV_SYMBOL_OK, ""
};

/* Widths in grid units; rows are normalised independently by lv_btnmatrix. */
static const lv_btnmatrix_ctrl_t k_url_ctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 1, 1, 1, 1, 3, 2
};

static const char *const k_promql_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "_", "\n",
    "z", "x", "c", "v", "b", "n", "m", "(", ")", "\"", "\n",
    "{", "}", "[", "]", "=", "!", "~", ",", ".", "/", "\n",
    LV_SYMBOL_BACKSPACE, " ", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t k_promql_ctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 4, 2
};

/* ----------------------------------------------------------------- state */

static lv_obj_t *s_kb;

/* Inline mode */
static lv_obj_t *s_inline_page;
static lv_coord_t s_inline_h;      /* the page height to restore on blur */

/* Modal mode */
static lv_obj_t    *s_sheet;
static lv_obj_t    *s_sheet_ta;
static lv_obj_t    *s_sheet_status;
static lv_timer_t  *s_validate_timer;
static ui_kbd_req_t s_req;

static lv_keyboard_mode_t mode_for(kb_kind_t k)
{
    switch (k) {
    case KB_NUMBER: return LV_KEYBOARD_MODE_NUMBER;
    case KB_URL:    return KB_MODE_URL_LO;
    case KB_PROMQL: return KB_MODE_PROMQL;
    case KB_PASSWORD:
    case KB_TEXT:
    default:        return LV_KEYBOARD_MODE_TEXT_LOWER;
    }
}

static void kb_show(lv_obj_t *ta, kb_kind_t kind)
{
    lv_keyboard_set_mode(s_kb, mode_for(kind));
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
}

static void kb_hide(void)
{
    if (s_kb == NULL) return;
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------- event handling */

/*
 * Wraps LVGL's default handler so the shift key can switch between our two
 * URL maps. Without the intercept the default handler would fall through to
 * its final else-branch and type the shift glyph into the textarea.
 */
static void kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        uint16_t id = lv_btnmatrix_get_selected_btn(kb);
        const char *txt = lv_btnmatrix_get_btn_text(kb, id);
        if (txt != NULL && strcmp(txt, LV_SYMBOL_UP) == 0) {
            lv_keyboard_mode_t m = lv_keyboard_get_mode(kb);
            lv_keyboard_set_mode(kb, m == KB_MODE_URL_LO ? KB_MODE_URL_UP
                                                        : KB_MODE_URL_LO);
            return;   /* swallow: do not let the default handler type it */
        }
    }

    lv_keyboard_def_event_cb(e);

    /* READY (the tick) and CANCEL (the close key) both dismiss. In modal mode
     * the sheet owns dismissal, so only the inline path acts here. */
    if ((code == LV_EVENT_READY || code == LV_EVENT_CANCEL) && s_sheet == NULL) {
        kb_hide();
        if (s_inline_page) {
            lv_obj_set_height(s_inline_page, s_inline_h);
            s_inline_page = NULL;
        }
    }
}

/* ------------------------------------------------------------ inline mode */

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    kb_kind_t kind = (kb_kind_t)(intptr_t)lv_event_get_user_data(e);

    kb_show(ta, kind);

    /*
     * Shrink the scroll page to the strip above the keyboard and bring the
     * field into view. Without this the field being edited can sit behind the
     * keyboard, which at 480px tall happens constantly.
     */
    lv_obj_t *page = lv_obj_get_parent(ta);
    while (page && !lv_obj_has_flag(page, LV_OBJ_FLAG_SCROLLABLE)) {
        page = lv_obj_get_parent(page);
    }
    if (page && page != lv_scr_act()) {
        if (s_inline_page != page) {
            s_inline_page = page;
            s_inline_h    = lv_obj_get_height(page);
        }
        lv_coord_t avail = EDIT_STRIP_H - lv_obj_get_y(page);
        if (avail > 80) lv_obj_set_height(page, avail);
        lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
    }
}

static void ta_defocus_cb(lv_event_t *e)
{
    if (s_kb == NULL) return;
    if (lv_keyboard_get_textarea(s_kb) != lv_event_get_target(e)) return;
    kb_hide();
    if (s_inline_page) {
        lv_obj_set_height(s_inline_page, s_inline_h);
        s_inline_page = NULL;
    }
}

void ui_kbd_attach(lv_obj_t *ta, kb_kind_t kind, lv_obj_t *page)
{
    (void)page;   /* the scrollable ancestor is found on focus instead, so a
                   * caller cannot pass a stale pointer */
    lv_obj_add_event_cb(ta, ta_focus_cb,   LV_EVENT_FOCUSED,   (void *)(intptr_t)kind);
    lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
}

/* ------------------------------------------------------------- modal mode */

static void sheet_close(const char *result)
{
    if (s_sheet == NULL) return;

    if (s_validate_timer) { lv_timer_del(s_validate_timer); s_validate_timer = NULL; }

    /* Copy the text out before deleting the tree it lives in. */
    char buf[512];
    buf[0] = '\0';
    if (result) {
        strncpy(buf, result, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }

    void (*done)(const char *, void *) = s_req.done;
    void *user = s_req.user;

    kb_hide();
    lv_obj_del(s_sheet);
    s_sheet = NULL; s_sheet_ta = NULL; s_sheet_status = NULL;

    if (done) done(result ? buf : NULL, user);
}

static void sheet_ok_cb(lv_event_t *e)
{
    (void)e;
    sheet_close(s_sheet_ta ? lv_textarea_get_text(s_sheet_ta) : "");
}

static void sheet_cancel_cb(lv_event_t *e) { (void)e; sheet_close(NULL); }

static void run_validate(lv_timer_t *t)
{
    (void)t;
    if (s_sheet_ta == NULL || s_req.validate == NULL) return;
    char msg[96] = "";
    severity_t sev = SEV_OK;
    s_req.validate(lv_textarea_get_text(s_sheet_ta), s_req.user,
                   msg, sizeof(msg), &sev);
    ui_kbd_set_status(msg, sev);
}

static void sheet_ta_changed(lv_event_t *e)
{
    (void)e;
    if (s_req.validate == NULL) return;
    /* Debounce: validating on every keystroke of a URL means a DNS lookup per
     * character. */
    if (s_validate_timer) lv_timer_reset(s_validate_timer);
}

static void chip_cb(lv_event_t *e)
{
    const char *txt = lv_event_get_user_data(e);
    if (s_sheet_ta && txt) {
        lv_textarea_add_text(s_sheet_ta, txt);
        sheet_ta_changed(e);
    }
}

static void recent_cb(lv_event_t *e)
{
    const char *txt = lv_event_get_user_data(e);
    if (s_sheet_ta && txt) {
        lv_textarea_set_text(s_sheet_ta, txt);
        sheet_ta_changed(e);
    }
}

static void reveal_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (s_sheet_ta == NULL) return;
    bool now = lv_textarea_get_password_mode(s_sheet_ta);
    lv_textarea_set_password_mode(s_sheet_ta, !now);
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) lv_label_set_text(l, now ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

void ui_kbd_set_status(const char *msg, severity_t sev)
{
    if (s_sheet_status == NULL) return;
    label_set_if_changed(s_sheet_status, msg ? msg : "");
    text_color_if_changed(s_sheet_status,
                          sev == SEV_OK ? COL_OK : app_theme_sev(sev));
}

void ui_kbd_edit(const ui_kbd_req_t *req)
{
    if (req == NULL || s_kb == NULL) return;
    if (s_sheet) sheet_close(NULL);
    s_req = *req;

    /* The sheet is a direct child of the screen with GESTURE_BUBBLE cleared,
     * so a swipe inside it stops here and never pages the dashboard behind. */
    s_sheet = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_sheet, SCR_W, EDIT_STRIP_H);
    lv_obj_set_pos(s_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_sheet, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_set_style_pad_all(s_sheet, 0, 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* header: cancel | title | done */
    lv_obj_t *cancel = make_btn(s_sheet, LV_SYMBOL_CLOSE, sheet_cancel_cb, NULL);
    lv_obj_set_size(cancel, 56, 40);
    lv_obj_set_pos(cancel, PAD_M, 2);

    lv_obj_t *title = make_label(s_sheet, FONT_L, COL_TEXT);
    lv_label_set_text(title, req->title ? req->title : "");
    lv_obj_set_pos(title, 80, 10);

    lv_obj_t *done = make_btn_accent(s_sheet, LV_SYMBOL_OK "  Done", sheet_ok_cb, NULL);
    lv_obj_set_size(done, 120, 40);
    lv_obj_set_pos(done, SCR_W - 120 - PAD_M, 2);

    lv_obj_t *cap = make_label(s_sheet, FONT_S, COL_DIM);
    lv_label_set_text(cap, req->label ? req->label : "");
    lv_obj_set_pos(cap, PAD_L, 50);

    bool password = (req->kind == KB_PASSWORD);
    lv_coord_t field_w = SCR_W - 2 * PAD_L - (password ? 60 : 0);
    s_sheet_ta = make_field(s_sheet, req->value, password);
    lv_obj_set_width(s_sheet_ta, field_w);
    lv_obj_set_pos(s_sheet_ta, PAD_L, 72);
    lv_obj_add_event_cb(s_sheet_ta, sheet_ta_changed, LV_EVENT_VALUE_CHANGED, NULL);

    if (password) {
        /* Typing a WPA passphrase blind and getting a silent failure twenty
         * seconds later is the worst moment in setting up any device. */
        lv_obj_t *eye = make_btn(s_sheet, LV_SYMBOL_EYE_CLOSE, reveal_cb, NULL);
        lv_obj_set_size(eye, 52, FIELD_H);
        lv_obj_set_pos(eye, PAD_L + field_w + 8, 72);
    }

    lv_coord_t y = 72 + FIELD_H + 8;   /* 128 */

    if (req->chips && req->chips[0]) {
        lv_obj_t *row = lv_obj_create(s_sheet);
        lv_obj_set_size(row, SCR_W - 2 * PAD_L, 44);
        lv_obj_set_pos(row, PAD_L, y);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_scroll_dir(row, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        for (int i = 0; req->chips[i]; i++) {
            make_chip(row, req->chips[i], chip_cb, (void *)req->chips[i]);
        }
        y += 50;
    }

    s_sheet_status = make_label(s_sheet, FONT_S, COL_DIM);
    lv_label_set_text(s_sheet_status, req->hint ? req->hint : "");
    lv_obj_set_width(s_sheet_status, SCR_W - 2 * PAD_L);
    lv_label_set_long_mode(s_sheet_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_sheet_status, PAD_L, y);
    y += 26;

    if (req->recents && req->recents[0] && y < EDIT_STRIP_H - 40) {
        lv_obj_t *rl = make_label(s_sheet, FONT_S, COL_DIM);
        lv_label_set_text(rl, "Recent");
        lv_obj_set_pos(rl, PAD_L, y);
        y += 20;

        lv_obj_t *col = lv_obj_create(s_sheet);
        lv_obj_set_size(col, SCR_W - 2 * PAD_L, EDIT_STRIP_H - y - 4);
        lv_obj_set_pos(col, PAD_L, y);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(col, LV_DIR_VER);
        for (int i = 0; req->recents[i]; i++) {
            lv_obj_t *b = make_chip(col, req->recents[i], recent_cb,
                                    (void *)req->recents[i]);
            lv_obj_set_width(b, LV_PCT(100));
        }
    }

    if (req->validate) {
        s_validate_timer = lv_timer_create(run_validate, 400, NULL);
        lv_timer_set_repeat_count(s_validate_timer, -1);
    }

    kb_show(s_sheet_ta, req->kind);
    /* Put the caret at the end so chips append rather than prepend. */
    lv_textarea_set_cursor_pos(s_sheet_ta, LV_TEXTAREA_CURSOR_LAST);
}

bool ui_kbd_is_open(void) { return s_sheet != NULL; }

/* -------------------------------------------------------------------- init */

void ui_kbd_init(void)
{
    if (s_kb) return;

    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, SCR_W, KB_H);
    lv_obj_set_pos(s_kb, 0, KB_Y);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lv_keyboard_set_map(s_kb, KB_MODE_URL_LO,
                        (const char **)k_url_lo_map, k_url_ctrl);
    lv_keyboard_set_map(s_kb, KB_MODE_URL_UP,
                        (const char **)k_url_up_map, k_url_ctrl);
    lv_keyboard_set_map(s_kb, KB_MODE_PROMQL,
                        (const char **)k_promql_map, k_promql_ctrl);

    lv_obj_set_style_bg_color(s_kb, COL_PANEL, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, PAD_S, 0);
    lv_obj_set_style_pad_gap(s_kb, 4, 0);
    lv_obj_set_style_bg_color(s_kb, COL_PANEL_ALT, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kb, FONT_M, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, RADIUS_CTRL, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);

    /* Replace, don't stack: our wrapper calls the default handler itself for
     * everything except the shift key. */
    lv_obj_remove_event_cb(s_kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_kb, kb_event, LV_EVENT_ALL, NULL);
}
