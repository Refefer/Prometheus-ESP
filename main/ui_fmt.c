#include "ui_fmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* prom_type_t values, mirrored so this file stays free of every other header
 * and therefore host-buildable. Kept in sync by ui_fmt_infer's callers. */
#define PT_UNTYPED    0
#define PT_COUNTER    1
#define PT_GAUGE      2
#define PT_HISTOGRAM  3
#define PT_SUMMARY    4
#define PT_INFO       5

/* Micro is rendered "u", not the real micro sign: LVGL's built-in Montserrat
 * has no 0xB5, and using "u" keeps the zero-custom-text-font story intact.
 * Prometheus itself writes "us" in metric names, so this is consistent. */
static const char *const k_si[]  = { "p", "n", "u", "m", "", "k", "M", "G", "T", "P" };
#define SI_ZERO 4                            /* index of the empty prefix */
#define SI_MIN  (-4)                         /* relative to SI_ZERO */
#define SI_MAX  5

static const char *const k_iec[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
#define IEC_MAX 5

/*
 * Boundary hysteresis. Stepping up only at 1000*1.05 and down only at
 * 1/1.05 means a value oscillating around a boundary settles instead of
 * flapping between "999" and "1.00k" on every poll.
 */
#define HYST_UP   1.05
#define HYST_DOWN (1.0 / 1.05)

static void safe_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Three significant digits: the decimal count is whatever leaves three
 * meaningful figures, clamped to [0,3]. */
static int decimals_for(double a)
{
    a = fabs(a);
    if (a >= 100.0) return 0;
    if (a >= 10.0)  return 1;
    if (a >= 1.0)   return 2;
    return 3;
}

/* As above, but a series that has only ever produced whole numbers prints as
 * a whole number -- see fmt_state_t.seen_fraction. */
static int decimals_tracked(double shown, fmt_state_t *st)
{
    if (st != NULL) {
        if (shown != floor(shown)) st->seen_fraction = true;
        if (!st->seen_fraction && fabs(shown) < 1e9) return 0;
    }
    return decimals_for(shown);
}

/*
 * Choose a prefix index for `v`, honouring the previous choice in `st`.
 * Returns the index; *scaled receives the value divided by that prefix.
 */
static int pick_exp(double v, double base, int lo, int hi,
                    fmt_state_t *st, double *scaled)
{
    double a = fabs(v);
    int e;

    if (a == 0.0 || !isfinite(a)) {
        e = (st && st->valid) ? st->exp : 0;     /* zero keeps the current
                                                  * prefix so a series that
                                                  * idles at 0 does not jump
                                                  * scale when it wakes */
        if (e < lo) e = lo;
        if (e > hi) e = hi;
    } else if (st && st->valid) {
        e = st->exp;
        if (e < lo) e = lo;
        if (e > hi) e = hi;
        /* Bounded walk: at most the full range, so no runaway on a NaN-ish
         * input that slipped through. */
        for (int guard = 0; guard < (hi - lo) + 2; guard++) {
            double s = a / pow(base, (double)e);
            if (s >= base * HYST_UP && e < hi)      { e++; continue; }
            if (s < HYST_DOWN && e > lo)            { e--; continue; }
            break;
        }
    } else {
        e = (int)floor(log(a) / log(base));
        if (e < lo) e = lo;
        if (e > hi) e = hi;
    }

    if (st) { st->exp = (int8_t)e; st->valid = true; }
    *scaled = v / pow(base, (double)e);
    return e;
}

void ui_fmt_duration(double seconds, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    if (!isfinite(seconds)) { safe_copy(out, cap, "--"); return; }

    double a = fabs(seconds);
    const char *sign = seconds < 0 ? "-" : "";

    /*
     * Always exactly two units, never three. "3d 4h 12m" is harder to read at
     * a glance than "3d 4h" and is never more actionable on a wall panel.
     */
    if (a < 1e-6)      snprintf(out, cap, "%s%.0f ns", sign, a * 1e9);
    else if (a < 1e-3) snprintf(out, cap, "%s%.0f us", sign, a * 1e6);
    else if (a < 1.0)  snprintf(out, cap, "%s%.0f ms", sign, a * 1e3);
    else if (a < 60.0) snprintf(out, cap, "%s%.*f s", sign, a < 10 ? 1 : 0, a);
    else if (a < 3600.0) {
        int m = (int)(a / 60), s = (int)a % 60;
        snprintf(out, cap, "%s%dm %02ds", sign, m, s);
    } else if (a < 86400.0) {
        int h = (int)(a / 3600), m = ((int)a % 3600) / 60;
        snprintf(out, cap, "%s%dh %02dm", sign, h, m);
    } else if (a < 7.0 * 86400.0) {
        int d = (int)(a / 86400), h = ((int)a % 86400) / 3600;
        snprintf(out, cap, "%s%dd %dh", sign, d, h);
    } else {
        snprintf(out, cap, "%s%dd", sign, (int)(a / 86400));
    }
}

void ui_fmt_value(double v, fmt_mode_t mode, const char *base_unit,
                  fmt_state_t *st,
                  char *num, size_t num_cap,
                  char *suffix, size_t suffix_cap,
                  bool *numeric_only)
{
    if (num && num_cap)       num[0] = '\0';
    if (suffix && suffix_cap) suffix[0] = '\0';
    if (numeric_only)         *numeric_only = true;

    /* Non-finite must never render as a number. A NaN gauge (an absent hwmon
     * sensor, say) showing "0" or "nan" both read as real data. */
    if (!isfinite(v)) {
        safe_copy(num, num_cap, isnan(v) ? "--" : (v > 0 ? "+Inf" : "-Inf"));
        if (numeric_only && isnan(v)) *numeric_only = true;
        else if (numeric_only) *numeric_only = false;   /* "Inf" needs letters */
        return;
    }

    switch (mode) {
    case FMT_BOOL:
        safe_copy(num, num_cap, v != 0.0 ? "UP" : "DOWN");
        if (numeric_only) *numeric_only = false;
        return;

    case FMT_DURATION: {
        char buf[32];
        ui_fmt_duration(v, buf, sizeof(buf));
        safe_copy(num, num_cap, buf);
        if (numeric_only) *numeric_only = false;   /* carries unit letters */
        return;
    }

    case FMT_PCT_01:
    case FMT_PCT_100: {
        double p = (mode == FMT_PCT_01) ? v * 100.0 : v;
        /* One decimal below 10%, none above. Never more than one: the extra
         * digit is noise at a glance and jitters constantly. */
        snprintf(num, num_cap, "%.*f", fabs(p) < 10.0 ? 1 : 0, p);
        safe_copy(suffix, suffix_cap, "%");
        return;
    }

    case FMT_IEC:
    case FMT_RATE_IEC: {
        double scaled = v;
        int e = pick_exp(v, 1024.0, 0, IEC_MAX, st, &scaled);
        int dp = decimals_tracked(scaled, st);
        if (e == 0) dp = 0;                       /* whole bytes, always */
        if (st) st->decimals = (int8_t)dp;
        snprintf(num, num_cap, "%.*f", dp, scaled);
        char u[16];
        snprintf(u, sizeof(u), "%s%s", k_iec[e],
                 mode == FMT_RATE_IEC ? "/s" : "");
        safe_copy(suffix, suffix_cap, u);
        return;
    }

    case FMT_SI:
    case FMT_RATE_SI: {
        double scaled = v;
        int e = pick_exp(v, 1000.0, SI_MIN, SI_MAX, st, &scaled);
        int dp = decimals_tracked(scaled, st);
        if (st) st->decimals = (int8_t)dp;
        snprintf(num, num_cap, "%.*f", dp, scaled);
        char u[16];
        snprintf(u, sizeof(u), "%s%s%s", k_si[e + SI_ZERO],
                 base_unit ? base_unit : "",
                 mode == FMT_RATE_SI ? "/s" : "");
        safe_copy(suffix, suffix_cap, u);
        return;
    }

    case FMT_RAW:
    case FMT_AUTO:
    default: {
        int dp = decimals_tracked(v, st);
        if (st) { st->decimals = (int8_t)dp; st->valid = true; st->exp = 0; }
        snprintf(num, num_cap, "%.*f", dp, v);
        safe_copy(suffix, suffix_cap, base_unit);
        return;
    }
    }
}

void ui_fmt_join(double v, fmt_mode_t mode, const char *base_unit,
                 char *out, size_t cap)
{
    char num[48], suf[24];
    bool numeric;
    ui_fmt_value(v, mode, base_unit, NULL, num, sizeof(num),
                 suf, sizeof(suf), &numeric);
    if (suf[0]) snprintf(out, cap, "%s %s", num, suf);
    else        safe_copy(out, cap, num);
}

void ui_fmt_axis(double v, fmt_mode_t mode, const char *base_unit,
                 char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (!isfinite(v)) { safe_copy(out, cap, ""); return; }

    /* Axis ticks get no hysteresis (they are recomputed wholesale with the
     * range) and are kept terse: a 4-char tick is all a 185px tile can spare. */
    switch (mode) {
    case FMT_DURATION:
        ui_fmt_duration(v, out, cap);
        return;
    case FMT_PCT_01:
        snprintf(out, cap, "%.0f%%", v * 100.0);
        return;
    case FMT_PCT_100:
        snprintf(out, cap, "%.0f%%", v);
        return;
    default: {
        char num[32], suf[24];
        bool numeric;
        ui_fmt_value(v, mode, base_unit, NULL, num, sizeof(num),
                     suf, sizeof(suf), &numeric);
        /* Drop a trailing ".00"/".0" so ticks line up in width. */
        char *dot = strchr(num, '.');
        if (dot) {
            char *end = num + strlen(num) - 1;
            while (end > dot && *end == '0') *end-- = '\0';
            if (end == dot) *end = '\0';
        }
        if (suf[0]) snprintf(out, cap, "%s%s", num, suf);
        else        safe_copy(out, cap, num);
        return;
    }
    }
}

int ui_fmt_delta_pct(double now, double before, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';

    if (!isfinite(now) || !isfinite(before) || before == 0.0) {
        safe_copy(out, cap, "--");
        return 0;
    }

    double pct = (now - before) / fabs(before) * 100.0;
    if (!isfinite(pct)) { safe_copy(out, cap, "--"); return 0; }

    /* Under half a percent is noise, not news. Showing "+0.1 %" on every poll
     * trains you to ignore the field entirely. */
    if (fabs(pct) < 0.5) { safe_copy(out, cap, "--"); return 0; }

    snprintf(out, cap, "%+.*f %%", fabs(pct) < 10.0 ? 1 : 0, pct);
    return pct > 0 ? 1 : -1;
}

/* ------------------------------------------------------- unit inference */

static bool ends(const char *s, size_t len, const char *suf)
{
    size_t n = strlen(suf);
    return len >= n && memcmp(s + len - n, suf, n) == 0;
}

bool ui_fmt_infer(const char *name, size_t name_len, int prom_type,
                  fmt_mode_t *mode_out, char *unit_out, size_t unit_cap,
                  agg_mode_t *agg_out)
{
    fmt_mode_t  mode = FMT_SI;
    agg_mode_t  agg  = AGG_LAST;
    const char *unit = "";

    if (name == NULL || name_len == 0) return false;

    /* *_info is a label carrier whose value is always 1. Offering it as a
     * panel is pure noise, so it is excluded from the browser entirely. */
    if (ends(name, name_len, "_info")) return false;

    bool is_counter = (prom_type == PT_COUNTER);
    /* A histogram/summary family's own _count is a counter too. */
    if (ends(name, name_len, "_count") &&
        (prom_type == PT_HISTOGRAM || prom_type == PT_SUMMARY)) {
        is_counter = true;
    }

    if (ends(name, name_len, "_bytes_total")) {
        mode = FMT_RATE_IEC; unit = ""; agg = AGG_RATE;
    } else if (ends(name, name_len, "_bytes")) {
        mode = FMT_IEC; unit = ""; agg = AGG_LAST;
    } else if (ends(name, name_len, "_seconds_total")) {
        /*
         * The rate of a seconds-counter is dimensionless: seconds of CPU per
         * second of wall clock is utilisation. node_cpu_seconds_total rated
         * gives 0..1 per core, which is a percentage, not "0.87 s/s".
         */
        mode = FMT_PCT_01; unit = ""; agg = AGG_RATE;
    } else if (ends(name, name_len, "_seconds") ||
               ends(name, name_len, "_duration_seconds")) {
        mode = FMT_DURATION; unit = ""; agg = AGG_LAST;
    } else if (ends(name, name_len, "_ratio")) {
        mode = FMT_PCT_01; unit = ""; agg = AGG_LAST;
    } else if (ends(name, name_len, "_percent")) {
        mode = FMT_PCT_100; unit = ""; agg = AGG_LAST;
    } else if (ends(name, name_len, "_celsius")) {
        mode = FMT_RAW; unit = "\xC2\xB0" "C"; agg = AGG_LAST;
    } else if (ends(name, name_len, "_volts"))   { mode = FMT_SI; unit = "V"; }
    else if (ends(name, name_len, "_amperes"))   { mode = FMT_SI; unit = "A"; }
    else if (ends(name, name_len, "_watts"))     { mode = FMT_SI; unit = "W"; }
    else if (ends(name, name_len, "_hertz"))     { mode = FMT_SI; unit = "Hz"; }
    else if (ends(name, name_len, "_joules_total")) {
        mode = FMT_RATE_SI; unit = "W"; agg = AGG_RATE;   /* J/s is watts */
    } else if (ends(name, name_len, "_meters"))  { mode = FMT_SI; unit = "m"; }
    else if (ends(name, name_len, "_grams"))     { mode = FMT_SI; unit = "g"; }
    else if (name_len == 2 && memcmp(name, "up", 2) == 0) {
        mode = FMT_BOOL; unit = ""; agg = AGG_LAST;
    } else if (ends(name, name_len, "_up")) {
        mode = FMT_BOOL; unit = ""; agg = AGG_LAST;
    } else if (is_counter || ends(name, name_len, "_total")) {
        mode = FMT_RATE_SI; unit = ""; agg = AGG_RATE;
    } else {
        mode = FMT_SI; unit = ""; agg = AGG_LAST;
    }

    /* A counter that did not match a unit-bearing suffix above still has to
     * be a rate -- a raw monotonic total is the one thing that is never worth
     * showing on a wall. */
    if (is_counter && agg != AGG_RATE) {
        agg = AGG_RATE;
        if (mode == FMT_SI)  mode = FMT_RATE_SI;
        if (mode == FMT_IEC) mode = FMT_RATE_IEC;
    }

    if (mode_out) *mode_out = mode;
    if (agg_out)  *agg_out  = agg;
    if (unit_out && unit_cap) safe_copy(unit_out, unit_cap, unit);
    return true;
}
