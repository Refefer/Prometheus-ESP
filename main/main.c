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
#include "esp_timer.h"
#include <math.h>
#include "lvgl.h"

#include "lvgl_port.h"
#include "config.h"
#include "poller.h"
#include "prom_text.h"
#include "secrets.h"
#include "storage.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_browser.h"
#include "ui_panelcfg.h"
#include "ui_layouts.h"
#include "timekeep.h"
#include "ui_endpoints.h"
#include "ui_setup.h"
#include "webcfg.h"
#include "ui_theme.h"
#include "ui_tile.h"
#include "ui_widgets.h"
#include "waveshare_rgb_lcd_port.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

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


static void rebuild_dashboard(void);   /* defined with the dashboard below */
static void dashboard_tick(lv_timer_t *timer);
static void set_hdr_title(void);
static void browser_closed(void);      /* rebuilds tiles after any modal */
static void hole_tapped(lv_event_t *e);

static void endpoints_cb(lv_event_t *e)
{
    (void)e;
    ui_endpoints_open(rebuild_dashboard);
}

/*
 * A real tile grid, laid out with the production geometry from ui_layout.h so
 * this milestone validates the grid arithmetic on the actual panel as well as
 * the data path. The watch list behind it is still fixed -- the metric store
 * and the browser replace that; the layout and the refresh path stay.
 */
/*
 * The dashboard.
 *
 * Tiles come from the stored panels, and the poller's watch slots are in the
 * same order, so slot i is panel i. Nothing about which metrics appear is
 * compiled in any more.
 */
static tile_inst_t *s_tiles[CFG_MAX_PANELS];
static tile_spec_t  s_specs[CFG_MAX_PANELS];
static int          s_tile_n;
/* One placeholder per free cell: an empty tile is a place to put something,
 * not an absence, and tapping where you want it beats picking from a list and
 * finding out afterwards where it landed. */
static lv_obj_t    *s_holes[GRID_COLS * GRID_ROWS];
static int          s_hole_n;
static lv_obj_t    *s_hdr_title;
static lv_obj_t    *s_hdr_sig;     /* wifi strength bars */
static lv_obj_t    *s_hdr_time;
static lv_obj_t    *s_hdr_date;
static lv_obj_t    *s_fbar;
static lv_obj_t    *s_ftr_left;
static lv_obj_t    *s_ftr_right;
static lv_obj_t    *s_empty;
static uint32_t     s_seen_gen = UINT32_MAX;

/*
 * The screen on display, and the dots that say which one it is.
 *
 * Screens are not a list you maintain -- they are wherever panels are. A
 * screen exists because something is on it, and the page after the last one
 * always exists while there is panel budget left, so a new screen is made by
 * swiping to it and tapping a cell rather than by finding an Add button.
 */
static uint8_t      s_screen;
static lv_obj_t    *s_dots[CFG_MAX_SCREENS];
static int          s_dot_n;

/*
 * True while a full-screen modal owns the display.
 *
 * Rebuilding tiles underneath one is worse than useless: lv_obj_create
 * appends to the parent's child list, so freshly built tiles draw ON TOP of
 * the overlay that is supposed to be covering them. Nothing is visible of the
 * rebuild anyway, since the overlay is opaque.
 */
static bool modal_open(void)
{
    return ui_panelcfg_is_open() || ui_browser_is_open() ||
           ui_endpoints_is_open() || ui_setup_is_open() ||
           ui_layouts_is_open();
}

/*
 * RSSI to a four-bar scale.
 *
 * The thresholds are the ones that matter in practice rather than a linear
 * split: above -55 is as good as it gets, below -80 is where a scrape starts
 * timing out, and the two in between are the useful middle.
 */
