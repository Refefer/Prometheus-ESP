#include "ui_panelcfg.h"

#include "config.h"
#include "poller.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_tile.h"
#include "ui_widgets.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *s_root;
static lv_obj_t *s_kind_btn[TILE_KIND_COUNT];
static lv_obj_t *s_size_btn[4];
static lv_obj_t *s_multi_lbl, *s_sel_lbl, *s_title_lbl;
static uint16_t  s_id;
static void (*s_on_close)(void);

/* Offered spans. A 3x or 4x wide tile is possible but there is rarely a
 * reason, and four choices fit as buttons where six would not. */
static const struct { uint8_t w, h; const char *name; } k_sizes[4] = {
    { 1, 1, "1x1" }, { 2, 1, "2x1" }, { 1, 2, "1x2" }, { 2, 2, "2x2" },
};

static cfg_panel_t *panel(void)
{
    config_t *c = config_get();
    for (int i = 0; i < c->n_panels; i++) {
        if (c->panels[i].id == s_id) return &c->panels[i];
    }
    return NULL;
}

static void refresh(void)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;

    for (int k = 0; k < TILE_KIND_COUNT; k++) {
        if (s_kind_btn[k] == NULL) continue;
        bool on = (p->kind == (tile_kind_t)k);
        bg_color_if_changed(s_kind_btn[k], on ? COL_ACCENT : COL_PANEL);
        lv_obj_t *l = lv_obj_get_child(s_kind_btn[k], 0);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }

    for (int i = 0; i < 4; i++) {
        bool on = (p->w == k_sizes[i].w && p->h == k_sizes[i].h);
        bg_color_if_changed(s_size_btn[i], on ? COL_ACCENT : COL_PANEL);
        lv_obj_t *l = lv_obj_get_child(s_size_btn[i], 0);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }

    label_set_if_changed(s_multi_lbl, p->multi ? "all series" : "one series");
    label_set_if_changed(s_title_lbl, p->title[0] ? p->title : "(metric name)");
    label_set_if_changed(s_sel_lbl, p->sel);
}

static void kind_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    tile_kind_t k = (tile_kind_t)(intptr_t)lv_event_get_user_data(e);

    /* Growing to satisfy a renderer's minimum is better than silently falling
     * back to something else -- picking "Chart" and getting a sparkline with
     * no explanation is worse than the tile getting bigger. */
    const tile_vt_t *vt = tile_vt(k);
    uint8_t w = p->w, h = p->h;
    if (w < vt->min_w) w = vt->min_w;
    if (h < vt->min_h) h = vt->min_h;

    uint8_t ow = p->w, oh = p->h;
    p->kind = k; p->w = w; p->h = h;
    if ((w != ow || h != oh) && !config_place_panel(p)) {
        p->w = ow; p->h = oh;
        char msg[72];
        snprintf(msg, sizeof(msg), "%s needs %ux%u and there is no room",
                 vt->name, vt->min_w, vt->min_h);
        ui_toast(msg, SEV_WARN, 2500);
    }
    config_touch();
    refresh();
}

static void size_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    const tile_vt_t *vt = tile_vt(p->kind);
    if (k_sizes[i].w < vt->min_w || k_sizes[i].h < vt->min_h) {
        char msg[72];
        snprintf(msg, sizeof(msg), "%s needs at least %ux%u",
                 vt->name, vt->min_w, vt->min_h);
        ui_toast(msg, SEV_WARN, 2500);
        return;
    }

    uint8_t ow = p->w, oh = p->h;
    p->w = k_sizes[i].w; p->h = k_sizes[i].h;
    if (!config_place_panel(p)) {
        p->w = ow; p->h = oh;
        ui_toast("No room for that size", SEV_WARN, 2500);
    }
    config_touch();
    refresh();
}

static void multi_cb(lv_event_t *e)
{
    (void)e;
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    p->multi = !p->multi;
    if (p->multi && p->kind != TILE_MULTI) {
        /* Showing every series as one number would just sum them, which is
         * rarely what anyone means by "all series". */
        p->kind = TILE_MULTI;
        const tile_vt_t *vt = tile_vt(TILE_MULTI);
        if (p->w < vt->min_w) p->w = vt->min_w;
        if (p->h < vt->min_h) p->h = vt->min_h;
        config_place_panel(p);
    }
    config_touch();
    refresh();
}

