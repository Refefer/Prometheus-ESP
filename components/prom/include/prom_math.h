/*
 * Derived-value math: counter rates and histogram quantiles.
 *
 * Pure C, no ESP-IDF, host-testable -- which matters more here than anywhere
 * else in the codebase, because every one of these edge cases (a counter
 * reset, a poll gap, a non-monotonic bucket, an unbounded +Inf bound) shows up
 * on a real panel as a plausible-looking wrong number rather than as a crash.
 */
#ifndef PROM_MATH_H
#define PROM_MATH_H

#include "prom_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- rate math */

typedef enum {
    RATE_OK = 0,       /* *out holds a rate */
    RATE_WARMING,      /* first sample: baseline armed, NO point emitted */
    RATE_RESET,        /* counter decreased; *out holds the post-reset rate */
    RATE_GAP,          /* dt > max_gap: baseline re-armed, no point emitted */
    RATE_BAD_DT,       /* dt <= 0: baseline KEPT, no point emitted */
    RATE_NOT_FINITE,   /* NaN/Inf sample: baseline disarmed */
} rate_status_t;

typedef struct {
    double  prev;
    int64_t prev_t_ms;
    bool    armed;
} rate_state_t;

/*
 * Advance a counter's rate state by one sample.
 *
 * `prev` is a double, not a float, and that is load-bearing:
 * node_network_receive_bytes_total reaches 1e13, while float32's 24-bit
 * mantissa resolves only ~1.7e7. A float32 subtraction on any counter above
 * ~17 million yields EXACTLY ZERO, so the panel would read 0 B/s forever.
 * Doubles keep a delta of 1 exact up to 9e15.
 *
 * `t_ms` must come from a MONOTONIC clock (esp_timer_get_time()/1000), not
 * wall clock: it has to work before NTP has synced and must not jump when it
 * does. Wall clock is only ever used for chart axis labels.
 *
 * Recommended max_gap_ms is 3x the poll interval. Beyond that, averaging
 * across a ten-minute WiFi outage produces a technically correct but deeply
 * misleading rate; emitting nothing leaves an honest break in the chart.
 */
rate_status_t prom_rate_step(rate_state_t *st, prom_value_t v, int64_t t_ms,
                             int64_t max_gap_ms, float *out_per_sec);

/* ---------------------------------------------------------- histogram math */

/*
 * Clamp a cumulative bucket array to be monotonically non-decreasing.
 *
 * A scrape that races a concurrent observation can return counts where a
 * later bucket is smaller than an earlier one. Without this the interpolation
 * below divides by a negative denominator and produces a quantile outside its
 * own bucket.
 */
void prom_hist_repair(double *cum, int n);

/*
 * Quantile by linear interpolation within the crossing bucket.
 *
 * `le` must be ascending with any infinite bound last; pass INFINITY for the
 * "+Inf" bucket. `cum` holds cumulative counts (repaired). Returns NaN when
 * there is no data to interpolate.
 *
 * Feed it per-interval bucket DELTAS rather than the cumulative-since-boot
 * counts for a windowed quantile -- that is what
 * histogram_quantile(0.99, rate(x_bucket[5m])) means, and on a process up for
 * a month the all-time p99 is dominated by one bad afternoon and never moves.
 */
double prom_hist_quantile(double q, const double *le, const double *cum, int n);

/*
 * Average of an aggregate family over an interval: d_sum / d_count.
 * Returns NaN when d_count <= 0. Usually more actionable than a quantile and
 * costs one division.
 */
double prom_hist_average(double d_sum, double d_count);

#ifdef __cplusplus
}
#endif
#endif /* PROM_MATH_H */
