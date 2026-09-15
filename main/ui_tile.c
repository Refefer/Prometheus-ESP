/*
 * The tile shell and the renderer registry. Renderers live in
 * ui_tile_kinds.c.
 */
#include "ui_tile.h"
#include "ui_layout.h"
#include "ui_widgets.h"

#include "esp_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tile";

extern const tile_vt_t tile_stat_vt, tile_spark_vt, tile_chart_vt,
                       tile_bar_vt, tile_gauge_vt, tile_status_vt,
                       tile_hist_vt, tile_multi_vt;

static const tile_vt_t *const k_vt[TILE_KIND_COUNT] = {
    [TILE_STAT]   = &tile_stat_vt,
    [TILE_SPARK]  = &tile_spark_vt,
    [TILE_CHART]  = &tile_chart_vt,
    [TILE_BAR]    = &tile_bar_vt,
    [TILE_GAUGE]  = &tile_gauge_vt,
    [TILE_STATUS] = &tile_status_vt,
    [TILE_HIST]   = &tile_hist_vt,
    [TILE_MULTI]  = &tile_multi_vt,
};

static void (*s_tap_cb)(uint16_t);

void tile_set_tap_handler(void (*cb)(uint16_t panel_id))
{
    s_tap_cb = cb;
}

static void shell_clicked(lv_event_t *e)
{
    tile_inst_t *t = lv_event_get_user_data(e);
    if (s_tap_cb && t && t->spec) s_tap_cb(t->spec->panel_id);
}

const tile_vt_t *tile_vt(tile_kind_t k)
{
    return (k < TILE_KIND_COUNT) ? k_vt[k] : k_vt[TILE_STAT];
}

/* Resolve a kind that fits the span, following fallbacks. A chart forced into
 * a 1x1 becomes a sparkline rather than drawing axes nobody can read. */
static const tile_vt_t *vt_for_span(tile_kind_t kind, uint8_t w, uint8_t h)
{
    for (int guard = 0; guard < TILE_KIND_COUNT; guard++) {
        const tile_vt_t *vt = tile_vt(kind);
        if (w >= vt->min_w && h >= vt->min_h) return vt;
        tile_kind_t next = vt->fallback;
        if (next == kind) break;
        kind = next;
    }
    return tile_vt(TILE_STAT);
}

static severity_t severity_of(const tile_spec_t *spec, const tile_data_t *d)
{
    if (!d->valid) return SEV_STALE;
    if (isnan(spec->crit) && isnan(spec->warn)) return SEV_OK;

    float v = d->value;
    if (spec->lower_is_worse) {
        if (!isnan(spec->crit) && v <= spec->crit) return SEV_CRIT;
        if (!isnan(spec->warn) && v <= spec->warn) return SEV_WARN;
    } else {
        if (!isnan(spec->crit) && v >= spec->crit) return SEV_CRIT;
        if (!isnan(spec->warn) && v >= spec->warn) return SEV_WARN;
    }
    return SEV_OK;
}

static void make_inert(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /*
     * And nothing inside a tile scrolls, ever.
     *
     * A scrollable ancestor claims the drag before gesture detection runs, so
     * a chart wide enough to scroll would eat the swipe on exactly the tiles
     * a finger is most likely to start on. lv_chart and lv_bar are scrollable
     * by default, same as they are clickable by default.
     */
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    uint32_t n = lv_obj_get_child_cnt(o);
    for (uint32_t i = 0; i < n; i++) make_inert(lv_obj_get_child(o, i));
}

