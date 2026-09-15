#include "prom_math.h"

#include <math.h>

rate_status_t prom_rate_step(rate_state_t *st, prom_value_t v, int64_t t_ms,
                             int64_t max_gap_ms, float *out_per_sec)
{
    if (!prom_is_num(v)) {
        /* NaN/Inf means the exporter has no reading. Disarm so the next real
         * sample re-baselines instead of differencing against a stale value
         * from before the gap. */
        st->armed = false;
        return RATE_NOT_FINITE;
    }

    if (!st->armed) {
        st->prev      = v.num;
        st->prev_t_ms = t_ms;
        st->armed     = true;
        /* No point emitted. The panel shows a dimmed em-dash and a "warming
         * up" chip -- a zero on the first poll is a lie that looks exactly
         * like a real reading. */
        return RATE_WARMING;
    }

    int64_t dt = t_ms - st->prev_t_ms;

    if (dt <= 0) {
        /* Duplicate scrape or clock skew. Keep the old baseline and do NOT
         * re-arm: re-arming on a repeated timestamp would stall the series
         * permanently, since every subsequent sample would also see dt <= 0
         * against the refreshed timestamp. */
        return RATE_BAD_DT;
    }

    if (dt > max_gap_ms) {
        st->prev      = v.num;
        st->prev_t_ms = t_ms;
        return RATE_GAP;
    }

    rate_status_t rc = RATE_OK;
    double delta;
    if (v.num < st->prev) {
        /*
         * Any decrease is a counter reset. Prometheus's increase() semantics:
         * the counter restarted at 0 and has since counted up to v, so the
         * delta for this interval is v itself.
         *
         * The point IS stored and flagged, not dropped -- "the process
         * restarted and has since served 40 requests" is real information,
         * and the chart draws a marker for it.
         */
        delta = v.num;
        rc    = RATE_RESET;
    } else {
        delta = v.num - st->prev;
    }

    if (out_per_sec) *out_per_sec = (float)(delta * 1000.0 / (double)dt);
    st->prev      = v.num;
    st->prev_t_ms = t_ms;
    return rc;
}

void prom_hist_repair(double *cum, int n)
{
    for (int i = 1; i < n; i++) {
        if (cum[i] < cum[i - 1]) cum[i] = cum[i - 1];
    }
}

/*
 * Index of the highest bucket with a finite bound.
 *
 * Walking back rather than assuming n-2 matters because a scrape can contain
 * more than one infinite bound -- a buggy exporter repeating a family, or a
 * proxy concatenating two scrapes. Taking n-2 blindly then returns infinity,
 * which propagates out as a non-finite "quantile" and shows on the panel as
 * no data at all rather than as a number.
 */
static int last_finite(const double *le, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (isfinite(le[i])) return i;
    }
    return -1;
}

double prom_hist_quantile(double q, const double *le, const double *cum, int n)
{
    if (n <= 0) return NAN;

    double total = cum[n - 1];          /* the +Inf bucket equals _count */
    if (!(total > 0.0)) return NAN;

    if (q <= 0.0) return le[0] < 0.0 ? le[0] : 0.0;
    if (q >= 1.0) {
        if (isfinite(le[n - 1])) return le[n - 1];
        int f = last_finite(le, n);
        return f >= 0 ? le[f] : NAN;
    }

    double rank = q * total;

    int i = 0;
    while (i < n && cum[i] < rank) i++;
    if (i >= n) i = n - 1;

    /* An unbounded bucket has nothing to interpolate towards. Report the
     * highest finite bound, which is the strongest true statement available:
     * "at least this much". */
    if (isinf(le[i])) {
        int f = last_finite(le, n);
        return f >= 0 ? le[f] : NAN;
    }

    /* Buckets may legitimately start below zero (rare, but some custom
     * histograms do it); the first bucket's lower bound is then le[0], not 0. */
    double lo  = (i == 0) ? (le[0] < 0.0 ? le[0] : 0.0) : le[i - 1];
    double clo = (i == 0) ? 0.0 : cum[i - 1];
    double hi  = le[i];
    double chi = cum[i];

    if (chi <= clo) return hi;          /* empty bucket; no slope to follow */
    return lo + (hi - lo) * (rank - clo) / (chi - clo);
}

double prom_hist_average(double d_sum, double d_count)
{
    if (!(d_count > 0.0)) return NAN;
    return d_sum / d_count;
}
