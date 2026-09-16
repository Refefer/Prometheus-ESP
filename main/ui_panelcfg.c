#include "ui_panelcfg.h"

#include "config.h"
#include "poller.h"
#include "ui_browser.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_tile.h"
#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_root;
static lv_obj_t *s_kind_btn[TILE_KIND_COUNT];
static lv_obj_t *s_size_btn[4];
static lv_obj_t *s_multi_lbl, *s_sel_lbl, *s_title_lbl;
static lv_obj_t *s_op_btn[5], *s_selb_lbl;
static lv_obj_t *s_op_cap, *s_selb_cap, *s_selb_btn;
static lv_obj_t *s_q_btn[3], *s_win_btn[4], *s_q_cap, *s_win_cap;
static lv_obj_t *s_scr_lbl;

static int screens_reachable(void);
static lv_obj_t *s_map_cell[GRID_ROWS][GRID_COLS];
static uint16_t  s_id;
static void (*s_on_close)(void);

/* Offered spans. A 3x or 4x wide tile is possible but there is rarely a
 * reason, and four choices fit as buttons where six would not. */
static const struct { uint8_t w, h; const char *name; } k_sizes[4] = {
    { 1, 1, "1x1" }, { 2, 1, "2x1" }, { 1, 2, "1x2" }, { 2, 2, "2x2" },
};

/*
 * The four ways two series get combined, named for what they are FOR rather
 * than for their arithmetic: nobody thinks "a/(a+b)", they think "what share
 * of the total is this".
 */
/*
 * Prefixes worth a button. Auto plus four steps up covers everything a panel
 * on this screen shows; the sub-unit prefixes exist in the config for
 * completeness but nothing here is measured in nanos.
 */
static const int8_t k_scales_ui[5] = { FMT_PIN_AUTO, 0, 1, 2, 3 };
static lv_obj_t *s_scale_cap, *s_scale_btn[5];
static lv_obj_t *s_group_cap, *s_group_btn;

/* Quantiles worth a button. p50/p90/p99 is the usual trio; anything finer
 * needs more observations than a 5s scrape of a quiet service provides. */
static const struct { float q; const char *name; } k_quants[3] = {
    { 0.50f, "p50" }, { 0.90f, "p90" }, { 0.99f, "p99" },
};

/*
 * How much history the quantile covers. All-time is offered but is almost
 * never right: on a long-lived process it is dominated by old observations
 * and stops moving -- measured on a real server, all-time p99 read 72s while
 * the live traffic was at 0.6s.
 */
static const struct { uint16_t s; const char *name; } k_windows[4] = {
    { 60, "1 min" }, { 300, "5 min" }, { 900, "15 min" }, { 0, "all time" },
};