static int wifi_level(int8_t rssi)
{
    if (!wifi_mgr_is_connected()) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

/* One past the highest screen anything sits on. Always at least 1. */
static int screens_used(void)
{
    const config_t *c = config_get();
    int hi = 0;
    for (int i = 0; i < c->n_panels; i++) {
        if (!c->panels[i].sel[0]) continue;
        if (c->panels[i].screen >= hi) hi = c->panels[i].screen + 1;
    }
    return hi > 0 ? hi : 1;
}

static int panels_live(void)
{
    const config_t *c = config_get();
    int n = 0;
    for (int i = 0; i < c->n_panels; i++) if (c->panels[i].sel[0]) n++;
    return n;
}

/*
 * Pages you can swipe to: the screens in use, plus one empty page to grow
 * into while both the panel budget and the screen limit allow it.
 */
static int screens_navigable(void)
{
    int used = screens_used();
    bool room = panels_live() < CFG_MAX_PANELS && used < CFG_MAX_SCREENS;
    return room ? used + 1 : used;
}

/*
 * Chart history, kept across a rebuild.
 *
 * A tile owns its history, so destroying one throws it away -- and a theme
 * change has to destroy every tile, because colours are read at build time.
 * Without this, picking a palette blanks every chart on the device and they
 * refill over the next ten minutes, which is the same mistake as resetting a
 * counter baseline because some unrelated panel was edited.
 *
 * Keyed by panel id AND by the data fingerprint, so history follows a tile
 * that merely changed how it looks -- including a change of widget kind,
 * which the adopt path cannot handle -- and is dropped when the tile now
 * shows a different series.
 */
typedef struct {
    uint16_t panel_id;
    uint32_t data_fp;
    uint16_t n;
    bool     used;
    float    v[TILE_HIST_MAX];
} hist_keep_t;
static hist_keep_t s_keep[CFG_MAX_PANELS];

static void hist_stash(const tile_inst_t *t, const tile_spec_t *sp)
{
    if (t == NULL || t->hist_n == 0) return;
    hist_keep_t *slot = NULL;
    for (int i = 0; i < CFG_MAX_PANELS; i++) {
        if (s_keep[i].used && s_keep[i].panel_id == sp->panel_id) { slot = &s_keep[i]; break; }
        if (!s_keep[i].used && slot == NULL) slot = &s_keep[i];
    }
    if (slot == NULL) slot = &s_keep[0];      /* full: the oldest loses */
    slot->used     = true;
    slot->panel_id = sp->panel_id;
    slot->data_fp  = sp->data_fp;
    slot->n        = t->hist_n;
    memcpy(slot->v, t->hist, sizeof(float) * t->hist_n);
}

static int s_hist_kept;   /* restored on the last rebuild, for the log line */

static void hist_apply(tile_inst_t *t, const tile_spec_t *sp)
{
    if (t == NULL) return;
    for (int i = 0; i < CFG_MAX_PANELS; i++) {
        if (!s_keep[i].used) continue;
        if (s_keep[i].panel_id != sp->panel_id) continue;
        if (s_keep[i].data_fp != sp->data_fp) return;   /* different series now */
        t->hist_n = s_keep[i].n;
        memcpy(t->hist, s_keep[i].v, sizeof(float) * s_keep[i].n);
        s_hist_kept++;
        return;
    }
}

/*
 * Page dots.
 *
 * Rebuilt rather than restyled because the count changes: filling the last
 * empty screen grows the strip by one, and emptying a screen shrinks it.
 * There are at most six.
 */
static void build_dots(void)
{
    for (int i = 0; i < s_dot_n; i++) if (s_dots[i]) lv_obj_del(s_dots[i]);
    s_dot_n = 0;

    int n = screens_navigable();
    if (n < 2 || s_fbar == NULL) return;   /* one page needs no indicator */

    /* Parented to the footer strip, not the screen: lv_obj_create appends,
     * so dots made after a rebuild would otherwise be drawn over by the
     * strip -- or under it, depending on which ran last. */
    const lv_coord_t pitch = 16, d = 8;
    lv_coord_t x0 = SCR_W / 2 - (n * pitch) / 2;
    for (int i = 0; i < n && i < CFG_MAX_SCREENS; i++) {
        bool here = (i == s_screen);
        lv_coord_t sz = here ? d + 2 : d;
        lv_obj_t *o = make_dot(s_fbar, sz, here ? COL_ACCENT : COL_LINE);
        lv_obj_set_pos(o, x0 + i * pitch, (FOOTER_H - sz) / 2);
        s_dots[s_dot_n++] = o;
    }
}

static void build_tiles(lv_obj_t *scr)
{
    if (modal_open()) {
        /* Deferred: every modal rebuilds on close, so nothing is lost. */
        return;
    }

    /* Screens come and go with their panels, so the one on display can stop
     * existing while you are looking at it -- emptying it, or activating a
     * layout with fewer screens. */
    int nav = screens_navigable();
    if ((int)s_screen >= nav) s_screen = (uint8_t)(nav - 1);

    /*
     * Tiles are adopted, not rebuilt, wherever they can be.
     *
     * Saving anything rewrites every panel, and destroying every tile to
     * rebuild it throws away the chart history each one has accumulated --
     * so moving one tile used to blank the other eleven. A tile survives when
     * it is still the same widget at the same size showing the same series;
     * its title and position are allowed to change, because neither affects
     * what the numbers mean. This is the display half of the same rule the
     * poller applies to its baselines.
     */
    tile_inst_t *keep[CFG_MAX_PANELS];
    tile_spec_t  had[CFG_MAX_PANELS];
    int          had_n = s_tile_n;
    memcpy(keep, s_tiles, sizeof(keep));
    memcpy(had,  s_specs, sizeof(had));

    /* The specs we want, built before anything is torn down so the old and
     * new lists can be compared. */
    tile_spec_t want[CFG_MAX_PANELS];
    int adopt[CFG_MAX_PANELS];
    int want_n = 0;

    const config_t *c = config_get();
    for (int i = 0; i < c->n_panels && want_n < CFG_MAX_PANELS; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (p->sel[0] == '\0') continue;

        tile_spec_t *sp = &want[want_n];
        memset(sp, 0, sizeof(*sp));
        sp->panel_id = p->id;
        sp->title = p->title[0] ? p->title : p->sel;
        sp->kind  = p->kind;
        sp->screen = p->screen;
        sp->col   = p->col;  sp->row = p->row;
        sp->w     = p->w ? p->w : 1;
        sp->h     = p->h ? p->h : 1;
        sp->vmin  = p->vmin; sp->vmax = p->vmax;
        sp->warn  = p->warn; sp->crit = p->crit;
        sp->lower_is_worse = p->lower_is_worse;
        sp->ramp  = p->ramp;
        sp->data_fp = config_panel_fingerprint(p);

        adopt[want_n] = -1;
        for (int j = 0; j < had_n; j++) {
            if (keep[j] == NULL) continue;
            if (had[j].panel_id != sp->panel_id) continue;
            if (had[j].kind != sp->kind || had[j].w != sp->w ||
                had[j].h != sp->h || had[j].data_fp != sp->data_fp) continue;
            adopt[want_n] = j;
            break;
        }
        want_n++;
    }

    /* Whatever nothing claimed is genuinely gone. Done before the spec array
     * is rewritten, since a tile holds a pointer into it. */
    for (int j = 0; j < had_n; j++) {
        bool claimed = false;
        for (int k = 0; k < want_n; k++) if (adopt[k] == j) { claimed = true; break; }
        if (!claimed && keep[j]) {
            hist_stash(keep[j], &had[j]);
            tile_destroy(keep[j]);
            keep[j] = NULL;
        }
    }

    memcpy(s_specs, want, sizeof(want));
    s_tile_n = want_n;
    for (int k = 0; k < want_n; k++) {
        if (adopt[k] >= 0) {
            s_tiles[k] = keep[adopt[k]];
            tile_adopt(s_tiles[k], &s_specs[k]);
        } else {
            s_tiles[k] = tile_create(scr, &s_specs[k]);
            hist_apply(s_tiles[k], &s_specs[k]);
        }
        tile_set_visible(s_tiles[k], s_specs[k].screen == s_screen);
    }

    int on_screen = 0;
    for (int k = 0; k < want_n; k++) if (s_specs[k].screen == s_screen) on_screen++;

    /* Placeholders for every cell nothing covers. */
    for (int i = 0; i < s_hole_n; i++) {
        if (s_holes[i]) lv_obj_del(s_holes[i]);
        s_holes[i] = NULL;
    }
    s_hole_n = 0;

    bool used[GRID_ROWS][GRID_COLS];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < c->n_panels; i++) {
        const cfg_panel_t *p = &c->panels[i];
        if (!p->sel[0] || p->screen != s_screen) continue;
        uint8_t pw = p->w ? p->w : 1, ph = p->h ? p->h : 1;
        for (int r = p->row; r < p->row + ph && r < GRID_ROWS; r++) {
            for (int cc = p->col; cc < p->col + pw && cc < GRID_COLS; cc++) {
                used[r][cc] = true;
            }
        }
    }

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int cc = 0; cc < GRID_COLS; cc++) {
            if (used[r][cc]) continue;
            lv_obj_t *o = lv_obj_create(scr);
            lv_obj_set_size(o, TILE_W(1), TILE_H(1));
            lv_obj_set_pos(o, TILE_X(cc), TILE_Y(r));
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(o, COL_LINE, 0);
            lv_obj_set_style_border_width(o, 2, 0);
            lv_obj_set_style_border_opa(o, LV_OPA_60, 0);
            lv_obj_set_style_radius(o, RADIUS_TILE, 0);
            lv_obj_set_style_pad_all(o, 0, 0);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(o, COL_PANEL, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);

            lv_obj_t *plus = make_label(o, FONT_XL, COL_LINE);
            lv_label_set_text(plus, "+");
            lv_obj_center(plus);
            lv_obj_clear_flag(plus, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_add_event_cb(o, hole_tapped, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(((uint32_t)cc << 8) | r));
            if (s_hole_n < (int)(sizeof(s_holes) / sizeof(s_holes[0]))) {
                s_holes[s_hole_n++] = o;
            }
        }
    }

    /* With outlines showing, an empty screen no longer reads as a fault, so
     * the hint only needs to explain the gesture once. */
    hidden_if_changed(s_empty, on_screen > 0);
    if (on_screen == 0) {
        label_set_if_changed(s_empty, s_screen > 0
            ? "New screen -- tap a  +  to put something here"
            : "Tap a  +  to choose what goes there");
    }
    build_dots();
}

