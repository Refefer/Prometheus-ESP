/*
 * The tile renderers. One section each; all of them obey two rules:
 *
 *  - every write goes through a *_if_changed setter, because LVGL 8's style
 *    setters have no old-value compare and an unconditional write repaints;
 *  - no renderer does arithmetic on the metric. Values arrive formatted.
 */
#include "ui_layout.h"
#include "poller.h"
#include "ui_tile.h"
#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Charts map float -> int16 through a fixed 0..CHART_SPAN domain, because
 * lv_coord_t is int16 and LV_USE_LARGE_COORD would widen every coordinate in
 * every object just to serve a few charts. */
#define CHART_SPAN 1000

/* --------------------------------------------------------------- helpers */

/*
 * The largest face whose text actually fits the space it is given.
 *
 * The span alone is not enough to choose by. Pinning a prefix and turning on
 * thousands separators can turn a three-character reading into thirteen
 * ("2,836,857,472"), and a face chosen from the span alone then ellipses a
 * number into nonsense. Stepping down one rung is far better than that, and
 * the reader loses stroke weight rather than digits.
 */
static const lv_font_t *fit_font(const lv_font_t *start, const char *txt,
                                 bool numeric_only, lv_coord_t avail)
{
    static const lv_font_t *const num_ladder[] = {
        FONT_NUM_XL, FONT_NUM_L, FONT_NUM_M, FONT_NUM_S,
    };
    static const lv_font_t *const txt_ladder[] = { FONT_XL, FONT_L, FONT_M };

    const lv_font_t *const *lad = numeric_only ? num_ladder : txt_ladder;
    int n = numeric_only ? 4 : 3;
    if (txt == NULL || txt[0] == '\0' || avail <= 0) return start;

    int i = 0;
    while (i < n && lad[i] != start) i++;
    if (i == n) i = 0;                       /* not on the ladder: start high */

    for (; i < n; i++) {
        lv_coord_t w = lv_txt_get_width(txt, (uint32_t)strlen(txt), lad[i], 0,
                                        LV_TEXT_FLAG_NONE);
        if (w <= avail) return lad[i];
    }
    return lad[n - 1];                       /* smallest we have; let it clip */
}

static const lv_font_t *num_font(const tile_inst_t *t, bool numeric_only)
{
    /* Durations ("3d 4h") and booleans ("UP") carry letters, which the
     * digits-only faces cannot draw -- they would render as nothing. */
    if (!numeric_only) {
        return (t->spec->w >= 2 && t->spec->h >= 2) ? FONT_XL : FONT_L;
    }
    if (t->spec->w >= 2 && t->spec->h >= 2) return FONT_NUM_XL;
    if (t->spec->w >= 2)                    return FONT_NUM_L;
    return FONT_NUM_M;
}

/* Snap a range to a 1/2/5 x 10^n ladder so the axis lands on readable
 * numbers, with headroom so the line does not touch the frame. */
static void nice_range(const float *v, int n, float *lo_out, float *hi_out)
{
    if (n <= 0) { *lo_out = 0; *hi_out = 1; return; }

    float lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    if (lo > 0 && lo < hi * 0.5f) lo = 0;      /* prefer a zero baseline */
    if (hi == lo) { hi = lo + (lo == 0 ? 1.0f : fabsf(lo) * 0.1f); }

    float span = hi - lo;
    span *= 1.08f;                              /* headroom */

    float mag  = powf(10.0f, floorf(log10f(span)));
    float norm = span / mag;
    float step = (norm <= 1.0f) ? 1.0f : (norm <= 2.0f) ? 2.0f
               : (norm <= 5.0f) ? 5.0f : 10.0f;
    float nice = step * mag;

    *lo_out = floorf(lo / (nice / 4.0f)) * (nice / 4.0f);
    *hi_out = *lo_out + nice;
    if (*hi_out <= *lo_out) *hi_out = *lo_out + 1.0f;
}

typedef struct {
    lv_obj_t        *chart;
    lv_chart_series_t *ser;
    lv_obj_t        *val, *suf;
    float            lo, hi;
    int              stale_range;   /* samples the range has been too wide */
    /* The tile's pinned prefix, kept here because the axis is drawn from a
     * callback that has no access to the refresh data. An axis reading "k"
     * above a value reading plain units would be worse than no axis. */
    int8_t           scale;
    bool             group;
} chart_priv_t;