tile_inst_t *tile_create(lv_obj_t *parent, const tile_spec_t *spec)
{
    tile_inst_t *t = calloc(1, sizeof(*t));
    if (t == NULL) return NULL;

    t->spec = spec;
    t->vt   = vt_for_span(spec->kind, spec->w, spec->h);
    t->last_sev = SEV_COUNT;    /* force the first style write */

    t->shell = make_card(parent);
    lv_obj_set_size(t->shell, TILE_W(spec->w), TILE_H(spec->h));
    lv_obj_set_pos(t->shell, TILE_X(spec->col), TILE_Y(spec->row));
    lv_obj_add_flag(t->shell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->shell, shell_clicked, LV_EVENT_CLICKED, t);
    /* Gestures bubble up from tiles to the page so a swipe that starts on a
     * tile still pages; only the page clears GESTURE_BUBBLE. */

    t->title_lbl = make_label(t->shell, FONT_S, COL_DIM);
    lv_label_set_long_mode(t->title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t->title_lbl, TILE_W(spec->w) - 2 * PAD_S - 16);
    lv_obj_set_pos(t->title_lbl, 0, 0);
    lv_label_set_text(t->title_lbl, spec->title ? spec->title : "");

    /* Freshness dot, top-right. Near-invisible when healthy: a wall of
     * coloured dots is noise, one red dot is a signal. */
    t->dot = make_dot(t->shell, 10, COL_DIM);
    lv_obj_set_pos(t->dot, TILE_W(spec->w) - 2 * PAD_S - 10, 2);
    lv_obj_set_style_opa(t->dot, LV_OPA_30, 0);

    t->body = lv_obj_create(t->shell);
    lv_obj_set_size(t->body, TILE_W(spec->w) - 2 * PAD_S,
                             TILE_H(spec->h) - 2 * PAD_S - 20);
    lv_obj_set_pos(t->body, 0, 20);
    lv_obj_set_style_bg_opa(t->body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t->body, 0, 0);
    lv_obj_set_style_pad_all(t->body, 0, 0);
    lv_obj_clear_flag(t->body, LV_OBJ_FLAG_SCROLLABLE);

    if (t->vt->build) t->vt->build(t, t->body);

    /*
     * The shell owns the tap and the page owns the swipe, so nothing inside
     * may claim either.
     *
     * lv_obj_create sets LV_OBJ_FLAG_CLICKABLE by default in LVGL 8, and so
     * do lv_bar and lv_chart -- so the body, which covers everything below
     * the title strip, silently swallowed every tap and did nothing with it.
     * Sweeping the subtree means a renderer cannot reintroduce this by adding
     * a widget, which is how it got here in the first place.
     */
    make_inert(t->body);

    /*
     * A child that extends past the body is silently CLIPPED -- it simply
     * does not appear, with nothing logged and nothing to see but a missing
     * caption. Renderers size themselves from the span, so a layout that fits
     * at 2x2 can overflow at 1x1; checking here catches every renderer at
     * every span instead of relying on someone redoing the arithmetic.
     */
    lv_obj_update_layout(t->body);
    lv_coord_t bh = lv_obj_get_height(t->body), bw = lv_obj_get_width(t->body);
    uint32_t n = lv_obj_get_child_cnt(t->body);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(t->body, i);
        lv_coord_t bot = lv_obj_get_y(c) + lv_obj_get_height(c);
        lv_coord_t rgt = lv_obj_get_x(c) + lv_obj_get_width(c);
        if (bot > bh || rgt > bw) {
            ESP_LOGE(TAG, "%s tile \"%s\" %ux%u: child %u extends to %d,%d "
                          "in a %dx%d body -- it will be clipped",
                     t->vt->name, spec->title ? spec->title : "?",
                     spec->w, spec->h, (unsigned)i, (int)rgt, (int)bot,
                     (int)bw, (int)bh);
        }
    }
    return t;
}

void tile_update(tile_inst_t *t, const tile_data_t *d)
{
    if (t == NULL || d == NULL) return;

    if (d->valid && isfinite(d->value)) {
        if (t->hist_n < TILE_HIST_MAX) {
            t->hist[t->hist_n++] = d->value;
        } else {
            memmove(t->hist, t->hist + 1, (TILE_HIST_MAX - 1) * sizeof(float));
            t->hist[TILE_HIST_MAX - 1] = d->value;
        }
    }

    severity_t sev = severity_of(t->spec, d);
    if (sev != t->last_sev) {
        t->last_sev = sev;
        bg_color_if_changed(t->dot, app_theme_sev(sev));
        opa_if_changed(t->dot, sev == SEV_OK ? LV_OPA_30 : LV_OPA_COVER);
        /* Only crit escalates to chrome. Colouring every tile's background by
         * state turns the whole panel into noise. */
        lv_obj_set_style_border_width(t->shell, sev == SEV_CRIT ? 2 : 0, 0);
        if (sev == SEV_CRIT) border_color_if_changed(t->shell, COL_CRIT);
    }

    if (t->vt->update) t->vt->update(t, d);
}

void tile_destroy(tile_inst_t *t)
{
    if (t == NULL) return;
    if (t->vt->destroy) t->vt->destroy(t);
    if (t->shell) lv_obj_del(t->shell);
    free(t);
}
