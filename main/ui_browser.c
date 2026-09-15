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
#define CAT_MAX_NAMES  384
#define CAT_NAME_MAX   112
#define ROWS_VISIBLE     8
/* Series kept per metric name, and in total. A family with more than this is
 * shown truncated -- past a couple of dozen label sets the useful tool is a
 * multi-series tile or a PromQL aggregation, not a longer list. */
#define CAT_MAX_SER_PER  24
#define CAT_MAX_SERIES  768

typedef struct {
    char        name[CAT_NAME_MAX];
    prom_type_t type;
    uint16_t    series;              /* saturating */
    double      value;               /* first sample seen, for the preview */
    bool        has_value;
    char        sel[CFG_SEL_MAX];    /* first series, as a selector */
    uint16_t    ser[CAT_MAX_SER_PER];/* indices into the series pool */
    uint8_t     n_ser;
    bool        ser_truncated;
} cat_entry_t;

/* One entry per distinct label set. Buckets and quantiles collapse into their
 * parent series here, because the parser already strips le and quantile from
 * the label set -- so a 19-bucket histogram is one series, not nineteen. */
typedef struct {
    char   sel[CFG_SEL_MAX];         /* full selector, what a panel binds to */
    char   labels[72];               /* the distinguishing part, for display */
    float  value;
    bool   has_value;
} ser_entry_t;

static cat_entry_t  *s_cat;
static int           s_cat_n;
static ser_entry_t  *s_ser;
static int           s_ser_n;
static volatile bool s_scanning;

/* Level 0 lists names; level 1 lists one name's label sets. */
static int s_level;
static int s_drill = -1;

/* Filtered view into the catalog. */
static uint16_t s_filt[CAT_MAX_NAMES];
static int      s_filt_n;
static int      s_page;
static char     s_query[48];
static bool     s_selected_only;

static lv_obj_t *s_root, *s_status, *s_count, *s_search_btn, *s_page_lbl;
static lv_obj_t *s_row[ROWS_VISIBLE], *s_row_tick[ROWS_VISIBLE];
static lv_obj_t *s_row_name[ROWS_VISIBLE], *s_row_type[ROWS_VISIBLE];
static lv_obj_t *s_row_val[ROWS_VISIBLE], *s_row_more[ROWS_VISIBLE];
static lv_obj_t *s_back_btn;
static void (*s_on_close)(void);

/* Pick-one mode: the cell the new tile should occupy. */
static bool     s_pick_mode;
static bool     s_picked;
static uint8_t  s_pick_col, s_pick_row;
static void   (*s_on_pick)(uint16_t panel_id);

static void refilter(void);
static void render_rows(void);
static void close_cb(lv_event_t *e);

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

/* Just the labels, for the level-1 rows: the metric name is in the header and
 * repeating it on every row wastes the width that distinguishes them. */
static void render_labels(const prom_sample_t *s, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < s->n_labels && o + 2 < cap; i++) {
        if (o > 0) { out[o++] = ' '; if (o + 1 >= cap) break; }
        size_t kn = s->labels[i].key_len, vn = s->labels[i].val_len;
        if (o + kn + vn + 2 >= cap) break;
        memcpy(out + o, s->labels[i].key, kn); o += kn;
        out[o++] = '=';
        memcpy(out + o, s->labels[i].val, vn); o += vn;
    }
    out[o] = '\0';
    if (out[0] == '\0') strncpy(out, "(no labels)", cap - 1);
}

