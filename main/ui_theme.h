/*
 * Runtime palettes.
 *
 * This board's backlight is a plain on/off line on the CH422G expander --
 * there is no PWM pin and no PWM peripheral -- so there is no brightness
 * control anywhere in this app by design. Themes are the answer to "it is too
 * bright / too cold in here" instead.
 */
#pragma once

#include "lvgl.h"

typedef enum {
    THEME_NIGHT_OPS = 0,
    THEME_DAYLIGHT,
    THEME_EMERALD,
    THEME_NORD,
    THEME_AMBER,
    THEME_MONO,
    THEME_COUNT,
} theme_id_t;

/* Severity, used for both threshold state and data freshness. */
typedef enum { SEV_OK = 0, SEV_WARN, SEV_CRIT, SEV_STALE, SEV_COUNT } severity_t;

#define THEME_SERIES_N 8

typedef struct {
    lv_color_t bg, panel, panel_alt, line;       /* surfaces */
    lv_color_t text, text_dim;                   /* type */
    lv_color_t accent;                           /* interactive */
    lv_color_t ok, warn, crit, stale, info;      /* semantic */
    lv_color_t series[THEME_SERIES_N];           /* categorical, for charts */
    bool       is_dark;
} app_theme_t;

const app_theme_t *app_theme(void);
theme_id_t         app_theme_id(void);
void               app_theme_set(theme_id_t id);
const char        *app_theme_name(theme_id_t id);
/* Newline-joined names, for lv_dropdown/lv_roller options. */
const char        *app_theme_options(void);

/* The stored spelling ("night_ops") and the lookup back from it. Matching is
 * loose over case and the separators, so both the slug and the display name
 * resolve -- a config written by hand should not have to guess. */
const char        *app_theme_slug(theme_id_t id);
theme_id_t         app_theme_from_name(const char *name);

/*
 * Rebuild the whole widget tree so a new theme takes effect.
 *
 * Implemented by the dashboard, declared here because the theme is the only
 * reason to call it. Everything on screen is built from config, so this
 * reuses the ordinary build path rather than needing a restyle hook on every
 * renderer -- which is why a palette can be added without touching a tile.
 */
void ui_restyle(void);

/* Colour for a severity, resolved against the current palette. */
lv_color_t app_theme_sev(severity_t s);

/* How a value-proportional indicator picks its colour. */
typedef enum {
    RAMP_NONE = 0,   /* one colour, from the panel's thresholds */
    RAMP_HEAT,       /* ok -> warn -> crit as the value rises */
    RAMP_COOL,       /* the reverse, for things where low is the problem */
    RAMP_SERIES,     /* stepped through the categorical palette */
    RAMP_COUNT,
} ramp_t;

/*
 * The colour for `frac` (0..1) of the way through a panel's range.
 *
 * Built from the theme's own semantic colours rather than a fixed gradient,
 * so a ramp follows the palette instead of fighting it -- Daylight's red is
 * not Night Ops' red, and a hard-coded spectrum would look wrong in both.
 */
lv_color_t app_theme_ramp(ramp_t r, float frac, lv_color_t fallback);

/*
 * Shorthand used throughout the UI files. These deliberately re-read
 * app_theme() on every use rather than caching a pointer, so a theme change
 * takes effect the moment the widget tree is rebuilt.
 */
#define COL_BG        (app_theme()->bg)
#define COL_PANEL     (app_theme()->panel)
#define COL_PANEL_ALT (app_theme()->panel_alt)
#define COL_LINE      (app_theme()->line)
#define COL_TEXT      (app_theme()->text)
#define COL_DIM       (app_theme()->text_dim)
#define COL_ACCENT    (app_theme()->accent)
#define COL_OK        (app_theme()->ok)
#define COL_WARN      (app_theme()->warn)
#define COL_CRIT      (app_theme()->crit)
#define COL_STALE     (app_theme()->stale)
#define COL_INFO      (app_theme()->info)
#define COL_SERIES(i) (app_theme()->series[(i) % THEME_SERIES_N])

/* ------------------------------------------------------------------- fonts */

/*
 * Text uses LVGL's built-in Montserrat throughout: its charset
 * (0x20-0x7F plus degree, bullet and the symbol glyphs) is the entire UI
 * charset, so there is no lv_font_conv step for text at all.
 *
 * The large numeric faces are digits-only and generated -- see fonts.h.
 */
#include "fonts.h"
#define FONT_XS  (&lv_font_montserrat_12)   /* chart ticks, badges */
#define FONT_S   (&lv_font_montserrat_14)   /* secondary lines, hints */
#define FONT_M   (&lv_font_montserrat_16)   /* body, list rows, buttons */
#define FONT_L   (&lv_font_montserrat_20)   /* header, section headings */
#define FONT_NUM_S (&lv_font_montserrat_24) /* multi-series rows, quantiles */
#define FONT_XL  (&lv_font_montserrat_28)   /* detail title, wizard headings */

#define FONT_NUM_M  (&font_num_44)   /* 1x1 tile value, gauge centre */
#define FONT_NUM_L  (&font_num_64)   /* 2x1 tile value */
#define FONT_NUM_XL (&font_num_96)   /* 2x2 hero value */