static const struct { panel_op_t op; const char *name; } k_ops[5] = {
    { OP_NONE,  "single"  },
    { OP_SHARE, "share %" },   /* a / (a+b) -- hit rate, error rate */
    { OP_RATIO, "ratio"   },   /* a / b     -- against a capacity */
    { OP_DIFF,  "a - b"   },
    { OP_SUM,   "a + b"   },
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

    /*
     * A histogram panel already reduces a distribution to one number, so the
     * controls it needs are WHICH number and over how long -- not how to
     * combine it with a second series. The two sets share the row rather than
     * making the sheet taller than the screen.
     */
    cfg_term_t *t0 = config_term0(p);
    bool hist = (t0->q > 0.0f);
    /* A windowed rate matters for any counter, not just histograms: an
     * exporter that updates on a log interval steps rather than flows, and
     * polled faster than it updates the raw rate alternates between zero and
     * a spike. */
    bool windowed = hist || (t0->agg == AGG_RATE);
    for (int i = 0; i < 3; i++) hidden_if_changed(s_q_btn[i], !hist);
    for (int i = 0; i < 4; i++) hidden_if_changed(s_win_btn[i], !windowed);
    hidden_if_changed(s_q_cap, !hist);
    hidden_if_changed(s_win_cap, !windowed);
    for (int i = 0; i < 5; i++) hidden_if_changed(s_op_btn[i], windowed);
    hidden_if_changed(s_op_cap, windowed);
    hidden_if_changed(s_selb_cap, windowed);
    hidden_if_changed(s_selb_btn, windowed);

    for (int i = 0; i < 3; i++) {
        bool on = (fabsf(t0->q - k_quants[i].q) < 0.001f);
        bg_color_if_changed(s_q_btn[i], on ? COL_ACCENT : COL_PANEL);
        lv_obj_t *l = lv_obj_get_child(s_q_btn[i], 0);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }
    /* Labels come from the format's own ladder, so a byte panel offers B and
     * KiB rather than 1 and k. A format with no ladder hides the row. */
    const char *probe = ui_fmt_prefix_name(p->fmt, 0);
    hidden_if_changed(s_scale_cap, probe == NULL);
    for (int i = 0; i < 5; i++) {
        hidden_if_changed(s_scale_btn[i], probe == NULL);
        if (probe == NULL) continue;
        const char *name = k_scales_ui[i] == FMT_PIN_AUTO
                         ? "Auto" : ui_fmt_prefix_name(p->fmt, k_scales_ui[i]);
        lv_obj_t *l = lv_obj_get_child(s_scale_btn[i], 0);
        if (l) label_set_if_changed(l, name ? name : "?");
        bool on = (p->scale == k_scales_ui[i]);
        bg_color_if_changed(s_scale_btn[i], on ? COL_ACCENT : COL_PANEL);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }

    bool groupable = (probe != NULL);
    hidden_if_changed(s_group_cap, !groupable);
    hidden_if_changed(s_group_btn, !groupable);
    if (groupable) {
        bg_color_if_changed(s_group_btn, p->group ? COL_ACCENT : COL_PANEL);
        lv_obj_t *gl = lv_obj_get_child(s_group_btn, 0);
        if (gl) text_color_if_changed(gl, p->group ? COL_BG : COL_TEXT);
    }

    for (int i = 0; i < 4; i++) {
        bool on = (t0->window_s == k_windows[i].s);
        bg_color_if_changed(s_win_btn[i], on ? COL_ACCENT : COL_PANEL);
        lv_obj_t *l = lv_obj_get_child(s_win_btn[i], 0);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }

    for (int i = 0; i < 5; i++) {
        bool on = (p->op == k_ops[i].op);
        bg_color_if_changed(s_op_btn[i], on ? COL_ACCENT : COL_PANEL);
        lv_obj_t *l = lv_obj_get_child(s_op_btn[i], 0);
        if (l) text_color_if_changed(l, on ? COL_BG : COL_TEXT);
    }
    bool have_b = (p->n_terms > 1 && p->terms[1].sel[0]);
    label_set_if_changed(s_selb_lbl,
                         p->op == OP_NONE ? "(not used)"
                         : have_b         ? p->terms[1].sel
                                          : "tap to choose the other series");
    text_color_if_changed(s_selb_lbl,
                          (p->op != OP_NONE && !have_b) ? COL_WARN : COL_TEXT);

    label_set_fmt_if_changed(s_scr_lbl, "%u / %d", p->screen + 1,
                             screens_reachable());

    /*
     * A miniature of the grid, so a move is visible without repainting the
     * dashboard underneath -- which cannot be seen through an opaque
     * full-screen sheet, and which draws over it if attempted.
     */
    const config_t *c = config_get();
    uint8_t pw = p->w ? p->w : 1, ph = p->h ? p->h : 1;
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int cc = 0; cc < GRID_COLS; cc++) {
            bool mine = (cc >= p->col && cc < p->col + pw &&
                         r  >= p->row && r  < p->row + ph);
            bool taken = false;
            for (int i = 0; i < c->n_panels && !taken; i++) {
                const cfg_panel_t *o = &c->panels[i];
                if (o == p || !o->sel[0] || o->screen != p->screen) continue;
                uint8_t ow = o->w ? o->w : 1, oh = o->h ? o->h : 1;
                taken = (cc >= o->col && cc < o->col + ow &&
                         r  >= o->row && r  < o->row + oh);
            }
            /* Would the tile's top-left land here? That is what the cell
             * offers, so that is what it advertises -- a cell that looks free
             * but cannot take a 2x2 would be a lie. */
            bool target = config_panel_fits(p, cc, r);

            lv_obj_t *cell = s_map_cell[r][cc];
            bg_color_if_changed(cell, mine ? COL_ACCENT
                                     : taken ? COL_PANEL_ALT : COL_BG);
            lv_obj_set_style_bg_opa(cell, mine ? LV_OPA_COVER
                                         : taken ? LV_OPA_COVER : LV_OPA_20, 0);
            border_color_if_changed(cell, mine ? COL_ACCENT
                                       : target ? COL_OK : COL_LINE);
            lv_obj_set_style_border_width(cell, (mine || target) ? 2 : 1, 0);
        }
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

