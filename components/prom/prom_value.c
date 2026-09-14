/*
 * Value and type-name parsing, shared by the text exposition parser and the
 * HTTP API JSON client.
 */
#include "prom_types.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Case-insensitive compare of a length-delimited token against a literal. */
static bool tok_ieq(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    if (len != n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

prom_value_t prom_parse_value(const char *s, size_t len)
{
    prom_value_t out = { PVAL_ABSENT, 0.0 };
    if (s == NULL || len == 0) return out;

    /* Trim -- callers hand us tokens split on whitespace, but the API client
     * hands us JSON string contents which may carry stray spaces. */
    while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) return out;

    /*
     * Classify Inf/NaN BEFORE calling strtod. newlib's strtod also accepts
     * "inf"/"infinity" and would hand back HUGE_VAL, which is a finite double
     * as far as the rest of the code is concerned. The quantile math needs to
     * know that an le="+Inf" bucket bound is genuinely unbounded, so the kind
     * matters more than the number.
     */
    if (tok_ieq(s, len, "+inf") || tok_ieq(s, len, "inf") ||
        tok_ieq(s, len, "+infinity") || tok_ieq(s, len, "infinity")) {
        out.kind = PVAL_POS_INF;
        return out;
    }
    if (tok_ieq(s, len, "-inf") || tok_ieq(s, len, "-infinity")) {
        out.kind = PVAL_NEG_INF;
        return out;
    }
    if (tok_ieq(s, len, "nan") || tok_ieq(s, len, "+nan") || tok_ieq(s, len, "-nan")) {
        out.kind = PVAL_NAN;
        return out;
    }

    /*
     * strtod needs a NUL-terminated buffer and we are handed a slice of the
     * line buffer. A stack copy is cheaper and far safer than temporarily
     * writing a NUL into the caller's memory.
     *
     * 63 chars is comfortably more than any legal float64 text form (the
     * longest round-trippable decimal is ~24 chars); anything longer is
     * malformed and correctly rejected.
     */
    char buf[64];
    if (len >= sizeof(buf)) return out;
    memcpy(buf, s, len);
    buf[len] = '\0';

    errno = 0;
    char *end = NULL;
    double d = strtod(buf, &end);

    /* Reject trailing garbage ("1.5kb") and empty parses outright rather than
     * silently accepting the prefix -- a wrong number is worse than no
     * number on a panel someone reads at a glance. */
    if (end == buf || *end != '\0') return out;
    if (errno == ERANGE) {
        /* Underflow to zero is fine and common; overflow is not a finite
         * number and must not be presented as one. */
        if (d > 1.0 || d < -1.0) {
            out.kind = (d > 0) ? PVAL_POS_INF : PVAL_NEG_INF;
            return out;
        }
    }

    out.kind = PVAL_NUM;
    out.num  = d;
    return out;
}

const char *prom_type_name(prom_type_t t)
{
    switch (t) {
    case PROM_TYPE_COUNTER:         return "counter";
    case PROM_TYPE_GAUGE:           return "gauge";
    case PROM_TYPE_HISTOGRAM:       return "histogram";
    case PROM_TYPE_SUMMARY:         return "summary";
    case PROM_TYPE_INFO:            return "info";
    case PROM_TYPE_STATESET:        return "stateset";
    case PROM_TYPE_GAUGEHISTOGRAM:  return "gaugehistogram";
    case PROM_TYPE_UNTYPED:
    default:                        return "untyped";
    }
}

prom_type_t prom_type_from_name(const char *s, size_t len)
{
    if (tok_ieq(s, len, "counter"))        return PROM_TYPE_COUNTER;
    if (tok_ieq(s, len, "gauge"))          return PROM_TYPE_GAUGE;
    if (tok_ieq(s, len, "histogram"))      return PROM_TYPE_HISTOGRAM;
    if (tok_ieq(s, len, "summary"))        return PROM_TYPE_SUMMARY;
    if (tok_ieq(s, len, "info"))           return PROM_TYPE_INFO;
    if (tok_ieq(s, len, "stateset"))       return PROM_TYPE_STATESET;
    if (tok_ieq(s, len, "gaugehistogram")) return PROM_TYPE_GAUGEHISTOGRAM;
    /* "unknown" is the OpenMetrics spelling of text/0.0.4's "untyped" */
    return PROM_TYPE_UNTYPED;
}
