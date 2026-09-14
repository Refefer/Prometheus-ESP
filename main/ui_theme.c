#include "ui_theme.h"

#include <stddef.h>

/*
 * Palettes as packed 0xRRGGBB, converted to lv_color_t on demand and cached.
 *
 * RGB565 discipline: the panel is 5/6/5, so flat fills quantise invisibly but
 * gradients band badly. Nothing in this design uses a gradient -- which is
 * also, conveniently, the cheapest thing to render.
 *
 * Colour discipline: threshold colour goes on the VALUE TEXT, never on the
 * tile background. A wall of coloured rectangles is noise; three red numbers
 * among twelve grey ones is a signal you can read from the doorway. Only crit
 * escalates to chrome (a border plus a faint tint).
 */
typedef struct {
    const char *name;
    bool        is_dark;
    uint32_t    bg, panel, panel_alt, line;
    uint32_t    text, text_dim;
    uint32_t    accent;
    uint32_t    ok, warn, crit, stale, info;
    uint32_t    series[THEME_SERIES_N];
} theme_def_t;

static const theme_def_t k_themes[THEME_COUNT] = {
    [THEME_NIGHT_OPS] = {
        "Night Ops", true,
        0x0A0E14, 0x141A24, 0x1B2230, 0x263041,
        0xE6EDF6, 0x93A1B5, 0x4EA8FF,
        0x3FD08A, 0xFFC24B, 0xFF5C5C, 0x6B7A90, 0x9B8CFF,
        { 0x4EA8FF, 0x3FD08A, 0xFFC24B, 0xFF7A66,
          0xB58CFF, 0x2FD2D2, 0xFF8CD0, 0x9AD84E },
    },
    [THEME_DAYLIGHT] = {
        "Daylight", false,
        0xF2F5F9, 0xFFFFFF, 0xE8EDF4, 0xD2DAE5,
        0x101720, 0x5A6779, 0x1668D6,
        0x12905C, 0xB87400, 0xC62828, 0x97A3B2, 0x5B4BD6,
        { 0x1668D6, 0x0E8A56, 0xB87400, 0xC6452B,
          0x6C3FD1, 0x0E8C92, 0xB93A86, 0x4F7A12 },
    },
    [THEME_EMERALD] = {
        "Emerald", true,
        0x05140F, 0x0D241C, 0x123024, 0x17382C,
        0xDFF5EA, 0x6FA98F, 0x34D399,
        0x34D399, 0xE3C766, 0xFF6B6B, 0x4A6B5C, 0x7DD3FC,
        { 0x34D399, 0x5EEAD4, 0xE3C766, 0xFF8F6B,
          0xA78BFA, 0x38BDF8, 0xF472B6, 0xBEF264 },
    },
    [THEME_NORD] = {
        "Nord", true,
        0x2E3440, 0x3B4252, 0x434C5E, 0x4C566A,
        0xECEFF4, 0xA9B3C4, 0x88C0D0,
        0xA3BE8C, 0xEBCB8B, 0xBF616A, 0x6E7A8F, 0xB48EAD,
        { 0x88C0D0, 0xA3BE8C, 0xEBCB8B, 0xD08770,
          0xB48EAD, 0x8FBCBB, 0xD6A9C9, 0xC0D69B },
    },
    [THEME_AMBER] = {
        /* A night-shift phosphor look: almost no blue, so it is easy on the
         * eyes in a dark room without any actual dimming. */
        "Amber", true,
        0x0A0705, 0x17120C, 0x201811, 0x2A1F12,
        0xFFD79A, 0xA8752E, 0xFFB000,
        0xB4D000, 0xFF8A00, 0xFF3B1F, 0x6B4E22, 0xFFCF6B,
        { 0xFFB000, 0xB4D000, 0xFF8A00, 0xFF3B1F,
          0xFFCF6B, 0xD98E00, 0xFFE9B0, 0x8A6A1F },
    },
    [THEME_MONO] = {
        /* Maximum contrast for awkward viewing angles or bright rooms.
         * Severity is carried by weight and chrome rather than hue, so crit
         * gets the border treatment and warn is simply dimmer. */
        "Mono", true,
        0x000000, 0x141414, 0x1F1F1F, 0x2A2A2A,
        0xFFFFFF, 0x9A9A9A, 0xFFFFFF,
        0xFFFFFF, 0xB0B0B0, 0xFFFFFF, 0x666666, 0xD0D0D0,
        { 0xFFFFFF, 0xC8C8C8, 0x9A9A9A, 0xE4E4E4,
          0xB0B0B0, 0x808080, 0xF0F0F0, 0x6A6A6A },
    },
};

static theme_id_t  s_id    = THEME_NIGHT_OPS;
static app_theme_t s_cache;
static bool        s_valid;

static void rebuild(void)
{
    const theme_def_t *d = &k_themes[s_id];
    s_cache.bg        = lv_color_hex(d->bg);
    s_cache.panel     = lv_color_hex(d->panel);
    s_cache.panel_alt = lv_color_hex(d->panel_alt);
    s_cache.line      = lv_color_hex(d->line);
    s_cache.text      = lv_color_hex(d->text);
    s_cache.text_dim  = lv_color_hex(d->text_dim);
    s_cache.accent    = lv_color_hex(d->accent);
    s_cache.ok        = lv_color_hex(d->ok);
    s_cache.warn      = lv_color_hex(d->warn);
    s_cache.crit      = lv_color_hex(d->crit);
    s_cache.stale     = lv_color_hex(d->stale);
    s_cache.info      = lv_color_hex(d->info);
    for (int i = 0; i < THEME_SERIES_N; i++) {
        s_cache.series[i] = lv_color_hex(d->series[i]);
    }
    s_cache.is_dark = d->is_dark;
    s_valid = true;
}

const app_theme_t *app_theme(void)
{
    if (!s_valid) rebuild();
    return &s_cache;
}

theme_id_t app_theme_id(void) { return s_id; }

void app_theme_set(theme_id_t id)
{
    if (id >= THEME_COUNT) return;
    if (id == s_id && s_valid) return;
    s_id = id;
    rebuild();
    /* Callers repaint by rebuilding the widget tree from config -- see
     * ui_restyle(). Everything is config-driven, so that reuses the same
     * path as a page bind rather than needing a restyle hook per renderer. */
}

const char *app_theme_name(theme_id_t id)
{
    return (id < THEME_COUNT) ? k_themes[id].name : "";
}

const char *app_theme_options(void)
{
    /* Built once; lv_dropdown/lv_roller want a single newline-joined string. */
    static char buf[128];
    static bool built;
    if (!built) {
        size_t w = 0;
        for (int i = 0; i < THEME_COUNT; i++) {
            const char *n = k_themes[i].name;
            if (i > 0 && w + 1 < sizeof(buf)) buf[w++] = '\n';
            for (const char *p = n; *p && w + 1 < sizeof(buf); p++) buf[w++] = *p;
        }
        buf[w] = '\0';
        built = true;
    }
    return buf;
}

lv_color_t app_theme_sev(severity_t s)
{
    const app_theme_t *t = app_theme();
    switch (s) {
    case SEV_WARN:  return t->warn;
    case SEV_CRIT:  return t->crit;
    case SEV_STALE: return t->stale;
    case SEV_OK:
    default:        return t->text;
    }
}