static void selb_chosen(const char *sel)
{
    cfg_panel_t *p = panel();
    if (p == NULL || sel == NULL) { refresh(); return; }
    if (p->n_terms < 2) {
        /* The second operand inherits the first's aggregation, which is what
         * makes a ratio of rates rather than a rate divided by a total. */
        p->terms[1] = p->terms[0];
        p->n_terms = 2;
    }
    strncpy(p->terms[1].sel, sel, sizeof(p->terms[1].sel) - 1);
    p->terms[1].sel[sizeof(p->terms[1].sel) - 1] = '\0';
    config_touch();
    refresh();
}

static void selb_cb(lv_event_t *e)
{
    (void)e;
    cfg_panel_t *p = panel();
    if (p == NULL || p->op == OP_NONE) {
        ui_toast("Choose a comparison first", SEV_WARN, 2000);
        return;
    }
    ui_browser_open_select(selb_chosen);
}

static void q_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    config_term0(p)->q = k_quants[(int)(intptr_t)lv_event_get_user_data(e)].q;
    config_touch();
    refresh();
}

static void win_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    uint16_t w = k_windows[(int)(intptr_t)lv_event_get_user_data(e)].s;
    /* Both operands share the window: two sides of a ratio measured over
     * different spans is almost always a mistake rather than an intention. */
    for (int i = 0; i < p->n_terms; i++) p->terms[i].window_s = w;
    config_touch();
    refresh();
}

static void op_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    p->op = k_ops[i].op;

    /*
     * A share is a proportion, so default it to a percentage gauge -- that is
     * what the number means, and leaving it as raw SI would show 0.972 where
     * the reader wants 97%.
     */
    if (p->op == OP_NONE) p->n_terms = 1;
    if (p->op == OP_SHARE) {
        p->fmt  = FMT_PCT_01;
        p->vmin = 0.0f;
        p->vmax = 100.0f;
        if (p->kind == TILE_STAT) p->kind = TILE_GAUGE;
    }
    if (p->op != OP_NONE) p->multi = false;   /* the two are incompatible */
    config_touch();
    refresh();
}

/*
 * Tap a cell of the miniature to put the tile's top-left there.
 *
 * This is the answer to moving tiles that are not all the same size: arrows
 * can only step into adjacent space, so a 2x2 on a busy grid has nowhere to
 * step. Tapping a destination works whatever the span, and the map already
 * shows which destinations are available.
 */
static void map_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    int cc = (int)(packed >> 8), r = (int)(packed & 0xFF);

    if (!config_move_panel(p, cc, r)) {
        uint8_t w = p->w ? p->w : 1, h = p->h ? p->h : 1;
        char msg[88];
        snprintf(msg, sizeof(msg),
                 "A %ux%u tile does not fit there - free some cells first", w, h);
        ui_toast(msg, SEV_WARN, 2500);
        return;
    }
    refresh();
}

/*
 * Screens a tile can move to: the ones in use, plus one beyond, so a second
 * screen is made by moving something onto it. Derived from where the panels
 * actually are rather than from the stored screen list, which is the same
 * rule the dashboard pages by.
 */
static int screens_reachable(void)
{
    const config_t *c = config_get();
    int hi = 1;
    for (int i = 0; i < c->n_panels; i++) {
        if (c->panels[i].sel[0] && c->panels[i].screen + 1 > hi) {
            hi = c->panels[i].screen + 1;
        }
    }
    return hi < CFG_MAX_SCREENS ? hi + 1 : CFG_MAX_SCREENS;
}

static void screen_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int want = (int)p->screen + d;
    if (want < 0 || want >= screens_reachable()) return;

    if (!config_ensure_screen((uint8_t)want)) return;

    uint8_t was_screen = p->screen, was_col = p->col, was_row = p->row;
    p->screen = (uint8_t)want;
    /* Keep the same cell when it is free over there, since that is the least
     * surprising outcome; otherwise take the first slot that fits. */
    if (!config_panel_fits(p, p->col, p->row) && !config_place_panel(p)) {
        p->screen = was_screen;
        p->col = was_col;
        p->row = was_row;
        ui_toast("No room on that screen", SEV_WARN, 2200);
        return;
    }
    config_touch();
    refresh();
}

