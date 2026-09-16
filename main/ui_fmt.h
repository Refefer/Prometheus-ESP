/*
 * Value formatting: SI/IEC prefixes, percentages, durations, rates, deltas,
 * and unit inference from Prometheus naming conventions.
 *
 * Deliberately free of LVGL and ESP-IDF types so it builds and runs on the
 * host -- see host/. The hysteresis rules below have boundary conditions that
 * are miserable to verify by watching a panel and pleasant to verify with a
 * table of assertions.
 */
#ifndef UI_FMT_H
#define UI_FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FMT_AUTO = 0,   /* resolved by ui_fmt_infer at selection time */
    FMT_RAW,        /* 3 significant digits, no prefix */
    FMT_SI,         /* 1000-based: n u m k M G T P */
    FMT_IEC,        /* 1024-based: B KiB MiB GiB TiB PiB */
    FMT_PCT_01,     /* source is a 0..1 ratio */
    FMT_PCT_100,    /* source is already 0..100 */
    FMT_DURATION,   /* seconds -> "820 ms" / "42.7 s" / "12m 30s" / "3d 4h" */
    FMT_RATE_SI,    /* SI + "/s" */
    FMT_RATE_IEC,   /* IEC + "/s" */
    FMT_BOOL,       /* 0/1 -> DOWN/UP */
    /*
     * A per-second rate shown as a per-hour quantity.
     *
     * The value on the wire is still per second -- this only changes the
     * scale and the suffix. It exists because "how many tokens went through
     * in the last hour" is a question about volume, and 1.2M/h answers it
     * where 333/s does not.
     */
    FMT_RATE_HOUR,
    FMT_MODE_COUNT,
} fmt_mode_t;

/*
 * A pinned SI/IEC prefix, or FMT_PIN_AUTO to let the value choose.
 *
 * Auto-scaling is the right default -- it keeps three significant digits
 * whatever the magnitude -- but it means a tile changes its unit as the value
 * moves, and a wall panel is read at a glance. If prefill sits near a
 * thousand, "847 tok/s" and "1.20 ktok/s" are the same reading wearing
 * different clothes, and telling them apart across the room takes a second
 * look. Pinning the prefix trades significant digits for a number whose scale
 * never moves.
 *
 * The value is the ladder exponent, and it means the same thing in both
 * ladders: 0 is no prefix, 1 is k or Ki, 2 is M or Mi. Negative steps below 1
 * exist only for SI and are clamped away for byte counts.
 */
#define FMT_PIN_AUTO ((int8_t)-128)

/* How a counter's samples become the displayed number. */
typedef enum {
    AGG_LAST = 0,   /* the value as scraped (gauges) */
    AGG_RATE,       /* per-second rate (counters) */
    AGG_DELTA,      /* change over the interval */
    AGG_AVG,        /* _sum/_count for aggregate families */
} agg_mode_t;

/*
 * Per-tile hysteresis memory. Zero-initialise for a fresh tile.
 *
 * Without this a value hovering near a boundary flaps every poll --
 * 999 -> 1.00k -> 999 -> 1.00k -- which is both ugly and, because every
 * change repaints, needlessly expensive.
 */
typedef struct {
    int8_t  exp;            /* current prefix index */
    int8_t  decimals;
    bool    valid;
    /*
     * Set once a fractional value has been seen, and never cleared.
     *
     * Request counts, queue depths and replica counts are integers, and
     * rendering them with three significant digits gives "3.00 req" and
     * "0.000 req", which reads as broken. Tracking integrality across samples
     * lets a genuinely integral series print as an integer while a
     * measurement that merely happens to land on a round number keeps its
     * decimals from the first fractional sample onward. It converges after
     * one sample and never oscillates, because the flag is one-way.
     */
    bool    seen_fraction;
} fmt_state_t;

/*
 * Format `v` into a numeric part and a unit suffix.
 *
 * The split exists because the large display faces are DIGITS ONLY (see
 * tools/gen_fonts.sh): `num` is guaranteed to contain nothing outside
 * " !%+,-./0-9:" whenever *numeric_only is true, so it is safe to render with
 * them. Durations set *numeric_only false -- "3d 4h" needs letters -- and the
 * caller must fall back to a text face for those.
 *
 * `st` may be NULL, which disables hysteresis (correct for one-shot
 * formatting like axis labels, wrong for a live tile).
 */
