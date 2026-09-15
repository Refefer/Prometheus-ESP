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
void ui_fmt_value(double v, fmt_mode_t mode, const char *base_unit,
                  fmt_state_t *st,
                  char *num, size_t num_cap,
                  char *suffix, size_t suffix_cap,
                  bool *numeric_only);

/* Convenience: num and suffix joined with a space, for logs and lists. */
void ui_fmt_join(double v, fmt_mode_t mode, const char *base_unit,
                 char *out, size_t cap);

/* Compact form for chart axis ticks: no hysteresis, 3 chars where possible. */
void ui_fmt_axis(double v, fmt_mode_t mode, const char *base_unit,
                 char *out, size_t cap);

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