static bool discover_sample(void *ctx, const prom_sample_t *s)
{
    (void)ctx;
    /* Group by FAMILY: a histogram's _bucket/_sum/_count are one metric to a
     * person, not three. */
    cat_entry_t *e = cat_find_or_add(s->base_name, s->base_len);
    if (e == NULL) return true;

    if (s->type != PROM_TYPE_UNTYPED) e->type = s->type;

    char sel[CFG_SEL_MAX];
    if (prom_render(sel, sizeof(sel), s->base_name, s->base_len,
                    s->labels, s->n_labels) == 0) {
        return true;
    }

    /*
     * Distinct LABEL SETS, not samples. The parser strips le and quantile
     * from the label set, so every bucket of a histogram renders to the same
     * selector and collapses to one series here -- which is what a person
     * means by "how many series does this metric have".
     */
    int found = -1;
    for (int i = 0; i < e->n_ser; i++) {
        if (strcmp(s_ser[e->ser[i]].sel, sel) == 0) { found = i; break; }
    }

    if (found < 0) {
        if (e->n_ser >= CAT_MAX_SER_PER || s_ser_n >= CAT_MAX_SERIES) {
            e->ser_truncated = true;
        } else {
            ser_entry_t *se = &s_ser[s_ser_n];
            memset(se, 0, sizeof(*se));
            strncpy(se->sel, sel, sizeof(se->sel) - 1);
            render_labels(s, se->labels, sizeof(se->labels));
            if (prom_is_num(s->value)) {
                se->value = (float)s->value.num;
                se->has_value = true;
            }
            e->ser[e->n_ser++] = (uint16_t)s_ser_n++;
            if (e->series < 0xFFFF) e->series++;
            found = e->n_ser - 1;
        }
    }

    if (!e->has_value && prom_is_num(s->value)) {
        e->value = s->value.num;
        e->has_value = true;
        /* The first series verbatim, so ticking the NAME still has a concrete
         * selector to bind to. */
        strncpy(e->sel, sel, sizeof(e->sel) - 1);
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

    if (s_level == 1 && s_drill >= 0 && s_drill < s_cat_n) {
        /* Level 1 lists one name's label sets. Search applies to the labels,
         * which is how you find cpu="3" among sixteen. */
        const cat_entry_t *e = &s_cat[s_drill];
        for (int i = 0; i < e->n_ser; i++) {
            const ser_entry_t *se = &s_ser[e->ser[i]];
            if (!contains_ci(se->labels, s_query)) continue;
            if (s_selected_only && !config_has_panel(ep_id(), se->sel)) continue;
            s_filt[s_filt_n++] = e->ser[i];
        }
    } else {
        for (int i = 0; i < s_cat_n; i++) {
            if (!contains_ci(s_cat[i].name, s_query)) continue;
            if (s_selected_only && !config_has_panel(ep_id(), s_cat[i].sel)) continue;
            s_filt[s_filt_n++] = (uint16_t)i;
        }
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

/* True if this panel's span fits with its top-left at (col,row). */
static bool cell_span_free(const cfg_panel_t *me, uint8_t col, uint8_t row)
{
    if (col + me->w > GRID_COLS || row + me->h > GRID_ROWS) return false;
    const config_t *c = config_get();
    for (int i = 0; i < c->n_panels; i++) {
        const cfg_panel_t *o = &c->panels[i];
        if (o == me || !o->sel[0] || o->screen != 0) continue;
        uint8_t ow = o->w ? o->w : 1, oh = o->h ? o->h : 1;
        bool overlap = !(col + me->w <= o->col || o->col + ow <= col ||
                         row + me->h <= o->row || o->row + oh <= row);
        if (overlap) return false;
    }
    return true;
}

/* ------------------------------------------------------------- selection */

/*
 * Turn a ticked metric into a panel, using the naming conventions to pick the
 * format, aggregation and widget. This is what makes auto-discovery usable
 * rather than merely possible: the common case needs no further input.
 */
/* Returns the new panel's id, or 0 if it could not be placed. */
static uint16_t add_panel_for(const cat_entry_t *e, bool at_cell,
                              uint8_t col, uint8_t row)
{
    cfg_panel_t *p = config_panel_add();
    if (p == NULL) return 0;

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

    if (at_cell) {
        /*
         * The user tapped a specific empty cell, so start there. A widget
         * whose natural span does not fit from that corner is shrunk to 1x1
         * rather than moved elsewhere -- landing somewhere other than where
         * they tapped would be the surprising outcome.
         */
        p->col = col;
        p->row = row;
        if (!cell_span_free(p, col, row)) {
            p->w = 1;
            p->h = 1;
        }
    } else if (!config_place_panel(p)) {
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
        return 0;
    }
    config_touch();
    return p->id;
}

static void remove_panel_sel(const char *sel)
{
    const config_t *c = config_get();
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

    if (s_level == 1) {
        const ser_entry_t *se = &s_ser[s_filt[idx]];
        cat_entry_t *parent = &s_cat[s_drill];

        if (s_pick_mode) {
            /* Bind the panel to this exact label set rather than the family's
             * first one. */
            cat_entry_t one = *parent;
            strncpy(one.sel, se->sel, sizeof(one.sel) - 1);
            one.value = se->value;
            one.has_value = se->has_value;
            uint16_t id = add_panel_for(&one, true, s_pick_col, s_pick_row);
            void (*cb)(uint16_t) = s_on_pick;
            s_picked = true;
            close_cb(NULL);
            if (cb) cb(id);
            return;
        }

        if (config_has_panel(ep_id(), se->sel)) {
            remove_panel_sel(se->sel);
        } else {
            cat_entry_t one = *parent;
            strncpy(one.sel, se->sel, sizeof(one.sel) - 1);
            one.value = se->value;
            one.has_value = se->has_value;
            add_panel_for(&one, false, 0, 0);
        }
        render_rows();
        return;
    }

    cat_entry_t *ce = &s_cat[s_filt[idx]];

    if (s_pick_mode) {
        /* One tap fills the cell and hands straight to the widget picker, so
         * the whole flow is: tap the hole, tap the metric, choose how it
         * looks. */
        uint16_t id = add_panel_for(ce, true, s_pick_col, s_pick_row);
        void (*cb)(uint16_t) = s_on_pick;
        s_picked = true;
        close_cb(NULL);
        if (cb) cb(id);
        return;
    }

    const char *sel = ce->sel[0] ? ce->sel : ce->name;
    if (config_has_panel(ep_id(), sel)) remove_panel_sel(sel);
    else                                add_panel_for(ce, false, 0, 0);
    render_rows();
}

static void drill_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int idx  = s_page * ROWS_VISIBLE + slot;
    if (s_level != 0 || idx >= s_filt_n) return;

    s_drill = s_filt[idx];
    s_level = 1;
    s_page  = 0;
    s_query[0] = '\0';
    lv_obj_t *l = lv_obj_get_child(s_search_btn, 0);
    if (l) label_set_if_changed(l, "search...");
    refilter();
    render_rows();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    s_level = 0;
    s_drill = -1;
    s_page  = 0;
    s_query[0] = '\0';
    lv_obj_t *l = lv_obj_get_child(s_search_btn, 0);
    if (l) label_set_if_changed(l, "search...");
    refilter();
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

    bool drilled = (s_level == 1 && s_drill >= 0 && s_drill < s_cat_n);
    hidden_if_changed(s_back_btn, !drilled);

    for (int r = 0; r < ROWS_VISIBLE; r++) {
        int idx = s_page * ROWS_VISIBLE + r;
        if (idx >= s_filt_n) { hidden_if_changed(s_row[r], true); continue; }
        hidden_if_changed(s_row[r], false);

        const char *sel, *primary, *secondary = "";
        bool has_value; float value;
        bool can_drill = false;
        char sec[40];

        if (drilled) {
            const ser_entry_t *se = &s_ser[s_filt[idx]];
            sel = se->sel;
            primary = se->labels;
            has_value = se->has_value;
            value = se->value;
        } else {
            const cat_entry_t *e = &s_cat[s_filt[idx]];
            sel = e->sel[0] ? e->sel : e->name;
            primary = e->name;
            has_value = e->has_value;
            value = (float)e->value;
            snprintf(sec, sizeof(sec), "%s  x%u%s", type_badge(e->type),
                     (unsigned)e->series, e->ser_truncated ? "+" : "");
            secondary = sec;
            /* Only offer the drill-down where there is something to drill
             * into: a chevron on a single-series metric is a dead end. */
            can_drill = (e->n_ser > 1);
        }

        bool on = config_has_panel(ep_id(), sel);
        label_set_if_changed(s_row_tick[r], on ? LV_SYMBOL_OK : "");
        text_color_if_changed(s_row_tick[r], on ? COL_ACCENT : COL_DIM);
        label_set_if_changed(s_row_name[r], primary);
        text_color_if_changed(s_row_name[r], on ? COL_TEXT : COL_DIM);
        label_set_if_changed(s_row_type[r], secondary);

        hidden_if_changed(s_row_more[r], !can_drill);

        if (has_value) {
            fmt_mode_t fmt = FMT_SI; agg_mode_t agg; char unit[8];
            const char *nm = drilled ? s_cat[s_drill].name
                                     : s_cat[s_filt[idx]].name;
            prom_type_t ty = drilled ? s_cat[s_drill].type
                                     : s_cat[s_filt[idx]].type;
            ui_fmt_infer(nm, strlen(nm), (int)ty, &fmt, unit, sizeof(unit), &agg);
            char buf[32];
            /* The preview shows the RAW sample, so a counter reads as its
             * total here even though its tile will show a rate. Formatting it
             * as a rate would be a lie: there is only one sample. */
            ui_fmt_join(value, (fmt == FMT_RATE_SI)  ? FMT_SI
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
    if (drilled) {
        label_set_fmt_if_changed(s_count, "%.40s   %d series   %d/%d cells",
                                 s_cat[s_drill].name, s_filt_n, used, total);
    } else {
        label_set_fmt_if_changed(s_count, "%d shown   %d selected   %d/%d cells",
                                 s_filt_n, sel_total, used, total);
    }
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
    if (l) label_set_if_changed(l, s_selected_only ? "Show: selected"
                                                   : "Show: all");
    /* Tinted while filtering, so it is obvious the list is not everything. */
    bg_color_if_changed(btn, s_selected_only ? COL_ACCENT : COL_PANEL);
    if (l) text_color_if_changed(l, s_selected_only ? COL_BG : COL_TEXT);
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
    s_ser_n = 0;
    s_level = 0;
    s_drill = -1;
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
    if (s_ser) { heap_caps_free(s_ser); s_ser = NULL; s_ser_n = 0; }
    s_level = 0; s_drill = -1;

    if (s_pick_mode) {
        s_pick_mode = false;
        /* In pick mode the caller is notified by row_cb, not here; reaching
         * this point means the user backed out. */
        void (*cb)(uint16_t) = s_on_pick;
        s_on_pick = NULL;
        if (cb && !s_picked) cb(0);
        s_picked = false;
        return;
    }
    if (s_on_close) s_on_close();
}

/* ----------------------------------------------------------------- open */


static void browser_build(const char *heading);

void ui_browser_open_pick(uint8_t col, uint8_t row,
                          void (*on_pick)(uint16_t panel_id))
{
    if (s_root) return;
    s_pick_mode = true;
    s_picked    = false;
    s_pick_col  = col;
    s_pick_row  = row;
    s_on_pick   = on_pick;
    s_on_close  = NULL;
    browser_build("Pick a metric");
}

void ui_browser_open(void (*on_close)(void))
{
    if (s_root) return;
    s_pick_mode = false;
    s_on_close = on_close;
    browser_build("Metrics");
}

static void browser_build(const char *heading)
{

    s_cat = heap_caps_malloc(sizeof(cat_entry_t) * CAT_MAX_NAMES, MALLOC_CAP_SPIRAM);
    s_ser = heap_caps_malloc(sizeof(ser_entry_t) * CAT_MAX_SERIES, MALLOC_CAP_SPIRAM);
    if (s_cat == NULL || s_ser == NULL) {
        if (s_cat) { heap_caps_free(s_cat); s_cat = NULL; }
        if (s_ser) { heap_caps_free(s_ser); s_ser = NULL; }
        ui_toast("Not enough memory to browse", SEV_CRIT, 3000);
        return;
    }
    s_cat_n = 0;
    s_ser_n = 0;
    s_level = 0;
    s_drill = -1;
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
    lv_label_set_text(title, heading);
    lv_obj_set_pos(title, GRID_MX, 10);

    s_status = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_set_pos(s_status, 130, 14);

    s_count = make_label(s_root, FONT_S, COL_DIM);
    lv_obj_set_pos(s_count, 430, 14);

    lv_obj_t *done = make_btn_accent(s_root,
                                     s_pick_mode ? LV_SYMBOL_CLOSE "  Cancel"
                                                 : LV_SYMBOL_OK "  Done",
                                     close_cb, NULL);
    lv_obj_set_size(done, 130, 34);
    lv_obj_set_pos(done, SCR_W - 130 - GRID_MX, 6);

    /* search + filters */
    s_back_btn = make_btn(s_root, LV_SYMBOL_LEFT, back_cb, NULL);
    lv_obj_set_size(s_back_btn, 56, 40);
    lv_obj_set_pos(s_back_btn, GRID_MX, 46);
    hidden_if_changed(s_back_btn, true);

    s_search_btn = make_btn(s_root, "search...", search_cb, NULL);
    lv_obj_set_size(s_search_btn, 268, 40);
    lv_obj_set_pos(s_search_btn, GRID_MX + 62, 46);

    /* Named for what it does, not for its state: "all"/"selected" alone reads
     * as a label rather than a control, and this is the button you want when
     * removing tiles. */
    lv_obj_t *selbtn = make_btn(s_root, "Show: all", selected_cb, NULL);
    lv_obj_set_size(selbtn, 190, 40);
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
        lv_obj_set_width(s_row_name[r], 410);
        lv_obj_set_pos(s_row_name[r], 48, 10);

        s_row_type[r] = make_label(s_row[r], FONT_XS, COL_DIM);
        lv_obj_set_pos(s_row_type[r], 470, 13);

        s_row_val[r] = make_label(s_row[r], FONT_S, COL_TEXT);
        lv_obj_set_style_text_align(s_row_val[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_row_val[r], 120);
        lv_obj_set_pos(s_row_val[r], 580, 11);

        /*
         * A button inside the row, not a region of it. LVGL does not bubble
         * clicks by default, so the chevron consumes its own tap and the row
         * underneath does not also toggle the selection.
         */
        s_row_more[r] = make_btn(s_row[r], LV_SYMBOL_RIGHT, drill_cb,
                                 (void *)(intptr_t)r);
        lv_obj_set_size(s_row_more[r], 46, 34);
        lv_obj_set_pos(s_row_more[r], SCR_W - 2 * GRID_MX - 50, 2);
        lv_obj_set_style_bg_color(s_row_more[r], COL_PANEL_ALT, 0);

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