/* A tap on a tile opens its settings; closing them rebuilds, since the widget
 * type or span may have changed. */
static void tile_tapped(uint16_t panel_id)
{
    ui_panelcfg_open(panel_id, browser_closed);
}

/* A metric was chosen for an empty cell: go straight to the widget picker, so
 * the flow is tap the hole, tap the metric, choose how it looks. */
static void hole_filled(uint16_t panel_id)
{
    if (panel_id == 0) { browser_closed(); return; }
    ui_panelcfg_open(panel_id, browser_closed);
}

static void hole_tapped(lv_event_t *e)
{
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    ui_browser_open_pick(s_screen, (uint8_t)(packed >> 8),
                         (uint8_t)(packed & 0xFF), hole_filled);
}

static void browse_cb(lv_event_t *e)
{
    (void)e;
    ui_browser_open(s_screen, browser_closed);
}

static void layouts_closed(void)
{
    /* Activating a layout replaces every panel, so both sides rebuild. The
     * order no longer matters -- tiles find their numbers by panel id. */
    poller_reload();
    s_screen = 0;
    build_tiles(lv_scr_act());
    set_hdr_title();
    s_seen_gen = UINT32_MAX;
}

/*
 * Swipe left for the next screen, right for the previous.
 *
 * LVGL delivers a gesture to the first ancestor of the touched object without
 * GESTURE_BUBBLE, and every modal root clears that flag, so a swipe inside a
 * sheet stops there and only the dashboard pages.
 */