static void title_done(const char *text, void *user)
{
    (void)user;
    cfg_panel_t *p = panel();
    if (p == NULL || text == NULL) return;
    strncpy(p->title, text, sizeof(p->title) - 1);
    p->title[sizeof(p->title) - 1] = '\0';
    config_touch();
    refresh();
}

static void title_cb(lv_event_t *e)
{
    (void)e;
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    ui_kbd_req_t req = {
        .title = "Tile title",
        .label = p->sel,
        .value = p->title,
        .hint  = "Leave empty to use the metric name.",
        .kind  = KB_TEXT,
        .done  = title_done,
    };
    ui_kbd_edit(&req);
}

static void close_overlay(void)
{
    if (s_root == NULL) return;
    config_flush();
    lv_obj_del(s_root);
    s_root = NULL;
    memset(s_kind_btn, 0, sizeof(s_kind_btn));
    if (s_on_close) s_on_close();
}

static void done_cb(lv_event_t *e)   { (void)e; close_overlay(); }

static void remove_cb(lv_event_t *e)
{
    (void)e;
    config_panel_remove(s_id);
    close_overlay();
}

void ui_panelcfg_open(uint16_t panel_id, void (*on_close)(void))
{
    if (s_root) return;
    s_id = panel_id;
    s_on_close = on_close;
    if (panel() == NULL) return;

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, SCR_W, SCR_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *h = make_label(s_root, FONT_L, COL_TEXT);
    lv_label_set_text(h, "Tile");
    lv_obj_set_pos(h, GRID_MX, 10);

    s_sel_lbl = make_label(s_root, FONT_XS, COL_DIM);
    lv_label_set_long_mode(s_sel_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_sel_lbl, 520);
    lv_obj_set_pos(s_sel_lbl, 90, 16);

    lv_obj_t *done = make_btn_accent(s_root, LV_SYMBOL_OK "  Done", done_cb, NULL);
    lv_obj_set_size(done, 130, 34);
    lv_obj_set_pos(done, SCR_W - 130 - GRID_MX, 6);

    /* widget type */
    lv_obj_t *kc = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(kc, "Widget");
    lv_obj_set_pos(kc, GRID_MX, 52);

    lv_coord_t x = GRID_MX, y = 74;
    for (int k = 0; k < TILE_KIND_COUNT; k++) {
        const tile_vt_t *vt = tile_vt((tile_kind_t)k);
        s_kind_btn[k] = make_btn(s_root, vt->name, kind_cb, (void *)(intptr_t)k);
        lv_obj_set_size(s_kind_btn[k], 148, 42);
        lv_obj_set_pos(s_kind_btn[k], x, y);
        x += 154;
        if (x + 148 > SCR_W - GRID_MX) { x = GRID_MX; y += 48; }
    }

    /* size */
    lv_obj_t *sc = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(sc, "Size");
    lv_obj_set_pos(sc, GRID_MX, y + 60);
    for (int i = 0; i < 4; i++) {
        s_size_btn[i] = make_btn(s_root, k_sizes[i].name, size_cb,
                                 (void *)(intptr_t)i);
        lv_obj_set_size(s_size_btn[i], 90, 42);
        lv_obj_set_pos(s_size_btn[i], GRID_MX + i * 96, y + 82);
    }

    /* series mode */
    lv_obj_t *mc = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(mc, "Series");
    lv_obj_set_pos(mc, GRID_MX + 420, y + 60);
    lv_obj_t *mbtn = make_btn(s_root, "", multi_cb, NULL);
    lv_obj_set_size(mbtn, 200, 42);
    lv_obj_set_pos(mbtn, GRID_MX + 420, y + 82);
    s_multi_lbl = lv_obj_get_child(mbtn, 0);

    /* title */
    lv_obj_t *tc = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(tc, "Title");
    lv_obj_set_pos(tc, GRID_MX, y + 140);
    lv_obj_t *tbtn = make_btn(s_root, "", title_cb, NULL);
    lv_obj_set_size(tbtn, 400, 42);
    lv_obj_set_pos(tbtn, GRID_MX, y + 162);
    s_title_lbl = lv_obj_get_child(tbtn, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_lbl, 370);

    lv_obj_t *rm = make_btn(s_root, LV_SYMBOL_TRASH "  Remove tile",
                            remove_cb, NULL);
    lv_obj_set_size(rm, 220, BTN_H);
    lv_obj_align(rm, LV_ALIGN_BOTTOM_LEFT, GRID_MX, -16);

    refresh();
}

bool ui_panelcfg_is_open(void) { return s_root != NULL; }
