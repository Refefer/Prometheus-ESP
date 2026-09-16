#include "ui_widgets.h"
#include "ui_layout.h"
#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------- diffing setters */

void label_set_if_changed(lv_obj_t *l, const char *txt)
{
    if (l == NULL || txt == NULL) return;
    const char *cur = lv_label_get_text(l);
    if (cur != NULL && strcmp(cur, txt) == 0) return;
    lv_label_set_text(l, txt);
}

void label_set_fmt_if_changed(lv_obj_t *l, const char *fmt, ...)
{
    if (l == NULL) return;
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    label_set_if_changed(l, buf);
}

void text_color_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (o == NULL) return;
    if (lv_obj_get_style_text_color(o, 0).full == c.full) return;
    lv_obj_set_style_text_color(o, c, 0);
}

void bg_color_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (o == NULL) return;
    if (lv_obj_get_style_bg_color(o, 0).full == c.full) return;
    lv_obj_set_style_bg_color(o, c, 0);
}

void border_color_if_changed(lv_obj_t *o, lv_color_t c)
{
    if (o == NULL) return;
    if (lv_obj_get_style_border_color(o, 0).full == c.full) return;
    lv_obj_set_style_border_color(o, c, 0);
}

void opa_if_changed(lv_obj_t *o, lv_opa_t opa)
{
    if (o == NULL) return;
    if (lv_obj_get_style_opa(o, 0) == opa) return;
    lv_obj_set_style_opa(o, opa, 0);
}