static void go_to_screen(int idx)
{
    int n = screens_navigable();
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    if (idx == (int)s_screen) return;
    s_screen = (uint8_t)idx;
    ESP_LOGI(TAG, "screen %d of %d", idx + 1, n);
    build_tiles(lv_scr_act());
    s_seen_gen = UINT32_MAX;        /* repaint from the next snapshot */
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    if (modal_open() || ui_kbd_is_open()) return;

    lv_indev_t *indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;

    /*
     * Without this the release at the end of the swipe also fires CLICKED on
     * whatever the finger started over, so paging across a tile would open
     * that tile's settings every time.
     */
    lv_indev_wait_release(indev);
    go_to_screen((int)s_screen + (dir == LV_DIR_LEFT ? 1 : -1));
}

static void layouts_cb(lv_event_t *e)
{
    (void)e;
    ui_layouts_open(layouts_closed);
}

/*
 * Endpoint on the left, layout after it.
 *
 * Which dashboard is on screen is not otherwise visible anywhere -- two
 * layouts over the same endpoint look like two different devices until you
 * notice the tiles differ.
 */
static void set_hdr_title(void)
{
    const config_t *c = config_get();
    const char *ep = (c->n_endpoints && c->endpoints[0].name[0])
                     ? c->endpoints[0].name : "Prometheus Panel";
    const char *ly = config_active_layout();
    if (ly[0]) label_set_fmt_if_changed(s_hdr_title, "%s  \u2022  %s", ep, ly);
    else       label_set_if_changed(s_hdr_title, ep);
}

