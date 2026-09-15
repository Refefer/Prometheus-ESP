#include "ui_browser.h"

#include "config.h"
#include "http_util.h"
#include "lvgl_port.h"
#include "poller.h"
#include "prom_ident.h"
#include "prom_text.h"
#include "ui_fmt.h"
#include "ui_kbd.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "browser";

/*
 * Discovery caps. The catalog is a browsing-time structure: allocated when
 * this screen opens and freed when it closes, so its footprint is a transient
 * peak rather than a steady-state cost.
 */
#define CAT_MAX_NAMES 512
#define CAT_NAME_MAX  112
#define ROWS_VISIBLE    8

typedef struct {
    char        name[CAT_NAME_MAX];
    prom_type_t type;
    uint16_t    series;              /* saturating */
    double      value;               /* first sample seen, for the preview */
    bool        has_value;
    char        sel[CFG_SEL_MAX];    /* first series, as a selector */
} cat_entry_t;

static cat_entry_t *s_cat;
static int          s_cat_n;
static volatile bool s_scanning;

/* Filtered view into the catalog. */
static uint16_t s_filt[CAT_MAX_NAMES];
static int      s_filt_n;
static int      s_page;
static char     s_query[48];
static bool     s_selected_only;

static lv_obj_t *s_root, *s_status, *s_count, *s_search_btn, *s_page_lbl;
static lv_obj_t *s_row[ROWS_VISIBLE], *s_row_tick[ROWS_VISIBLE];
static lv_obj_t *s_row_name[ROWS_VISIBLE], *s_row_type[ROWS_VISIBLE];
static lv_obj_t *s_row_val[ROWS_VISIBLE];
static void (*s_on_close)(void);

static void refilter(void);
static void render_rows(void);

/* ------------------------------------------------------------- discovery */

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)   { heap_caps_free(p); }

/* Catalog entries are appended in first-seen order and looked up linearly.
 * Exposition format groups a family's samples together, so the entry being
 * added is almost always the last one -- checking that first turns what looks
 * like O(n*m) into O(n) in practice. */
static cat_entry_t *cat_find_or_add(const char *name, size_t len)
{
    if (len >= CAT_NAME_MAX) len = CAT_NAME_MAX - 1;

    if (s_cat_n > 0) {
        cat_entry_t *last = &s_cat[s_cat_n - 1];
        if (strlen(last->name) == len && memcmp(last->name, name, len) == 0) {
            return last;
        }
    }
    for (int i = 0; i < s_cat_n; i++) {
        if (strlen(s_cat[i].name) == len && memcmp(s_cat[i].name, name, len) == 0) {
            return &s_cat[i];
        }
    }
    if (s_cat_n >= CAT_MAX_NAMES) return NULL;

    cat_entry_t *e = &s_cat[s_cat_n++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, len);
    e->name[len] = '\0';
    return e;
}

static bool discover_sample(void *ctx, const prom_sample_t *s)
{
    (void)ctx;
    /* Group by FAMILY: a histogram's _bucket/_sum/_count are one metric to a
     * person, not three. */
    cat_entry_t *e = cat_find_or_add(s->base_name, s->base_len);
    if (e == NULL) return true;

    if (s->type != PROM_TYPE_UNTYPED) e->type = s->type;
    if (e->series < 0xFFFF) e->series++;

    if (!e->has_value && prom_is_num(s->value)) {
        e->value = s->value.num;
        e->has_value = true;
        /* Remember the first series verbatim so ticking the name has a
         * concrete selector to bind to. */
        prom_render(e->sel, sizeof(e->sel), s->base_name, s->base_len,
                    s->labels, s->n_labels);
    }
    return true;
}

static bool discover_chunk(void *ctx, const char *d, size_t n)
{
    return prom_text_feed((prom_text_parser_t *)ctx, d, n);
}