/*
 * How a panel wants its numbers written -- everything that shapes the text
 * without changing the quantity.
 *
 * A struct rather than four more parameters. The pin and the grouping flag
 * are both small scalars that would sit adjacent in the argument list, where
 * transposing them compiles cleanly and is invisible at the call site; and
 * every tile that renders a value also renders an axis or a quantile beside
 * it, which have to agree. Passing one thing makes disagreement hard.
 */
typedef struct {
    fmt_mode_t  mode;
    const char *unit;    /* base unit inside the ladder, e.g. "tok" -> "ktok" */
    int8_t      pin;     /* FMT_PIN_AUTO, or a fixed ladder exponent */
    bool        group;   /* thousands separators: 17,321 rather than 17321 */
    /*
     * Free text wrapped around the finished number -- "$" and "1,234" and
     * " EUR" -- for the labels the ladder cannot express. Distinct from
     * `unit`, which is part of the magnitude ("ktok/s") and moves with the
     * prefix; these do not.
     *
     * Either may be NULL. They ride in the numeric label with the digits, so
     * a symbol outside the digits-only charset drops the whole value to a
     * text face rather than vanishing -- the same fallback durations use.
     */
    const char *prefix;
    const char *suffix;
} fmt_style_t;

/* The plain default: auto prefix, no grouping, no unit. */
#define FMT_STYLE(m) ((fmt_style_t){ .mode = (m), .unit = "", \
                                     .pin = FMT_PIN_AUTO, .group = false, \
                                     .prefix = NULL, .suffix = NULL })

void ui_fmt_value(double v, const fmt_style_t *sy, fmt_state_t *st,
                  char *num, size_t num_cap,
                  char *suffix, size_t suffix_cap,
                  bool *numeric_only);

/* Convenience: num and suffix joined with a space, for logs and lists. */
void ui_fmt_join(double v, const fmt_style_t *sy, char *out, size_t cap);

/* Compact form for chart axis ticks: no hysteresis, 3 chars where possible.
 * Takes the whole style, so an axis cannot disagree with the value above it. */
void ui_fmt_axis(double v, const fmt_style_t *sy, char *out, size_t cap);

/*
 * The prefix a pin would produce, for labelling a chooser: "1", "k", "M" on
 * an SI ladder, "B", "KiB", "MiB" on a byte count. NULL when the format has
 * no ladder at all -- percentages, durations and booleans cannot be pinned,
 * and a chooser offering it would be lying.
 */
const char *ui_fmt_prefix_name(fmt_mode_t mode, int8_t e);

/*
 * True if every character can be drawn by the large digits-only faces.
 *
 * Those faces carry " !$%+,-./0-9:" plus the currency marks, and nothing
 * else; a glyph they lack draws as NOTHING rather than as a box, so a letter
 * reaching them makes the value disappear from the tile. This is the single
 * definition of that charset -- the generator script and the host sweep both
 * describe the same set, and this is what the formatter actually enforces.
 * UTF-8 aware, because the euro sign is three bytes.
 */
bool ui_fmt_digits_safe(const char *s);

/*
 * The bare-magnitude mode for a format: what an axis tick or a range endpoint
 * should be written in.
 *
 * Those describe a scale rather than a reading, so they carry no unit and no
 * "/s" -- but they must use the right base, since a byte axis stepping in
 * thousands next to a value in KiB is simply wrong. They must also not
 * re-apply a multiplier: the published value is already the displayed
 * quantity, so formatting it as a percent or a per-hour rate a second time
 * would scale it twice.
 */
fmt_mode_t ui_fmt_magnitude(fmt_mode_t m);

/* Seconds -> exactly two units, never three. */
void ui_fmt_duration(double seconds, char *out, size_t cap);

/*
 * Relative change, e.g. "+12.4 %" / "-0.8 %" / "--" when it is under 0.5%.
 * Returns +1 / -1 / 0 for the direction. The CALLER maps direction to colour
 * using the tile's polarity, because whether "up" is good depends entirely on
 * the metric and nothing here knows that.
 */
int ui_fmt_delta_pct(double now, double before, char *out, size_t cap);

/*
 * Infer format, unit and aggregation from a metric name and its Prometheus
 * type. This is what makes auto-discovery usable rather than merely possible:
 * a ticked metric gets a correct, labelled, correctly-united panel with no
 * further input.
 *
 * Returns false for names that should not become panels at all (*_info is a
 * label carrier that is always 1).
 */
bool ui_fmt_infer(const char *name, size_t name_len, int prom_type,
                  fmt_mode_t *mode_out, char *unit_out, size_t unit_cap,
                  agg_mode_t *agg_out);

#ifdef __cplusplus
}
#endif
#endif /* UI_FMT_H */