void hidden_if_changed(lv_obj_t *o, bool hidden)
{
    if (o == NULL) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ factories */

lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *c = make_panel(parent);
    lv_obj_set_style_radius(c, RADIUS_TILE, 0);
    lv_obj_set_style_pad_all(c, PAD_S, 0);
    return c;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *btn_common(lv_obj_t *parent, const char *text, lv_color_t bg,
                            lv_color_t fg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, BTN_H);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, RADIUS_CTRL, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_LINE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    /* A pressed state that is a flat tint rather than a shadow: RGB565 bands
     * on gradients, and shadows cost a blur pass we do not need. */
    lv_obj_set_style_bg_color(b, COL_PANEL_ALT, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, FONT_M, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

lv_obj_t *make_btn(lv_obj_t *parent, const char *text,
                   lv_event_cb_t cb, void *user_data)
{
    return btn_common(parent, text, COL_PANEL, COL_TEXT, cb, user_data);
}

lv_obj_t *make_btn_accent(lv_obj_t *parent, const char *text,
                          lv_event_cb_t cb, void *user_data)
{
    const app_theme_t *t = app_theme();
    lv_obj_t *b = btn_common(parent, text, t->accent,
                             t->is_dark ? t->bg : lv_color_hex(0xFFFFFF),
                             cb, user_data);
    lv_obj_set_style_border_width(b, 0, 0);
    return b;
}

lv_obj_t *make_chip(lv_obj_t *parent, const char *text,
                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, COL_PANEL_ALT, 0);
    lv_obj_set_style_bg_color(b, COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, RADIUS_CHIP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, PAD_M, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, FONT_S, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

lv_obj_t *make_field(lv_obj_t *parent, const char *value, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_height(ta, FIELD_H);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_text(ta, value ? value : "");
    lv_obj_set_style_bg_color(ta, COL_PANEL, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta, FONT_M, 0);
    lv_obj_set_style_border_color(ta, COL_LINE, 0);
    lv_obj_set_style_border_color(ta, COL_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, RADIUS_CTRL, 0);
    return ta;
}

lv_obj_t *make_dropdown(lv_obj_t *parent, const char *options,
                        lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options ? options : "");
    lv_obj_set_height(dd, FIELD_H);
    lv_obj_set_style_bg_color(dd, COL_PANEL, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    lv_obj_set_style_text_font(dd, FONT_M, 0);
    lv_obj_set_style_border_color(dd, COL_LINE, 0);
    lv_obj_set_style_radius(dd, RADIUS_CTRL, 0);

    /* The popup list is a separate object and inherits none of the above. */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, COL_PANEL, 0);
        lv_obj_set_style_text_color(list, COL_TEXT, 0);
        lv_obj_set_style_text_font(list, FONT_M, 0);
        lv_obj_set_style_border_color(list, COL_LINE, 0);
        lv_obj_set_style_bg_color(list, COL_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    }
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return dd;
}

lv_obj_t *make_switch(lv_obj_t *parent, bool on, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 56, 28);
    lv_obj_set_style_bg_color(sw, COL_PANEL_ALT, 0);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

lv_obj_t *make_divider(lv_obj_t *parent, lv_coord_t w)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, w, 1);
    lv_obj_set_style_bg_color(d, COL_LINE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

lv_obj_t *make_dot(lv_obj_t *parent, lv_coord_t d, lv_color_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, colour, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ------------------------------------------------------------------- toast */

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_expire(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_del(s_toast); s_toast = NULL; }
    if (s_toast_timer) { lv_timer_del(s_toast_timer); s_toast_timer = NULL; }
}

static const char *TAG = "ui";

#define SIG_BARS 4

lv_obj_t *make_signal(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 26, 18);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    /* Bottom-aligned and climbing, so the shape reads as a ramp even before
     * the colours are taken in. */
    for (int i = 0; i < SIG_BARS; i++) {
        lv_obj_t *b = lv_obj_create(box);
        lv_coord_t h = (lv_coord_t)(5 + i * 4);
        lv_obj_set_size(b, 4, h);
        lv_obj_set_pos(b, i * 6, 18 - h);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    return box;
}

void signal_set_level(lv_obj_t *sig, int level)
{
    if (sig == NULL) return;
    const app_theme_t *t = app_theme();

    /*
     * Colour says whether the link is a problem, height says how much signal
     * there is. Four bars is fine, three is fine, two is worth noticing and
     * one is nearly gone -- which maps onto the semantic palette directly.
     */
    lv_color_t on = level >= 3 ? t->ok : level == 2 ? t->warn : t->crit;

    uint32_t n = lv_obj_get_child_cnt(sig);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(sig, i);
        bool lit = ((int)i < level);
        bg_color_if_changed(b, lit ? on : t->line);
        opa_if_changed(b, lit ? LV_OPA_COVER : LV_OPA_40);
    }
}

void ui_check_overlaps(lv_obj_t *root, const char *what)
{
    if (root == NULL) return;
    lv_obj_update_layout(root);

    uint32_t n = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *a = lv_obj_get_child(root, i);
        if (lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t ra;
        lv_obj_get_coords(a, &ra);

        for (uint32_t j = i + 1; j < n; j++) {
            lv_obj_t *b = lv_obj_get_child(root, j);
            if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) continue;
            lv_area_t rb;
            lv_obj_get_coords(b, &rb);

            if (ra.x2 < rb.x1 || rb.x2 < ra.x1 ||
                ra.y2 < rb.y1 || rb.y2 < ra.y1) continue;

            /* ESP_LOG, not LV_LOG: LVGL's logging is compiled out in this
             * build, and a guard that reports nothing is worse than none. */
            ESP_LOGW(TAG, "%s: children %u and %u overlap "
                          "(%d,%d..%d,%d vs %d,%d..%d,%d)",
                        what, (unsigned)i, (unsigned)j,
                        (int)ra.x1, (int)ra.y1, (int)ra.x2, (int)ra.y2,
                        (int)rb.x1, (int)rb.y1, (int)rb.x2, (int)rb.y2);
        }
    }
}

void ui_toast(const char *text, severity_t sev, uint32_t ms)
{
    toast_expire(NULL);   /* only ever one; a queue of these would obscure the UI */

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_toast, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, app_theme_sev(sev), 0);
    lv_obj_set_style_radius(s_toast, RADIUS_CTRL, 0);
    lv_obj_set_style_pad_all(s_toast, PAD_M, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = make_label(s_toast, FONT_M, app_theme_sev(sev));
    lv_label_set_text(l, text);
    lv_obj_center(l);

    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H - PAD_M);

    s_toast_timer = lv_timer_create(toast_expire, ms ? ms : 2500, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}