/* Refill the chart from the tile's history, rescaling if needed. */
static void chart_sync(tile_inst_t *t, chart_priv_t *p, int points)
{
    int n = t->hist_n < points ? t->hist_n : points;
    const float *src = t->hist + (t->hist_n - n);
    if (n <= 0) return;

    float lo, hi;
    nice_range(src, n, &lo, &hi);

    /*
     * Rescale only when a sample falls outside the current range, or when the
     * range has been more than 3x too wide for a while. Without the
     * hysteresis the axis moves on nearly every poll and the line jumps
     * around inside a tile that is only 128px tall.
     */
    bool outside = false;
    for (int i = 0; i < n; i++) {
        if (src[i] < p->lo || src[i] > p->hi) { outside = true; break; }
    }
    float used = hi - lo, have = p->hi - p->lo;
    if (have > 0 && used > 0 && have > used * 3.0f) p->stale_range++;
    else p->stale_range = 0;

    if (outside || have <= 0 || p->stale_range > 10) {
        p->lo = lo; p->hi = hi; p->stale_range = 0;
        lv_chart_set_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_SPAN);
    }

    float span = p->hi - p->lo;
    if (span <= 0) span = 1;

    lv_chart_set_point_count(p->chart, (uint16_t)points);
    for (int i = 0; i < points; i++) {
        lv_coord_t y;
        if (i < points - n) {
            y = LV_CHART_POINT_NONE;     /* not enough history yet: a gap,
                                          * not a line pinned to the floor */
        } else {
            float v = src[i - (points - n)];
            float f = (v - p->lo) / span;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            y = (lv_coord_t)(f * CHART_SPAN);
        }
        lv_chart_set_value_by_id(p->chart, p->ser, (uint16_t)i, y);
    }
    lv_chart_refresh(p->chart);
}