static void build_dashboard(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);

    /*
     * The screen does not scroll. Two symptoms came from leaving it able to.
     *
     * A screen is scrollable by default, and the footer strip and the header
     * divider are the full 800px, so with any padding at all the content
     * overflows and LVGL has something to scroll. It then draws a horizontal
     * scrollbar along the bottom -- last, over the footer strip and through
     * the "updated ... ago" text -- which is the line that looked like a
     * swipe affordance, and it was never the divider that got replaced.
     *
     * Worse, a scrollable ancestor claims the drag: indev_gesture() returns
     * immediately when scroll_obj is set (lv_indev.c:1121), so the swipe
     * between screens was being swallowed before a direction was ever
     * computed. One flag, both bugs.
     */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /*
     * Signal first, hard left. It is the one thing that explains everything
     * else being wrong, so it reads before the title rather than after it.
     */
    s_hdr_sig = make_signal(scr);
    lv_obj_set_pos(s_hdr_sig, GRID_MX, 11);

    s_hdr_title = make_label(scr, FONT_L, COL_TEXT);
    lv_obj_set_pos(s_hdr_title, GRID_MX + 37, 8);
    /* Bounded and elided rather than left to grow: an endpoint and a layout
     * name concatenated would otherwise run into the clock. */
    lv_obj_set_width(s_hdr_title, 268);
    lv_label_set_long_mode(s_hdr_title, LV_LABEL_LONG_DOT);
    set_hdr_title();

    /*
     * The clock, centred on the screen rather than on whatever width the
     * time happens to render at: a digit changing every minute must not
     * shuffle the block sideways.
     */
    const lv_coord_t clock_w = 140;
    s_hdr_time = make_label(scr, FONT_L, COL_TEXT);
    lv_obj_set_width(s_hdr_time, clock_w);
    lv_obj_set_style_text_align(s_hdr_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hdr_time, SCR_W / 2 - clock_w / 2, 1);

    s_hdr_date = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_width(s_hdr_date, clock_w);
    lv_obj_set_style_text_align(s_hdr_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_hdr_date, SCR_W / 2 - clock_w / 2, 23);

    /* Wi-Fi setup lives in the gear sheet now; it is a thing you do once. */
    lv_obj_t *lay = make_btn(scr, LV_SYMBOL_COPY, layouts_cb, NULL);
    lv_obj_set_size(lay, 52, 30);
    lv_obj_set_pos(lay, SCR_W - 168 - GRID_MX, 4);

    lv_obj_t *list = make_btn(scr, LV_SYMBOL_LIST, browse_cb, NULL);
    lv_obj_set_size(list, 52, 30);
    lv_obj_set_pos(list, SCR_W - 110 - GRID_MX, 4);

    lv_obj_t *gear = make_btn(scr, LV_SYMBOL_SETTINGS, endpoints_cb, NULL);
    lv_obj_set_size(gear, 52, 30);
    lv_obj_set_pos(gear, SCR_W - 52 - GRID_MX, 4);

    lv_obj_t *div = make_divider(scr, SCR_W);
    lv_obj_set_pos(div, 0, HEADER_H - 1);

    s_empty = make_label(scr, FONT_L, COL_DIM);
    lv_label_set_text(s_empty, "Tap a  +  to choose what goes there");
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H - 4);

    /*
     * A filled strip rather than a rule.
     *
     * A 1px line 7px above a 12px label, with 4px of slack below it, reads at
     * a glance as striking through the text -- and a horizontal line at the
     * bottom of a screen looks like it is advertising a swipe. Tinting the
     * strip separates the zone without drawing anything that could be
     * mistaken for a control.
     */
    s_fbar = make_panel(scr);
    lv_obj_set_size(s_fbar, SCR_W, FOOTER_H);
    lv_obj_set_pos(s_fbar, 0, FOOTER_Y);
    lv_obj_set_style_bg_color(s_fbar, COL_PANEL, 0);

    /* Centred in the strip: (26 - 15) / 2 leaves equal air above and below. */
    const lv_coord_t ftext_y = FOOTER_Y + (FOOTER_H - 15) / 2;

    s_ftr_left = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_left, GRID_MX, ftext_y);

    /* Right-aligned to the margin, so the last figure is the one that is
     * always whole. The page dots are centred, so this has to start past
     * where six screens' worth of them would end. */
    const lv_coord_t fr_w = 200;
    s_ftr_right = make_label(scr, FONT_XS, COL_DIM);
    lv_obj_set_pos(s_ftr_right, SCR_W - GRID_MX - fr_w, ftext_y);
    lv_obj_set_width(s_ftr_right, fr_w);
    lv_label_set_long_mode(s_ftr_right, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_ftr_right, LV_TEXT_ALIGN_RIGHT, 0);

    /* The screen object outlives a rebuild -- ui_restyle cleans its children
     * and calls back in here -- and lv_obj_clean does not touch the screen's
     * own callbacks. Without this, every theme change stacked another copy
     * of the handler, and one swipe paged once per copy. */
    while (lv_obj_remove_event_cb(scr, gesture_cb)) { }
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    tile_set_tap_handler(tile_tapped);
    build_tiles(scr);

}

