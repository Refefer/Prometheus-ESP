/*
 * Core Prometheus data shapes.
 *
 * This header, and the whole `prom` component, deliberately depends on
 * NOTHING from ESP-IDF -- only the C standard library. That is what lets the
 * exposition parser, the rate math and the quantile math compile and run on a
 * development machine under ASan/UBSan against a corpus of real /metrics
 * captures. A format with this many edge cases is not something to debug by
 * flashing a board and squinting at a screen.
 */
#ifndef PROM_TYPES_H
#define PROM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ limits */

#define PROM_LINE_MAX       4096  /* node_exporter lines top out ~200B;
                                   * cAdvisor container_* reach 600-900B;
                                   * kube-state-metrics with annotation labels
                                   * ~2KB. 4KB is 2x the worst realistic case
                                   * and costs 4KB of PSRAM. */
#define PROM_MAX_LABELS       32
#define PROM_NAME_MAX        192
#define PROM_LABEL_KEY_MAX    96
#define PROM_LABEL_VAL_MAX   512
#define PROM_MAX_BUCKETS      64
#define PROM_MAX_QUANTILES    16

/* ------------------------------------------------------------------- value */

/*
 * A sample value and its kind, kept together so that "no value" is
 * structurally distinct from the number zero.
 *
 * Why this is not just a double: node_hwmon_temp_celsius on an absent sensor
 * is NaN, and a struct that decayed it to 0.0 would chart it as a real 0 C
 * reading. PVAL_ABSENT being 0 also means a memset(0) struct reads as "no
 * value", not as "exactly zero".
 */
typedef enum {
    PVAL_ABSENT = 0,   /* no value present -- the zero-initialised state */
    PVAL_NUM,          /* a finite number, in .num */
    PVAL_POS_INF,
    PVAL_NEG_INF,
    PVAL_NAN,
} pval_kind_t;

typedef struct {
    pval_kind_t kind;
    double      num;   /* meaningful only when kind == PVAL_NUM */
} prom_value_t;

static inline bool prom_is_num(prom_value_t v)    { return v.kind == PVAL_NUM; }
static inline bool prom_is_absent(prom_value_t v) { return v.kind == PVAL_ABSENT; }
static inline bool prom_is_inf(prom_value_t v)
{
    return v.kind == PVAL_POS_INF || v.kind == PVAL_NEG_INF;
}
static inline prom_value_t prom_num(double d)
{
    prom_value_t v = { PVAL_NUM, d };
    return v;
}
static inline prom_value_t prom_absent(void)
{
    prom_value_t v = { PVAL_ABSENT, 0.0 };
    return v;
}

/*
 * Parse a value token exactly as the exposition format and the HTTP API
 * encode it. Shared by the text parser and the JSON client so that both
 * sources produce byte-identical internal state -- the UI must not be able to
 * tell which one a series came from.
 *
 * The explicit Inf/NaN checks run before strtod because we want the
 * CLASSIFICATION, not merely the double: an le="+Inf" bucket bound has to be
 * recognised as infinite by the quantile code, not silently become HUGE_VAL.
 */
prom_value_t prom_parse_value(const char *s, size_t len);

/* -------------------------------------------------------------------- type */

typedef enum {
    PROM_TYPE_UNTYPED = 0,
    PROM_TYPE_COUNTER,
    PROM_TYPE_GAUGE,
    PROM_TYPE_HISTOGRAM,
    PROM_TYPE_SUMMARY,
    PROM_TYPE_INFO,        /* OpenMetrics; treated as a label carrier */
    PROM_TYPE_STATESET,    /* OpenMetrics */
    PROM_TYPE_GAUGEHISTOGRAM,
} prom_type_t;

const char *prom_type_name(prom_type_t t);
prom_type_t prom_type_from_name(const char *s, size_t len);

/*
 * Counters and rates are DIFFERENT TYPES, with no implicit conversion between
 * them, so a raw counter total cannot be charted by accident. The only
 * producer of prom_rate_t anywhere in the codebase is prom_rate_step().
 *
 * This is the type-level encoding of "a counter must be displayed as a rate":
 * showing the raw total is still possible, but it has to be asked for.
 */
typedef struct { double total;   } prom_counter_raw_t;
typedef struct { float  per_sec; } prom_rate_t;

/* -------------------------------------------------------------------- role */

/*
 * Where a sample sits within its metric family. Resolved once, at parse time,
 * from the name suffix -- so downstream code never re-derives it by string
 * matching.
 */
typedef enum {
    PROM_ROLE_PLAIN = 0,
    PROM_ROLE_BUCKET,     /* _bucket{le="..."} */
    PROM_ROLE_QUANTILE,   /* {quantile="..."} */
    PROM_ROLE_SUM,        /* _sum */
    PROM_ROLE_COUNT,      /* _count */
    PROM_ROLE_CREATED,    /* _created (OpenMetrics) */
} prom_role_t;

/* ------------------------------------------------------------------- label */

typedef struct {
    const char *key;
    uint16_t    key_len;
    const char *val;      /* escapes already decoded */
    uint16_t    val_len;
} prom_label_t;

/* ------------------------------------------------------------------ sample */

/*
 * One parsed sample line. All pointers alias the parser's line buffer and are
 * valid only for the duration of the on_sample callback -- the sink must copy
 * anything it wants to keep.
 *
 * `le` and `quantile` are prom_value_t rather than double precisely so that
 * "this is not a bucket" (PVAL_ABSENT) is structurally distinct from the
 * legitimate bucket bound le=0.
 */
typedef struct {
    const char  *name;        /* sample name as written, e.g. foo_bucket */
    uint16_t     name_len;
    const char  *base_name;   /* family name with the role suffix stripped */
    uint16_t     base_len;

    prom_type_t  type;        /* HINT from the last # TYPE; the store owns the
                               * authoritative name->type map and overrides it */
    prom_role_t  role;

    prom_label_t labels[PROM_MAX_LABELS];  /* sorted by key; le and quantile
                                            * removed and lifted out below */
    uint8_t      n_labels;

    prom_value_t value;
    prom_value_t le;          /* ABSENT unless role == PROM_ROLE_BUCKET */
    prom_value_t quantile;    /* ABSENT unless role == PROM_ROLE_QUANTILE */

    bool         has_ts;
    int64_t      ts_ms;       /* optional exposition timestamp, milliseconds */
} prom_sample_t;

/* ------------------------------------------------------------------- stats */

typedef struct {
    uint32_t lines, samples, help_lines, type_lines, comments;
    uint32_t err_too_long, err_bad_label, err_bad_value, err_too_many_labels;
    uint64_t bytes;
} prom_text_stats_t;

#ifdef __cplusplus
}
#endif
#endif /* PROM_TYPES_H */