static void discover_task(void *arg)
{
    (void)arg;

    const config_t *c = config_get();
    char url[CFG_URL_MAX] = "";
    if (c->n_endpoints) strncpy(url, c->endpoints[0].url, sizeof(url) - 1);

    const prom_text_sink_t sink = { NULL, NULL, discover_sample };
    prom_text_parser_t *p = prom_text_new(&sink, NULL, psram_alloc, psram_free);

    http_result_t res = {0};
    prom_text_stats_t st = {0};
    if (p && url[0]) {
        /* Slot -1: a one-shot connection, so discovery never disturbs the
         * socket the live poller is using. */
        http_get_stream(-1, url, NULL, discover_chunk, p, 12000, &res);
        prom_text_finish(p, &st);
    }
    if (p) prom_text_free(p);

    char msg[96];
    if (url[0] == '\0')            snprintf(msg, sizeof(msg), "no endpoint configured");
    else if (p == NULL)            snprintf(msg, sizeof(msg), "out of memory");
    else if (res.klass != HTTP_ERR_NONE)
        snprintf(msg, sizeof(msg), "%s", http_err_text(res.klass));
    else if (st.samples == 0)      snprintf(msg, sizeof(msg), "no metrics found");
    else snprintf(msg, sizeof(msg), "%d metrics, %u series, %u KB",
                  s_cat_n, (unsigned)st.samples, (unsigned)(res.bytes / 1024));

    if (lvgl_port_lock(-1)) {
        if (s_root != NULL) {          /* the screen can be closed mid-scan */
            label_set_if_changed(s_status, msg);
            text_color_if_changed(s_status, s_cat_n ? COL_OK : COL_WARN);
            refilter();
            render_rows();
        }
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "discovery: %s", msg);
    s_scanning = false;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------- filter */

static bool contains_ci(const char *hay, const char *needle)
{
    if (needle[0] == '\0') return true;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

static uint16_t ep_id(void)
{
    const config_t *c = config_get();
    return c->n_endpoints ? c->endpoints[0].id : 0;
}

static void refilter(void)
{
    s_filt_n = 0;
    for (int i = 0; i < s_cat_n; i++) {
        if (!contains_ci(s_cat[i].name, s_query)) continue;
        if (s_selected_only && !config_has_panel(ep_id(), s_cat[i].sel)) continue;
        s_filt[s_filt_n++] = (uint16_t)i;
    }
    int pages = (s_filt_n + ROWS_VISIBLE - 1) / ROWS_VISIBLE;
    if (s_page >= pages) s_page = pages > 0 ? pages - 1 : 0;
}

/* Cells occupied on screen 0, so capacity is visible while picking rather
 * than discovered by a refusal. */
static int cells_used(void)
{
    const config_t *c = config_get();
    int n = 0;
    for (int i = 0; i < c->n_panels; i++) {
        if (!c->panels[i].sel[0] || c->panels[i].screen != 0) continue;
        n += (c->panels[i].w ? c->panels[i].w : 1) *
             (c->panels[i].h ? c->panels[i].h : 1);
    }
    return n;
}

/* ------------------------------------------------------------- selection */

/*
 * Turn a ticked metric into a panel, using the naming conventions to pick the
 * format, aggregation and widget. This is what makes auto-discovery usable
 * rather than merely possible: the common case needs no further input.
 */
static void add_panel_for(const cat_entry_t *e)
{
    cfg_panel_t *p = config_panel_add();
    if (p == NULL) return;

    p->ep_id = ep_id();
    strncpy(p->sel, e->sel[0] ? e->sel : e->name, sizeof(p->sel) - 1);

    fmt_mode_t fmt = FMT_SI;
    agg_mode_t agg = AGG_LAST;
    ui_fmt_infer(e->name, strlen(e->name), (int)e->type,
                 &fmt, p->unit, sizeof(p->unit), &agg);
    p->fmt = fmt;
    p->agg = agg;

    switch (e->type) {
    case PROM_TYPE_HISTOGRAM:
    case PROM_TYPE_SUMMARY:
        /*
         * Show the distribution, not just a number. Reducing a histogram to
         * one p99 throws away the shape, which is usually the interesting
         * part -- a bimodal latency distribution and a smooth one can share a
         * p99 and mean entirely different things.
         *
         * q is still set because the renderer falls back to a plain quantile
         * when the tile is too small for bars.
         */
        p->q    = 0.99f;
        p->kind = TILE_HIST;
        p->w    = 2;
        p->h    = 2;
        break;
    case PROM_TYPE_COUNTER:
        p->kind = TILE_SPARK;      /* a rate is only meaningful over time */
        break;
    default:
        p->kind = (fmt == FMT_PCT_01 || fmt == FMT_PCT_100) ? TILE_GAUGE
                                                            : TILE_STAT;
        if (p->kind == TILE_GAUGE) {
            p->vmin = 0.0f;
            p->vmax = (fmt == FMT_PCT_01) ? 100.0f : 100.0f;
        }
        break;
    }

    /* A quantile panel shows a duration; the family name says seconds. */
    if (p->q > 0.0f && fmt == FMT_DURATION) p->fmt = FMT_DURATION;

    size_t n = strlen(e->name);
    const char *shortname = e->name;
    /* Trim a common vendor prefix so the tile title is readable at 14px. */
    const char *colon = strchr(e->name, ':');
    if (colon && colon[1]) { shortname = colon + 1; n = strlen(shortname); }
    strncpy(p->title, shortname, sizeof(p->title) - 1);
    (void)n;

    if (!config_place_panel(p)) {
        /*
         * No room. Refusing beats silently dropping the selection or
         * reshuffling tiles the user already arranged -- but say what would
         * fit, because a 2x2 failing on a screen with three free cells looks
         * like a bug otherwise.
         */
        char msg[80];
        snprintf(msg, sizeof(msg), "No room for a %ux%u tile (%d of %d cells used)",
                 p->w, p->h, cells_used(), GRID_COLS * GRID_ROWS);
        config_panel_remove(p->id);
        ui_toast(msg, SEV_WARN, 3000);
        return;
    }
    config_touch();
}

static void remove_panel_for(const cat_entry_t *e)
{
    const config_t *c = config_get();
    const char *sel = e->sel[0] ? e->sel : e->name;
    for (int i = 0; i < c->n_panels; i++) {
        if (c->panels[i].ep_id == ep_id() &&
            strcmp(c->panels[i].sel, sel) == 0) {
            config_panel_remove(c->panels[i].id);
            return;
        }
    }
}

static void row_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int idx  = s_page * ROWS_VISIBLE + slot;
    if (idx >= s_filt_n) return;

    cat_entry_t *ce = &s_cat[s_filt[idx]];
    const char *sel = ce->sel[0] ? ce->sel : ce->name;
    if (config_has_panel(ep_id(), sel)) remove_panel_for(ce);
    else                                add_panel_for(ce);
    render_rows();
}

/* --------------------------------------------------------------- render */

static const char *type_badge(prom_type_t t)
{
    switch (t) {
    case PROM_TYPE_COUNTER:   return "counter";
    case PROM_TYPE_GAUGE:     return "gauge";
    case PROM_TYPE_HISTOGRAM: return "histogram";
    case PROM_TYPE_SUMMARY:   return "summary";
    default:                  return "untyped";
    }
}

static void render_rows(void)
{
    int sel_total = 0;
    const config_t *c = config_get();
    for (int i = 0; i < c->n_panels; i++) if (c->panels[i].sel[0]) sel_total++;

    for (int r = 0; r < ROWS_VISIBLE; r++) {
        int idx = s_page * ROWS_VISIBLE + r;
        if (idx >= s_filt_n) { hidden_if_changed(s_row[r], true); continue; }
        hidden_if_changed(s_row[r], false);

        cat_entry_t *e = &s_cat[s_filt[idx]];
        const char *sel = e->sel[0] ? e->sel : e->name;
        bool on = config_has_panel(ep_id(), sel);

        label_set_if_changed(s_row_tick[r], on ? LV_SYMBOL_OK : "");
        text_color_if_changed(s_row_tick[r], on ? COL_ACCENT : COL_DIM);
        label_set_if_changed(s_row_name[r], e->name);
        text_color_if_changed(s_row_name[r], on ? COL_TEXT : COL_DIM);

        label_set_fmt_if_changed(s_row_type[r], "%s  x%u",
                                 type_badge(e->type), (unsigned)e->series);

        if (e->has_value) {
            fmt_mode_t fmt = FMT_SI; agg_mode_t agg; char unit[8];
            ui_fmt_infer(e->name, strlen(e->name), (int)e->type,
                         &fmt, unit, sizeof(unit), &agg);
            char buf[32];
            /* The preview shows the RAW sample, so a counter reads as its
             * total here even though its tile will show a rate. Formatting it
             * as a rate would be a lie: there is only one sample. */
            ui_fmt_join(e->value, (fmt == FMT_RATE_SI)  ? FMT_SI
                                : (fmt == FMT_RATE_IEC) ? FMT_IEC
                                : (fmt == FMT_PCT_01 && agg == AGG_RATE) ? FMT_SI
                                : fmt,
                        unit, buf, sizeof(buf));
            label_set_if_changed(s_row_val[r], buf);
        } else {
            label_set_if_changed(s_row_val[r], "");
        }
    }

    int pages = (s_filt_n + ROWS_VISIBLE - 1) / ROWS_VISIBLE;
    label_set_fmt_if_changed(s_page_lbl, "page %d / %d",
                             pages ? s_page + 1 : 0, pages);
    int used = cells_used();
    int total = GRID_COLS * GRID_ROWS;
    label_set_fmt_if_changed(s_count, "%d shown   %d selected   %d/%d cells",
                             s_filt_n, sel_total, used, total);
    /* Amber once the screen is nearly full, so "why did nothing happen?"
     * becomes "ah, it is full" before the refusal rather than after. */
    text_color_if_changed(s_count, used >= total ? COL_WARN : COL_DIM);
}

/* --------------------------------------------------------------- events */

static void search_done(const char *text, void *user)
{
    (void)user;
    if (text == NULL) return;
    strncpy(s_query, text, sizeof(s_query) - 1);
    s_query[sizeof(s_query) - 1] = '\0';
    lv_obj_t *l = lv_obj_get_child(s_search_btn, 0);
    if (l) label_set_if_changed(l, s_query[0] ? s_query : "search...");
    s_page = 0;
    refilter();
    render_rows();
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    ui_kbd_req_t req = {
        .title = "Search metrics",
        .label = "Name contains",
        .value = s_query,
        .kind  = KB_TEXT,
        .done  = search_done,
    };
    ui_kbd_edit(&req);
}

static void selected_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    s_selected_only = !s_selected_only;
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    if (l) label_set_if_changed(l, s_selected_only ? "selected" : "all");
    s_page = 0;
    refilter();
    render_rows();
}