static void group_cb(lv_event_t *e)
{
    (void)e;
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    p->group = !p->group;
    config_touch();
    refresh();
}

static void scale_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    p->scale = k_scales_ui[(int)(intptr_t)lv_event_get_user_data(e)];
    config_touch();
    refresh();
}

static void move_cb(lv_event_t *e)
{
    cfg_panel_t *p = panel();
    if (p == NULL) return;
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int dc = (d == 0) ? -1 : (d == 1) ? 1 : 0;
    int dr = (d == 2) ? -1 : (d == 3) ? 1 : 0;

    if (!config_nudge_panel(p, dc, dr)) {
        ui_toast("Blocked - the neighbour is a different size", SEV_WARN, 2000);
        return;
    }
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
    s_on_close  = on_close;
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
    lv_obj_set_pos(mc, GRID_MX + 400, y + 60);
    lv_obj_t *mbtn = make_btn(s_root, "", multi_cb, NULL);
    lv_obj_set_size(mbtn, 170, 42);
    lv_obj_set_pos(mbtn, GRID_MX + 400, y + 82);
    s_multi_lbl = lv_obj_get_child(mbtn, 0);

    /* title */
    lv_obj_t *tc = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(tc, "Title");
    lv_obj_set_pos(tc, GRID_MX + 580, y + 60);
    lv_obj_t *tbtn = make_btn(s_root, "", title_cb, NULL);
    lv_obj_set_size(tbtn, 190, 42);
    lv_obj_set_pos(tbtn, GRID_MX + 580, y + 82);
    s_title_lbl = lv_obj_get_child(tbtn, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_lbl, 160);

    /* combine with a second series */
    /* Histogram controls and Combine share this row; refresh() shows one. */
    s_q_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_q_cap, "Quantile");
    lv_obj_set_pos(s_q_cap, GRID_MX, y + 140);
    for (int i = 0; i < 3; i++) {
        s_q_btn[i] = make_btn(s_root, k_quants[i].name, q_cb, (void *)(intptr_t)i);
        lv_obj_set_size(s_q_btn[i], 92, 42);
        lv_obj_set_pos(s_q_btn[i], GRID_MX + i * 98, y + 162);
    }

    s_win_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_win_cap, "Over the last");
    lv_obj_set_pos(s_win_cap, GRID_MX + 310, y + 140);
    for (int i = 0; i < 4; i++) {
        s_win_btn[i] = make_btn(s_root, k_windows[i].name, win_cb,
                                (void *)(intptr_t)i);
        lv_obj_set_size(s_win_btn[i], 110, 42);
        lv_obj_set_pos(s_win_btn[i], GRID_MX + 310 + i * 116, y + 162);
    }

    s_op_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_op_cap, "Combine");
    lv_obj_set_pos(s_op_cap, GRID_MX, y + 140);
    for (int i = 0; i < 5; i++) {
        s_op_btn[i] = make_btn(s_root, k_ops[i].name, op_cb, (void *)(intptr_t)i);
        lv_obj_set_size(s_op_btn[i], 92, 42);
        lv_obj_set_pos(s_op_btn[i], GRID_MX + i * 98, y + 162);
    }

    s_selb_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_selb_cap, "with");
    lv_obj_set_pos(s_selb_cap, GRID_MX + 500, y + 140);
    s_selb_btn = make_btn(s_root, "", selb_cb, NULL);
    lv_obj_set_size(s_selb_btn, 270, 42);
    lv_obj_set_pos(s_selb_btn, GRID_MX + 500, y + 162);
    s_selb_lbl = lv_obj_get_child(s_selb_btn, 0);
    lv_label_set_long_mode(s_selb_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_selb_lbl, 240);

    /* Position. Arrows rather than drag-and-drop: a corner drag on a 185px
     * tile with a fingertip is a coin flip, and an arrow is unambiguous. */
    lv_obj_t *poscap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(poscap, "Position");
    lv_obj_align(poscap, LV_ALIGN_BOTTOM_LEFT, GRID_MX, -62);

    static const char *const arrows[4] = {
        LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_UP, LV_SYMBOL_DOWN
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = make_btn(s_root, arrows[i], move_cb, (void *)(intptr_t)i);
        lv_obj_set_size(b, 72, BTN_H);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, GRID_MX + i * 78, -12);
    }

    lv_obj_t *maphint = make_label(s_root, FONT_XS, COL_DIM);
    lv_label_set_text(maphint, "tap a cell to place it");
    lv_obj_align(maphint, LV_ALIGN_BOTTOM_LEFT, GRID_MX + 336, -82);

    /* 4x3 miniature, 22x16 cells. Small enough to sit beside the arrows,
     * large enough that a 1x1 in a corner is unmistakable. */
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int cc = 0; cc < GRID_COLS; cc++) {
            lv_obj_t *cell = lv_obj_create(s_root);
            /* 30x20 rather than 22x16: these are touch targets now, and a
             * 22px cell is under half the comfortable minimum. */
            lv_obj_set_size(cell, 30, 20);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, map_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(((uint32_t)cc << 8) | r));
            lv_obj_align(cell, LV_ALIGN_BOTTOM_LEFT,
                         GRID_MX + 336 + cc * 33, -10 - (GRID_ROWS - 1 - r) * 23);
            s_map_cell[r][cc] = cell;
        }
    }

    /* Units. Auto-scaling keeps three significant digits but changes the
     * unit as the value moves, which costs a glance to read; pinning trades
     * digits for a number whose scale never shifts. */
    s_scale_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_scale_cap, "Units");
    lv_obj_set_pos(s_scale_cap, GRID_MX, 336);
    for (int i = 0; i < 5; i++) {
        s_scale_btn[i] = make_btn(s_root, "", scale_cb, (void *)(intptr_t)i);
        lv_obj_set_size(s_scale_btn[i], 60, 38);
        lv_obj_set_pos(s_scale_btn[i], GRID_MX + i * 64, 356);
    }

    /* Thousands separators. Sits beside Units because both shape the number
     * without changing it, and the comma is in the digits-only face. */
    s_group_cap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(s_group_cap, "Thousands");
    lv_obj_set_pos(s_group_cap, GRID_MX + 490, 336);

    s_group_btn = make_btn(s_root, "1,000", group_cb, NULL);
    lv_obj_set_size(s_group_btn, 110, 38);
    lv_obj_set_pos(s_group_btn, GRID_MX + 490, 356);

    /* Which screen the tile lives on. Without this a tile placed on a second
     * screen could only be deleted, never brought back. */
    /*
     * Up on the Units row rather than the bottom one, where it ran under the
     * Remove button: Remove is anchored to the right edge and 180 wide, so
     * the bottom row has nothing usable past GRID_MX+590.
     */
    lv_obj_t *scap = make_label(s_root, FONT_S, COL_DIM);
    lv_label_set_text(scap, "Screen");
    lv_obj_set_pos(scap, GRID_MX + 615, 336);

    lv_obj_t *sprev = make_btn(s_root, LV_SYMBOL_LEFT, screen_cb,
                               (void *)(intptr_t)-1);
    lv_obj_set_size(sprev, 40, 38);
    lv_obj_set_pos(sprev, GRID_MX + 615, 356);

    /* Fixed width and centred, so the gap to the buttons does not depend on
     * how wide "1 / 3" happens to render. */
    s_scr_lbl = make_label(s_root, FONT_M, COL_TEXT);
    lv_obj_set_width(s_scr_lbl, 52);
    lv_obj_set_style_text_align(s_scr_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_scr_lbl, GRID_MX + 659, 366);

    lv_obj_t *snext = make_btn(s_root, LV_SYMBOL_RIGHT, screen_cb,
                               (void *)(intptr_t)1);
    lv_obj_set_size(snext, 40, 38);
    lv_obj_set_pos(snext, GRID_MX + 715, 356);

    lv_obj_t *rm = make_btn(s_root, LV_SYMBOL_TRASH "  Remove", remove_cb, NULL);
    lv_obj_set_size(rm, 180, BTN_H);
    lv_obj_align(rm, LV_ALIGN_BOTTOM_RIGHT, -GRID_MX, -12);

    refresh();
    ui_check_overlaps(s_root, "panel settings");
}

bool ui_panelcfg_is_open(void) { return s_root != NULL; }