static lv_obj_t *make_chart(tile_inst_t *t, lv_obj_t *body, chart_priv_t *p,
                            int points, bool axes)
{
    p->chart = lv_chart_create(body);
    lv_chart_set_type(p->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(p->chart, (uint16_t)points);
    lv_chart_set_update_mode(p->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(p->chart, axes ? 3 : 0, 0);
    lv_obj_set_style_bg_opa(p->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->chart, 0, 0);
    lv_obj_set_style_pad_all(p->chart, 0, 0);
    lv_obj_set_style_line_color(p->chart, COL_LINE, LV_PART_MAIN);
    lv_obj_set_style_size(p->chart, 0, LV_PART_INDICATOR);   /* no point dots */
    lv_obj_clear_flag(p->chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(p->chart, LV_OBJ_FLAG_CLICKABLE);

    p->ser = lv_chart_add_series(p->chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    /* 3px: a 2px line is genuinely hard to see on this panel from across a
     * room, and the cost is nothing. */
    lv_obj_set_style_line_width(p->chart, 3, LV_PART_ITEMS);
    lv_chart_set_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_SPAN);
    return p->chart;
}

/* ------------------------------------------------------------- TILE_STAT */

typedef struct { lv_obj_t *val, *suf; } stat_priv_t;

static void stat_build(tile_inst_t *t, lv_obj_t *body)
{
    stat_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    p->val = make_label(body, FONT_NUM_M, COL_TEXT);
    lv_obj_set_pos(p->val, 0, 0);
    /*
     * Bounded, because a pinned prefix can be asked for a number far wider
     * than the tile -- pin a billion to plain units and it is ten digits. An
     * over-wide value ellipses, which reads as "too big to show here"; left
     * unbounded it is clipped mid-digit, which reads as a smaller number.
     * The ellipsis is three periods, which the digits-only face does have.
     *
     * Only this tile can do it: its unit label sits underneath at a fixed
     * position, where the chart and sparkline hang theirs off the right edge
     * of the number itself.
     */
    lv_obj_set_width(p->val, TILE_W(t->spec->w) - 2 * PAD_S);
    lv_label_set_long_mode(p->val, LV_LABEL_LONG_DOT);

    p->suf = make_label(body, FONT_M, COL_DIM);
    lv_obj_set_pos(p->suf, 0, TILE_H(t->spec->h) - 2 * PAD_S - 20 - 26);
}

static void stat_update(tile_inst_t *t, const tile_data_t *d)
{
    stat_priv_t *p = t->priv;
    const char *txt = d->valid ? d->num : "--";
    const lv_font_t *f = fit_font(num_font(t, d->numeric_only), txt,
                                  d->numeric_only,
                                  TILE_W(t->spec->w) - 2 * PAD_S);
    if (lv_obj_get_style_text_font(p->val, 0) != f) {
        lv_obj_set_style_text_font(p->val, f, 0);
    }
    label_set_if_changed(p->val, d->valid ? d->num : "--");
    label_set_if_changed(p->suf, d->valid ? d->suffix
                                          : (d->restarted ? "restarted"
                                             : d->warming ? "warming up" : "no data"));
    text_color_if_changed(p->val, d->valid ? app_theme_sev(t->last_sev) : COL_STALE);
}

static void stat_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_stat_vt = {
    "Big number", 1, 1, TILE_STAT, stat_build, stat_update, stat_destroy,
};

/* ------------------------------------------------------------ TILE_SPARK */

static void spark_build(tile_inst_t *t, lv_obj_t *body)
{
    chart_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t h = TILE_H(t->spec->h) - 2 * PAD_S - 20;

    p->val = make_label(body, num_font(t, true), COL_TEXT);
    /*
     * Left to size itself. The suffix is aligned to this label's right edge
     * (see the update below), so giving it a fixed width parks the unit
     * outside the tile and it vanishes -- which is exactly what a width bound
     * here did. The big-number tile can bound its value because its suffix
     * sits underneath at a fixed position; these cannot.
     */
    p->suf = make_label(body, FONT_M, COL_DIM);
    make_chart(t, body, p, 60, false);

    /*
     * A wide tile puts the sparkline beside the number, not under it: at 2x1
     * the body is only 96px tall and the 64px numeral would sit on top of the
     * chart. A square tile stacks them, with the numeral dropped to the
     * smaller face so the two do not collide there either.
     */
    if (t->spec->w >= 2) {
        lv_coord_t cw = w / 2 - 8;
        lv_obj_set_pos(p->val, 0, 6);
        lv_obj_set_size(p->chart, cw, h - 16);
        lv_obj_set_pos(p->chart, w - cw, 8);
    } else {
        lv_coord_t ch = h - 52;
        if (ch < 24) ch = 24;
        lv_obj_set_pos(p->val, 0, 0);
        lv_obj_set_size(p->chart, w, ch);
        lv_obj_set_pos(p->chart, 0, h - ch);
    }
}

static void spark_update(tile_inst_t *t, const tile_data_t *d)
{
    chart_priv_t *p = t->priv;
    const char *txt = d->valid ? d->num : "--";
    /* At 2x1 and wider the sparkline takes half the body, so the value only
     * gets the other half. */
    lv_coord_t body_w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t avail  = t->spec->w >= 2 ? body_w / 2 - 8 : body_w;
    const lv_font_t *f = fit_font(num_font(t, d->numeric_only), txt,
                                  d->numeric_only, avail);
    if (lv_obj_get_style_text_font(p->val, 0) != f) {
        lv_obj_set_style_text_font(p->val, f, 0);
    }
    label_set_if_changed(p->val, d->valid ? d->num : "--");
    text_color_if_changed(p->val, d->valid ? app_theme_sev(t->last_sev) : COL_STALE);

    /* The suffix sits to the right of the value, so it has to move as the
     * value's width changes. */
    label_set_if_changed(p->suf, d->valid ? d->suffix : "");
    lv_obj_align_to(p->suf, p->val, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

    chart_sync(t, p, 60);
}

static void chart_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_spark_vt = {
    "Sparkline", 1, 1, TILE_STAT, spark_build, spark_update, chart_destroy,
};

/* ------------------------------------------------------------ TILE_CHART */

/*
 * LVGL 8 prints raw chart values on the axis, which here would be our
 * internal 0..1000 domain. Rewriting dsc->text in LV_EVENT_DRAW_PART_BEGIN is
 * the only way to get real units on an axis in v8; the field is documented as
 * modifiable and text_length gives the buffer size.
 */
static void chart_tick_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class,
                                     LV_CHART_DRAW_PART_TICK_LABEL)) return;

    tile_inst_t *t = lv_event_get_user_data(e);
    chart_priv_t *p = t->priv;
    if (dsc->text == NULL) return;

    if (dsc->id == LV_CHART_AXIS_PRIMARY_Y) {
        float frac = (float)dsc->value / (float)CHART_SPAN;
        float v = p->lo + frac * (p->hi - p->lo);
        fmt_style_t sy = { FMT_SI, "", p->scale, p->group };
        ui_fmt_axis(v, &sy, dsc->text, (size_t)dsc->text_length);
    } else {
        dsc->text[0] = '\0';      /* x ticks are handled by the caption */
    }
}

static void chartt_build(tile_inst_t *t, lv_obj_t *body)
{
    chart_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t h = TILE_H(t->spec->h) - 2 * PAD_S - 20;

    p->val = make_label(body, FONT_NUM_L, COL_TEXT);
    /*
     * Left to size itself. The suffix is aligned to this label's right edge
     * (see the update below), so giving it a fixed width parks the unit
     * outside the tile and it vanishes -- which is exactly what a width bound
     * here did. The big-number tile can bound its value because its suffix
     * sits underneath at a fixed position; these cannot.
     */
    lv_obj_set_pos(p->val, 0, 0);
    p->suf = make_label(body, FONT_M, COL_DIM);

    /*
     * Vertical budget, explicitly: the value occupies the top ~64px, the
     * caption needs CAP_H at the bottom, and the chart gets what is left.
     * Positioning the caption at h-4 put its TOP 4px from the bottom edge, so
     * its whole height overflowed the body -- which clips, so it vanished.
     */
    const lv_coord_t CHART_TOP = 68;
    const lv_coord_t CAP_H     = 16;
    make_chart(t, body, p, TILE_HIST_MAX, true);
    lv_obj_set_size(p->chart, w - 48, h - CHART_TOP - CAP_H);
    lv_obj_set_pos(p->chart, 44, CHART_TOP);
    lv_chart_set_axis_tick(p->chart, LV_CHART_AXIS_PRIMARY_Y,
                           0, 0, 3, 1, true, 44);
    lv_obj_set_style_text_font(p->chart, FONT_XS, LV_PART_TICKS);
    lv_obj_set_style_text_color(p->chart, COL_DIM, LV_PART_TICKS);
    lv_obj_add_event_cb(p->chart, chart_tick_cb, LV_EVENT_DRAW_PART_BEGIN, t);

    lv_obj_t *cap = make_label(body, FONT_XS, COL_DIM);
    lv_label_set_text(cap, "last 10 min");
    lv_obj_set_pos(cap, 44, h - CAP_H + 1);
}

static void chartt_update(tile_inst_t *t, const tile_data_t *d)
{
    chart_priv_t *p = t->priv;
    p->scale = d->scale;
    p->group = d->group;
    const char *txt = d->valid ? d->num : "--";
    const lv_font_t *f = fit_font(d->numeric_only ? FONT_NUM_L : FONT_XL, txt,
                                  d->numeric_only,
                                  TILE_W(t->spec->w) - 2 * PAD_S);
    if (lv_obj_get_style_text_font(p->val, 0) != f) {
        lv_obj_set_style_text_font(p->val, f, 0);
    }
    label_set_if_changed(p->val, d->valid ? d->num : "--");
    text_color_if_changed(p->val, d->valid ? app_theme_sev(t->last_sev) : COL_STALE);
    label_set_if_changed(p->suf, d->valid ? d->suffix : "");
    lv_obj_align_to(p->suf, p->val, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);

    chart_sync(t, p, TILE_HIST_MAX);
}

const tile_vt_t tile_chart_vt = {
    "Chart", 2, 2, TILE_SPARK, chartt_build, chartt_update, chart_destroy,
};

/*
 * The range a ratio widget spans when the panel does not say.
 *
 * vmin/vmax are in the SOURCE domain, not the displayed one: a panel whose
 * format is "percent" is fed a 0..1 ratio, so its full arc is 1.0, and
 * setting vmax to 100 there pins the needle near zero while the number above
 * it reads 99%. Defaulting from the format gets the common cases right
 * without anyone having to know that.
 */
static float ratio_hi(const tile_inst_t *t, const tile_data_t *d)
{
    if (!isnan(t->spec->vmax)) return t->spec->vmax;

    /*
     * vmin/vmax are in the DISPLAYED domain, not the source one: the poller
     * publishes a 0..1 ratio as 0..100 for a percent panel, so full scale is
     * 100 whatever the metric underneath reads. Getting this backwards gave a
     * percent gauge a range of 1 against a value of 98, and an arc that was
     * simply always full.
     */
    if (d->fmt == FMT_PCT_01 || d->fmt == FMT_PCT_100) return 100.0f;

    /*
     * Otherwise the highest reading so far. A gauge needs a full scale and
     * there is often nowhere to look one up -- an inference server does not
     * export its own concurrency limit -- so the peak is the only number
     * available that means anything. Before the first sample it is 1, which
     * is what the old default was for everything.
     */
    return (d->peak > 0.0f) ? d->peak : 1.0f;
}

/* -------------------------------------------------------------- TILE_BAR */

typedef struct { lv_obj_t *bar, *val, *suf, *range; } bar_priv_t;

static void bar_build(tile_inst_t *t, lv_obj_t *body)
{
    bar_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;

    p->val = make_label(body, FONT_NUM_M, COL_TEXT);
    lv_obj_set_pos(p->val, 0, 0);
    p->suf = make_label(body, FONT_M, COL_DIM);

    p->bar = lv_bar_create(body);
    lv_obj_set_size(p->bar, w, 20);
    lv_obj_set_pos(p->bar, 0, TILE_H(t->spec->h) - 2 * PAD_S - 20 - 40);
    lv_bar_set_range(p->bar, 0, CHART_SPAN);
    lv_obj_set_style_bg_color(p->bar, COL_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->bar, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(p->bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(p->bar, 3, LV_PART_INDICATOR);

    p->range = make_label(body, FONT_XS, COL_DIM);
    lv_obj_set_pos(p->range, 0, TILE_H(t->spec->h) - 2 * PAD_S - 20 - 16);
}

static void bar_update(tile_inst_t *t, const tile_data_t *d)
{
    bar_priv_t *p = t->priv;
    const lv_font_t *f = num_font(t, d->numeric_only);
    if (lv_obj_get_style_text_font(p->val, 0) != f) {
        lv_obj_set_style_text_font(p->val, f, 0);
    }
    label_set_if_changed(p->val, d->valid ? d->num : "--");
    text_color_if_changed(p->val, d->valid ? app_theme_sev(t->last_sev) : COL_STALE);
    label_set_if_changed(p->suf, d->valid ? d->suffix : "");
    lv_obj_align_to(p->suf, p->val, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

    float lo = isnan(t->spec->vmin) ? 0.0f : t->spec->vmin;
    float hi = ratio_hi(t, d);
    if (hi <= lo) hi = lo + 1.0f;

    if (d->valid) {
        float frac = (d->value - lo) / (hi - lo);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int32_t want = (int32_t)(frac * CHART_SPAN);
        /* lv_bar restarts its animation on every set, even to the same value. */
        if (lv_bar_get_value(p->bar) != want) {
            lv_bar_set_value(p->bar, want, LV_ANIM_OFF);
        }
        lv_color_t c = app_theme_ramp((ramp_t)t->spec->ramp, frac,
                                      app_theme_sev(t->last_sev));
        bg_color_if_changed(p->bar, c);
        lv_obj_set_style_bg_color(p->bar, c, LV_PART_INDICATOR);
    }

    char a[24], b[24];
    fmt_style_t rsy = { FMT_SI, "", d->scale, d->group };
    ui_fmt_join(lo, &rsy, a, sizeof(a));
    ui_fmt_join(hi, &rsy, b, sizeof(b));
    label_set_fmt_if_changed(p->range, "%s  -  %s", a, b);
}

static void bar_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_bar_vt = {
    "Bar", 1, 1, TILE_STAT, bar_build, bar_update, bar_destroy,
};

/* ------------------------------------------------------------ TILE_GAUGE */

typedef struct { lv_obj_t *arc, *val, *suf; } gauge_priv_t;

static void gauge_build(tile_inst_t *t, lv_obj_t *body)
{
    gauge_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t h = TILE_H(t->spec->h) - 2 * PAD_S - 20;
    lv_coord_t d = (w < h ? w : h) - 4;

    /*
     * lv_arc, not lv_meter. A meter draws every tick as a rotated masked line
     * and lays out every tick label on each invalidation -- 31 masked draws
     * plus text layout per repaint, for twelve tiles on a PSRAM framebuffer.
     * An arc draws two strokes. lv_meter is worth it in the detail view,
     * where there is one and it repaints rarely.
     */
    p->arc = lv_arc_create(body);
    lv_obj_set_size(p->arc, d, d);
    lv_obj_align(p->arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_rotation(p->arc, 135);
    lv_arc_set_bg_angles(p->arc, 0, 270);
    lv_arc_set_range(p->arc, 0, CHART_SPAN);
    lv_arc_set_value(p->arc, 0);
    lv_obj_remove_style(p->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(p->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(p->arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->arc, COL_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_arc_color(p->arc, COL_ACCENT, LV_PART_INDICATOR);

    p->val = make_label(body, FONT_NUM_M, COL_TEXT);
    lv_obj_align(p->val, LV_ALIGN_CENTER, 0, -4);
    p->suf = make_label(body, FONT_XS, COL_DIM);
    lv_obj_align(p->suf, LV_ALIGN_CENTER, 0, 22);
}

static void gauge_update(tile_inst_t *t, const tile_data_t *d)
{
    gauge_priv_t *p = t->priv;
    label_set_if_changed(p->val, d->valid ? d->num : "--");
    text_color_if_changed(p->val, d->valid ? app_theme_sev(t->last_sev) : COL_STALE);

    /*
     * On an auto-ranged gauge the caption says what full scale is, because
     * otherwise the needle is a fraction of a number the reader cannot see --
     * "5" against an invisible maximum tells you nothing.
     */
    if (d->valid && isnan(t->spec->vmax) &&
        d->fmt != FMT_PCT_01 && d->fmt != FMT_PCT_100 && d->peak > 0.0f) {
        char pk[32], line[48];
        fmt_style_t psy = { d->fmt, d->unit ? d->unit : "", d->scale, d->group,
                            NULL, NULL };
        ui_fmt_join(d->peak, &psy, pk, sizeof(pk));
        snprintf(line, sizeof(line), "of %s", pk);
        label_set_if_changed(p->suf, line);
    } else {
        label_set_if_changed(p->suf, d->valid ? d->suffix : "");
    }
    lv_obj_align(p->val, LV_ALIGN_CENTER, 0, -4);
    lv_obj_align(p->suf, LV_ALIGN_CENTER, 0, 22);

    float lo = isnan(t->spec->vmin) ? 0.0f : t->spec->vmin;
    float hi = ratio_hi(t, d);
    if (hi <= lo) hi = lo + 1.0f;

    if (d->valid) {
        float frac = (d->value - lo) / (hi - lo);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int16_t want = (int16_t)(frac * CHART_SPAN);
        if (lv_arc_get_value(p->arc) != want) lv_arc_set_value(p->arc, want);
        /* A ramp colours by position in the range; without one the arc takes
         * the threshold colour, which on a panel with no thresholds set is a
         * single colour forever. */
        lv_obj_set_style_arc_color(p->arc,
            app_theme_ramp((ramp_t)t->spec->ramp, frac,
                           app_theme_sev(t->last_sev)),
            LV_PART_INDICATOR);
    }
}

static void gauge_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_gauge_vt = {
    "Gauge", 1, 1, TILE_STAT, gauge_build, gauge_update, gauge_destroy,
};

/* ----------------------------------------------------------- TILE_STATUS */

typedef struct { lv_obj_t *dot, *word, *sub; } status_priv_t;

static void status_build(tile_inst_t *t, lv_obj_t *body)
{
    status_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    /* A filled circle, not LV_SYMBOL_OK: the symbol glyphs live in the text
     * faces and the numeric faces are digits-only, so a large tick would have
     * to drag a whole text face along for one character. */
    p->dot = make_dot(body, 44, COL_OK);
    lv_obj_align(p->dot, LV_ALIGN_TOP_MID, 0, 2);

    p->word = make_label(body, FONT_L, COL_TEXT);
    lv_obj_align(p->word, LV_ALIGN_TOP_MID, 0, 52);

    p->sub = make_label(body, FONT_XS, COL_DIM);
    lv_obj_align(p->sub, LV_ALIGN_TOP_MID, 0, 76);
}

static void status_update(tile_inst_t *t, const tile_data_t *d)
{
    status_priv_t *p = t->priv;
    bool up = d->valid && d->value != 0.0f;

    bg_color_if_changed(p->dot, !d->valid ? COL_STALE : up ? COL_OK : COL_CRIT);
    label_set_if_changed(p->word, !d->valid ? "?" : up ? "UP" : "DOWN");
    text_color_if_changed(p->word, !d->valid ? COL_STALE : up ? COL_OK : COL_CRIT);
    label_set_if_changed(p->sub, d->valid ? "" : "no data");
    lv_obj_align(p->word, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_align(p->sub, LV_ALIGN_TOP_MID, 0, 76);
}

static void status_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_status_vt = {
    "Status", 1, 1, TILE_STAT, status_build, status_update, status_destroy,
};

/* ------------------------------------------------------------- TILE_HIST */

/*
 * A histogram shown as a histogram.
 *
 * Reducing one to a single p99 throws away the shape, which is usually the
 * interesting part -- a bimodal latency distribution and a smooth one can
 * share a p99 and mean completely different things. Bars are the
 * NON-cumulative share per bucket; the exposition format gives cumulative
 * counts, and drawing those is a monotonic ramp that tells you nothing.
 */
typedef struct {
    lv_obj_t *chart;
    lv_chart_series_t *ser;
    lv_obj_t *lo_lbl, *hi_lbl;
    lv_obj_t *q_lbl[3];
    lv_obj_t *q_val[3];
} hist_priv_t;

static void hist_build(tile_inst_t *t, lv_obj_t *body)
{
    hist_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t h = TILE_H(t->spec->h) - 2 * PAD_S - 20;

    /* Quantile rows take a fixed strip at the bottom; the bars get the rest. */
    const lv_coord_t QROW = 20;
    lv_coord_t qh = 3 * QROW;
    lv_coord_t ch = h - qh - 14;
    if (ch < 30) ch = 30;

    p->chart = lv_chart_create(body);
    lv_chart_set_type(p->chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(p->chart, 0, 0);
    lv_chart_set_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_SPAN);
    lv_obj_set_size(p->chart, w, ch);
    lv_obj_set_pos(p->chart, 0, 0);
    lv_obj_set_style_bg_opa(p->chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->chart, 0, 0);
    lv_obj_set_style_pad_all(p->chart, 0, 0);
    lv_obj_set_style_pad_column(p->chart, 2, LV_PART_ITEMS);
    lv_obj_clear_flag(p->chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(p->chart, LV_OBJ_FLAG_CLICKABLE);
    p->ser = lv_chart_add_series(p->chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    /* The bounds of the range the bars span, so the shape has a scale. */
    p->lo_lbl = make_label(body, FONT_XS, COL_DIM);
    lv_obj_set_pos(p->lo_lbl, 0, ch + 1);
    p->hi_lbl = make_label(body, FONT_XS, COL_DIM);
    lv_obj_set_style_text_align(p->hi_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(p->hi_lbl, w / 2);
    lv_obj_set_pos(p->hi_lbl, w - w / 2, ch + 1);

    static const char *const names[3] = { "p50", "p90", "p99" };
    for (int i = 0; i < 3; i++) {
        p->q_lbl[i] = make_label(body, FONT_XS, COL_DIM);
        lv_label_set_text(p->q_lbl[i], names[i]);
        lv_obj_set_pos(p->q_lbl[i], 0, ch + 14 + i * QROW);

        p->q_val[i] = make_label(body, FONT_S, COL_TEXT);
        lv_obj_set_pos(p->q_val[i], 40, ch + 14 + i * QROW);
    }
}

static void hist_update(tile_inst_t *t, const tile_data_t *d)
{
    hist_priv_t *p = t->priv;

    if (!d->valid || !d->has_hist || d->n_buckets == 0) {
        for (int i = 0; i < 3; i++) label_set_if_changed(p->q_val[i], "--");
        label_set_if_changed(p->lo_lbl, "");
        label_set_if_changed(p->hi_lbl, d->warming ? "warming up" : "no data");
        return;
    }

    /* Scale to the tallest bar rather than to 1.0: most buckets in a real
     * latency histogram hold a few percent, and a fixed 0..1 scale renders
     * them as invisible slivers. */
    float peak = 0;
    for (int i = 0; i < d->n_buckets; i++) {
        if (d->bucket_share[i] > peak) peak = d->bucket_share[i];
    }
    if (peak <= 0) peak = 1.0f;

    lv_chart_set_point_count(p->chart, d->n_buckets);
    for (int i = 0; i < d->n_buckets; i++) {
        lv_coord_t v = (lv_coord_t)((d->bucket_share[i] / peak) * CHART_SPAN);
        lv_chart_set_value_by_id(p->chart, p->ser, (uint16_t)i, v);
    }
    lv_chart_refresh(p->chart);

    char buf[32];
    fmt_style_t hsy = { d->fmt, d->unit ? d->unit : "", d->scale, d->group };
    ui_fmt_join(0.0, &hsy, buf, sizeof(buf));
    label_set_if_changed(p->lo_lbl, buf);

    /* The last bound is +Inf by construction, so label the highest finite
     * one -- "+Inf" as an axis label tells the reader nothing. */
    float hi = 0;
    for (int i = 0; i < d->n_buckets; i++) {
        if (isfinite(d->bucket_le[i])) hi = d->bucket_le[i];
    }
    ui_fmt_join(hi, &hsy, buf, sizeof(buf));
    label_set_if_changed(p->hi_lbl, buf);

    const float q[3] = { d->p50, d->p90, d->p99 };
    for (int i = 0; i < 3; i++) {
        if (isfinite(q[i])) {
            ui_fmt_join(q[i], &hsy, buf, sizeof(buf));
            label_set_if_changed(p->q_val[i], buf);
            text_color_if_changed(p->q_val[i], COL_TEXT);
        } else {
            label_set_if_changed(p->q_val[i], "--");
            text_color_if_changed(p->q_val[i], COL_STALE);
        }
    }
}

static void hist_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_hist_vt = {
    /* Needs a 2x2: three quantile rows plus bars will not fit in 128px, and
     * the fallback to a plain p99 number is the honest degradation. */
    "Histogram", 2, 2, TILE_STAT, hist_build, hist_update, hist_destroy,
};

/* ------------------------------------------------------------ TILE_MULTI */

/*
 * One metric across several label sets, as ranked bars.
 *
 * Deliberately bars and not small multiples: per-CPU or per-mode is
 * fundamentally a COMPARISON, and sorted bars answer "which one is hot"
 * instantly from across a room, where eight tiny sparklines answer nothing at
 * that distance.
 *
 * Row order is held steady for about a minute by the poller. Re-ranking every
 * poll makes rows leapfrog continuously and you cannot follow one long enough
 * to read it.
 */
#define MULTI_ROW_H 22

typedef struct {
    lv_obj_t *name[POLLER_ROWS_MAX];
    lv_obj_t *val[POLLER_ROWS_MAX];
    lv_obj_t *bar[POLLER_ROWS_MAX];
    lv_obj_t *more;
    int       rows;
} multi_priv_t;

static void multi_build(tile_inst_t *t, lv_obj_t *body)
{
    multi_priv_t *p = lv_mem_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    t->priv = p;

    lv_coord_t w = TILE_W(t->spec->w) - 2 * PAD_S;
    lv_coord_t h = TILE_H(t->spec->h) - 2 * PAD_S - 20;

    p->rows = (h - 14) / MULTI_ROW_H;
    if (p->rows > POLLER_ROWS_MAX) p->rows = POLLER_ROWS_MAX;
    if (p->rows < 1) p->rows = 1;

    lv_coord_t name_w = w < 260 ? 74 : 110;
    lv_coord_t val_w  = w < 260 ? 54 : 70;
    lv_coord_t bar_x  = name_w + val_w + 8;
    lv_coord_t bar_w  = w - bar_x;
    if (bar_w < 20) bar_w = 20;

    for (int r = 0; r < p->rows; r++) {
        lv_coord_t y = r * MULTI_ROW_H;

        p->name[r] = make_label(body, FONT_S, COL_DIM);
        lv_label_set_long_mode(p->name[r], LV_LABEL_LONG_DOT);
        lv_obj_set_width(p->name[r], name_w);
        lv_obj_set_pos(p->name[r], 0, y + 2);

        p->val[r] = make_label(body, FONT_S, COL_TEXT);
        lv_obj_set_style_text_align(p->val[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(p->val[r], val_w);
        lv_obj_set_pos(p->val[r], name_w, y + 2);

        p->bar[r] = lv_bar_create(body);
        lv_obj_set_size(p->bar[r], bar_w, 8);
        lv_obj_set_pos(p->bar[r], bar_x, y + 7);
        lv_bar_set_range(p->bar[r], 0, CHART_SPAN);
        lv_obj_set_style_bg_color(p->bar[r], COL_PANEL_ALT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(p->bar[r], COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_radius(p->bar[r], 2, LV_PART_MAIN);
        lv_obj_set_style_radius(p->bar[r], 2, LV_PART_INDICATOR);
    }

    p->more = make_label(body, FONT_XS, COL_DIM);
    lv_obj_set_pos(p->more, 0, p->rows * MULTI_ROW_H + 1);
}

static void multi_update(tile_inst_t *t, const tile_data_t *d)
{
    multi_priv_t *p = t->priv;

    if (!d->valid || d->n_children == 0) {
        for (int r = 0; r < p->rows; r++) {
            hidden_if_changed(p->name[r], true);
            hidden_if_changed(p->val[r], true);
            hidden_if_changed(p->bar[r], true);
        }
        label_set_if_changed(p->more, d->warming ? "warming up" : "no data");
        return;
    }

    /* Scale to the largest child, so the comparison fills the tile whatever
     * the absolute magnitudes happen to be. */
    float peak = 0;
    for (int i = 0; i < d->n_children; i++) {
        if (d->child_value[i] > peak) peak = d->child_value[i];
    }
    if (peak <= 0) peak = 1.0f;

    for (int r = 0; r < p->rows; r++) {
        if (r >= d->n_children) {
            hidden_if_changed(p->name[r], true);
            hidden_if_changed(p->val[r], true);
            hidden_if_changed(p->bar[r], true);
            continue;
        }
        hidden_if_changed(p->name[r], false);
        hidden_if_changed(p->val[r], false);
        hidden_if_changed(p->bar[r], false);

        int i = d->child_order ? d->child_order[r] : r;
        if (i >= d->n_children) i = r;

        label_set_if_changed(p->name[r], d->child_label[i]);
        label_set_if_changed(p->val[r], d->child_num[i]);

        int32_t want = (int32_t)((d->child_value[i] / peak) * CHART_SPAN);
        if (lv_bar_get_value(p->bar[r]) != want) {
            lv_bar_set_value(p->bar[r], want, LV_ANIM_OFF);
        }
    }

    if (d->n_matched > d->n_children) {
        label_set_fmt_if_changed(p->more, "+%u more",
                                 (unsigned)(d->n_matched - d->n_children));
    } else {
        label_set_if_changed(p->more, "");
    }
}

static void multi_destroy(tile_inst_t *t) { lv_mem_free(t->priv); t->priv = NULL; }

const tile_vt_t tile_multi_vt = {
    /* Needs the width for name + value + bar on one row. */
    "Multi-series", 2, 1, TILE_STAT, multi_build, multi_update, multi_destroy,
};