static void page_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int pages = (s_filt_n + ROWS_VISIBLE - 1) / ROWS_VISIBLE;
    if (pages == 0) return;
    s_page = (s_page + dir + pages) % pages;
    render_rows();
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    if (s_scanning) return;
    s_scanning = true;
    s_cat_n = 0;
    label_set_if_changed(s_status, "scanning...");
    text_color_if_changed(s_status, COL_DIM);
    xTaskCreate(discover_task, "discover", 8192, NULL, 4, NULL);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    if (s_root == NULL) return;

    config_flush();
    lv_obj_del(s_root);
    s_root = NULL;
    s_status = s_count = s_search_btn = s_page_lbl = NULL;
    for (int i = 0; i < ROWS_VISIBLE; i++) s_row[i] = NULL;

    /* The catalog is a browsing-time structure; holding ~150KB of PSRAM for a
     * screen nobody is looking at is pure waste. */
    if (s_cat) { heap_caps_free(s_cat); s_cat = NULL; s_cat_n = 0; }

    if (s_on_close) s_on_close();
}

/* ----------------------------------------------------------------- open */


void ui_browser_open(void (*on_close)(void))
{
    if (s_root) return;
    s_on_close = on_close;

    s_cat = heap_caps_malloc(sizeof(cat_entry_t) * CAT_MAX_NAMES, MALLOC_CAP_SPIRAM);
    if (s_cat == NULL) {
        ui_toast("Not enough memory to browse", SEV_CRIT, 3000);
        return;
    }
    s_cat_n = 0;
    s_page = 0;
    s_query[0] = '\0';

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

    lv_obj_t *title = make_label(s_root, FONT_L, COL_TEXT);
    lv_label_set_text(title, "Metrics");
    lv_obj_set_pos(title, GRID_MX, 10);

    s_status = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_set_pos(s_status, 130, 14);

    s_count = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_set_pos(s_count, 430, 14);

    lv_obj_t *done = make_btn_accent(s_root, LV_SYMBOL_OK "  Done", close_cb, NULL);
    lv_obj_set_size(done, 130, 34);
    lv_obj_set_pos(done, SCR_W - 130 - GRID_MX, 6);

    /* search + filters */
    s_search_btn = make_btn(s_root, "search...", search_cb, NULL);
    lv_obj_set_size(s_search_btn, 330, 40);
    lv_obj_set_pos(s_search_btn, GRID_MX, 46);

    lv_obj_t *selbtn = make_btn(s_root, "all", selected_cb, NULL);
    lv_obj_set_size(selbtn, 130, 40);
    lv_obj_set_pos(selbtn, GRID_MX + 340, 46);

    lv_obj_t *rescan = make_btn(s_root, LV_SYMBOL_REFRESH "  Rescan",
                                rescan_cb, NULL);
    lv_obj_set_size(rescan, 150, 40);
    lv_obj_set_pos(rescan, GRID_MX + 480, 46);

    /* A fixed pool of rows, rewritten in place and paged.
     *
     * Not a virtualised scroll: rebinding rows inside LV_EVENT_SCROLL at 30fps
     * stutters badly on an RGB/PSRAM pipeline, it is several times the code,
     * and on glass a page is a stable target where a moving list is not. */
    for (int r = 0; r < ROWS_VISIBLE; r++) {
        lv_coord_t y = 96 + r * 42;
        s_row[r] = lv_btn_create(s_root);
        lv_obj_set_size(s_row[r], SCR_W - 2 * GRID_MX, 38);
        lv_obj_set_pos(s_row[r], GRID_MX, y);
        lv_obj_set_style_bg_color(s_row[r], (r % 2) ? COL_PANEL : COL_PANEL_ALT, 0);
        lv_obj_set_style_bg_color(s_row[r], COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(s_row[r], 4, 0);
        lv_obj_set_style_border_width(s_row[r], 0, 0);
        lv_obj_set_style_shadow_width(s_row[r], 0, 0);
        lv_obj_set_style_pad_all(s_row[r], 0, 0);
        lv_obj_add_event_cb(s_row[r], row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)r);

        s_row_tick[r] = make_label(s_row[r], FONT_M, COL_DIM);
        lv_obj_set_pos(s_row_tick[r], 14, 10);

        s_row_name[r] = make_label(s_row[r], FONT_M, COL_DIM);
        lv_label_set_long_mode(s_row_name[r], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_row_name[r], 420);
        lv_obj_set_pos(s_row_name[r], 48, 10);

        s_row_type[r] = make_label(s_row[r], FONT_XS, COL_DIM);
        lv_obj_set_pos(s_row_type[r], 480, 13);

        s_row_val[r] = make_label(s_row[r], FONT_S, COL_TEXT);
        lv_obj_set_pos(s_row_val[r], 610, 11);

        hidden_if_changed(s_row[r], true);
    }

    lv_obj_t *prev = make_btn(s_root, LV_SYMBOL_LEFT, page_cb, (void *)(intptr_t)-1);
    lv_obj_set_size(prev, 70, 40);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_LEFT, GRID_MX, -12);

    s_page_lbl = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_align(s_page_lbl, LV_ALIGN_BOTTOM_LEFT, GRID_MX + 90, -24);

    lv_obj_t *next = make_btn(s_root, LV_SYMBOL_RIGHT, page_cb, (void *)(intptr_t)1);
    lv_obj_set_size(next, 70, 40);
    lv_obj_align(next, LV_ALIGN_BOTTOM_LEFT, GRID_MX + 220, -12);

    rescan_cb(NULL);
}

bool ui_browser_is_open(void) { return s_root != NULL; }