/*
 * Tear the tree down and build it again, which is how a theme is applied.
 *
 * Every colour is read at build time from app_theme(), so there is no
 * restyle hook on any renderer to keep in step -- a new palette works
 * everywhere by construction. ~20ms, and it only happens when someone picks
 * a different one.
 */
void ui_restyle(void)
{
    if (modal_open()) return;          /* a sheet owns the screen; it rebuilds on close */

    /* It ends by repainting through dashboard_tick, which can itself decide a
     * restyle is due; one guard is cheaper than reasoning about that. */
    static bool busy;
    if (busy) return;
    busy = true;

    lv_obj_t *scr = lv_scr_act();
    for (int i = 0; i < s_tile_n; i++) {
        if (s_tiles[i]) {
            hist_stash(s_tiles[i], &s_specs[i]);
            tile_destroy(s_tiles[i]);
            s_tiles[i] = NULL;
        }
    }
    s_tile_n = 0;
    s_hole_n = 0;
    s_dot_n  = 0;
    memset(s_holes, 0, sizeof(s_holes));
    memset(s_dots,  0, sizeof(s_dots));

    lv_obj_clean(scr);
    s_fbar = s_hdr_title = NULL;
    s_hdr_sig = s_hdr_time = s_hdr_date = NULL;
    s_ftr_left = s_ftr_right = s_empty = NULL;

    s_hist_kept = 0;
    build_dashboard();
    ESP_LOGI(TAG, "theme %s applied: %d tiles, %d kept their history",
             app_theme_name(app_theme_id()), s_tile_n, s_hist_kept);

    /*
     * Repaint from the snapshot already in hand rather than waiting for the
     * next scrape: a palette change should not leave every tile reading "--"
     * for a poll interval.
     */
    s_seen_gen = UINT32_MAX;
    dashboard_tick(NULL);
    busy = false;
}

/*
 * Adopt the configured palette, if it is not the one already showing.
 *
 * One definition with one order: the modal check comes BEFORE the theme is
 * set, because setting it first and then finding the screen busy would leave
 * the id updated and the tree unrebuilt -- and the "has it changed" test
 * would then answer no forever, so the new palette would never be drawn.
 * A sheet rebuilds on close, which is where this gets called again.
 */
static bool apply_theme_if_changed(void)
{
    theme_id_t want = app_theme_from_name(config_get()->device.theme);
    if (want == app_theme_id()) return false;
    if (modal_open()) return false;
    app_theme_set(want);
    ui_restyle();
    return true;
}

static void rebuild_dashboard(void)
{
    if (apply_theme_if_changed()) return;
    set_hdr_title();
}

static void browser_closed(void)
{
    /* Selections changed: ask for a fresh watch list and redraw. */
    poller_reload();
    build_tiles(lv_scr_act());
    s_seen_gen = UINT32_MAX;     /* force a repaint from the next snapshot */
}

/*
 * Runs in the LVGL task. Compares the poller's generation with != rather than
 * > so a uint32 wrap is a non-event, and still repaints the age readout when
 * no new data arrived, because "updated 40s ago" has to keep counting.
 */
static void dashboard_tick(lv_timer_t *timer)
{
    (void)timer;

    /*
     * Liveness beat from the LVGL task itself. The poller logs from core 0,
     * so a wedged UI task looks identical over serial to a healthy one --
     * data flowing, screen frozen. This distinguishes the two.
     */
    /* Same pattern as the poller: a pushed config is noticed here rather than
     * calling into LVGL from the HTTP task. */
    static uint32_t seen_cfg = 0;
    uint32_t cfg_gen = config_generation();
    if (cfg_gen != seen_cfg) {
        seen_cfg = cfg_gen;

        /* A pushed config can change the palette too, and that is a rebuild
         * rather than a repaint. */
        if (apply_theme_if_changed()) return;
        timekeep_set_tz(config_get()->device.tz);

        build_tiles(lv_scr_act());
        s_seen_gen = UINT32_MAX;
        set_hdr_title();
        const config_t *c = config_get();
        if (c->n_endpoints) {
            poller_set_endpoint(c->endpoints[0].url,
                                c->endpoints[0].poll_s ? c->endpoints[0].poll_s
                                                       : c->device.poll_default_s);
        }
    }

    /* Started here rather than at boot: DNS cannot resolve a pool name before
     * the link is up, and a failed first attempt would not be retried until
     * the next update interval. Idempotent. */
    if (wifi_mgr_is_connected()) timekeep_start();

    char tbuf[12], dbuf[20];
    timekeep_now(tbuf, sizeof(tbuf), dbuf, sizeof(dbuf));
    label_set_if_changed(s_hdr_time, tbuf);
    label_set_if_changed(s_hdr_date, dbuf);

    static int beat;
    if (beat++ % 20 == 0) {
        /* Tiles exist for every screen; only one screen's are shown. Both
         * numbers matter -- "8 of 10" says paging is working, "10 of 10"
         * after a swipe would say it is not. */
        int vis = 0;
        for (int i = 0; i < s_tile_n; i++) if (s_specs[i].screen == s_screen) vis++;
        ESP_LOGI(TAG, "ui alive: tiles=%d/%d seen_gen=%u stack_hw=%u",
                 vis, s_tile_n, (unsigned)s_seen_gen,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    /*
     * Static, not a stack local, and taken ONCE.
     *
     * poller_snap_t carries every watch slot -- ~2.5KB at 24 panels -- and
     * this used to declare two of them on a 6KB LVGL task stack, which
     * overflowed the moment the watch limit was raised from 8. It only ever
     * runs on the LVGL task, so a static is safe and free.
     */
    static poller_snap_t snap;
    poller_snapshot(&snap);

    if (snap.generation != s_seen_gen) {
        s_seen_gen = snap.generation;
        /*
         * Matched by panel id, not by position.
         *
         * The poller watches every screen's panels while the tiles cover only
         * the screen on display, so the two lists have different lengths and
         * different orders. Pairing them by index -- which this used to do --
         * would put one metric's numbers under another metric's title.
         */
        for (int t = 0; t < s_tile_n; t++) {
            const poller_metric_t *m = NULL;
            for (int i = 0; i < snap.n; i++) {
                if (snap.m[i].panel_id == s_specs[t].panel_id) {
                    m = &snap.m[i];
                    break;
                }
            }
            /* No slot yet: the poller rebuilds on its next cycle, and the
             * tile keeps what it last showed rather than flashing empty. */
            if (m == NULL) continue;
            tile_data_t d = {
                .valid        = m->valid,
                .warming      = m->warming,
                .restarted    = m->restarted,
                .value        = m->value,
                .num          = m->num,
                .suffix       = m->suffix,
                .numeric_only = m->numeric_only,
                .has_hist     = m->has_hist,
                .n_buckets    = m->n_buckets,
                .bucket_le    = m->bucket_le,
                .bucket_share = m->bucket_share,
                .p50          = m->p50,
                .p90          = m->p90,
                .p99          = m->p99,
                .fmt          = (fmt_mode_t)m->fmt,
                .unit         = m->unit,
                .scale        = m->scale,
                .group        = m->group,
                .peak         = m->peak,
                .seen_fraction = m->seen_fraction,
                .n_children   = m->n_children,
                .n_matched    = m->n_matched,
                .child_label  = m->child_label,
                .child_value  = m->child_value,
                .child_num    = m->child_num,
                .child_order  = m->child_order,
            };
            tile_update(s_tiles[t], &d);
        }

        int8_t rssi = 0;
        wifi_mgr_info(NULL, 0, &rssi);
        signal_set_level(s_hdr_sig, wifi_level(rssi));

        label_set_fmt_if_changed(s_ftr_right, "SRAM %uK  PSRAM %uK",
                                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    /* The age readout has to keep counting even when no new data arrived, so
     * it is refreshed outside the generation check -- but from the same
     * snapshot. */
    if (snap.last_ok_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - snap.last_ok_ms) / 1000);
        /* The scrape figures live here, beside the status they describe,
         * rather than on the right where they crowded the memory readout
         * off the edge. */
        label_set_fmt_if_changed(s_ftr_left,
                                 "updated %ds ago   %s   %u samples  %u KB  %u ms",
                                 age, snap.status,
                                 (unsigned)snap.samples,
                                 (unsigned)(snap.body_bytes / 1024),
                                 (unsigned)snap.latency_ms);
    } else {
        label_set_fmt_if_changed(s_ftr_left, "%s", snap.status);
    }
    text_color_if_changed(s_ftr_left, snap.ok ? COL_DIM : COL_WARN);
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
        /*
         * Inside the lock: config_load creates the debounced-flush lv_timer,
         * and the dashboard reads the endpoint name for its header.
         */
        if (config_load() != ESP_OK && config_was_reset()) {
            ESP_LOGW(TAG, "starting from factory defaults");
        }
        /* Before a single widget exists: colours are read at build time. */
        app_theme_set(app_theme_from_name(config_get()->device.theme));
        timekeep_set_tz(config_get()->device.tz);
        ui_kbd_init();
        if (secrets_have_wifi()) {
            build_dashboard();
            lv_timer_create(dashboard_tick, 250, NULL);
        } else {
            /* First boot: land straight in setup rather than showing a
             * dashboard that cannot possibly have data. */
            ui_setup_open(browser_closed);
        }
        /* Created regardless of which screen is up: the heartbeat is the only
         * way to see this device's state over serial, since the native-USB
         * console loses everything printed before a host attaches. */
        lvgl_port_unlock();
    }

    if (secrets_have_wifi()) {
        const config_t *c = config_get();
        const char *url = c->n_endpoints ? c->endpoints[0].url : "";
        int interval = c->n_endpoints && c->endpoints[0].poll_s
                     ? c->endpoints[0].poll_s : c->device.poll_default_s;
        poller_start(url, interval);
        if (url[0] == '\0') {
            ESP_LOGW(TAG, "no endpoint configured; use the gear button");
        }
    }

    if (secrets_have_wifi() && webcfg_start() != ESP_OK) {
        ESP_LOGW(TAG, "config endpoint unavailable");
    }

    ESP_LOGI(TAG, "boot complete: SRAM %u KB free, PSRAM %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
