/*
 * Streaming Prometheus text exposition parser. See prom_text.h.
 *
 * Grammar (text/plain;version=0.0.4, plus the OpenMetrics leniencies):
 *
 *   line    := blank | comment | meta | sample
 *   meta    := '#' WS ('HELP' WS name WS text | 'TYPE' WS name WS type | 'EOF')
 *   sample  := name [ '{' [label (',' label)* [',']] '}' ] WS value
 *              [ WS timestamp ] [ WS '#' exemplar ]
 *   label   := key WS* '=' WS* '"' escaped '"'
 */
#include "prom_text.h"
#include "prom_ident.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ parser */

struct prom_text_parser {
    const prom_text_sink_t *sink;
    void  *ctx;
    void *(*alloc)(size_t);
    void (*dealloc)(void *);

    char   *line;       /* PROM_LINE_MAX + 1 bytes */
    size_t  line_len;
    bool    overlong;   /* current line exceeded the buffer; drop it whole */
    bool    aborted;
    bool    saw_eof;    /* OpenMetrics "# EOF" terminator */

    /*
     * Single-entry # TYPE cache. A full hash map is unnecessary: the format
     * puts TYPE immediately before its family in every real exporter, and the
     * store owns the authoritative name->type map anyway. Keeping this
     * stateless-ish is what lets the parser stay host-testable.
     */
    char        type_name[PROM_NAME_MAX];
    uint16_t    type_name_len;
    prom_type_t type_cached;
    bool        have_type;

    prom_text_stats_t st;
};

/* ------------------------------------------------------------------ helpers */

static bool is_ws(char c)    { return c == ' ' || c == '\t'; }
static bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}
static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '.';
}

/* ---------------------------------------------------------- suffix handling */

typedef struct { const char *suf; uint8_t len; prom_role_t role; } suffix_t;

/*
 * Order matters only in that every entry is tried; the longest distinct
 * suffixes cannot collide with each other.
 *
 * _total is present so the TYPE HINT matches across dialects -- OpenMetrics
 * writes "# TYPE http_requests counter" for a sample named
 * http_requests_total, while text/0.0.4 writes the full name. Matching both
 * with one rule removes the need for a dialect flag.
 *
 * But _total is deliberately NOT stripped from base_name: users think of the
 * metric as http_requests_total, text/0.0.4 exporters name it that way, and a
 * counter's value sample IS the family. Only the histogram/summary sub-series
 * suffixes below produce a different base_name from their sample name.
 */
static const suffix_t k_suffixes[] = {
    { "_bucket",  7, PROM_ROLE_BUCKET  },
    { "_created", 8, PROM_ROLE_CREATED },
    { "_count",   6, PROM_ROLE_COUNT   },
    { "_sum",     4, PROM_ROLE_SUM     },
    { "_total",   6, PROM_ROLE_PLAIN   },
};

static bool ends_with(const char *s, size_t len, const char *suf, size_t suf_len)
{
    return len > suf_len && memcmp(s + len - suf_len, suf, suf_len) == 0;
}

/*
 * Decide the family name, role and type hint for a sample name.
 *
 * The type hint is applied ONLY when the sample name equals the cached TYPE
 * name, or equals it plus exactly one whitelisted suffix. That whitelist is
 * what stops "# TYPE foo gauge" from poisoning an unrelated foo_bar_baz that
 * happens to follow it in the stream.
 */
static void resolve_role(const prom_text_parser_t *p,
                         const char *name, size_t name_len,
                         const char **base, uint16_t *base_len,
                         prom_role_t *role, prom_type_t *type)
{
    *base     = name;
    *base_len = (uint16_t)name_len;
    *role     = PROM_ROLE_PLAIN;
    *type     = PROM_TYPE_UNTYPED;

    if (!p->have_type) return;

    /* Exact match: the sample IS the family's value sample. */
    if (name_len == p->type_name_len &&
        memcmp(name, p->type_name, name_len) == 0) {
        *type = p->type_cached;
        return;
    }

    for (size_t i = 0; i < sizeof(k_suffixes) / sizeof(k_suffixes[0]); i++) {
        const suffix_t *sx = &k_suffixes[i];
        if (!ends_with(name, name_len, sx->suf, sx->len)) continue;

        size_t stem_len = name_len - sx->len;
        if (stem_len != p->type_name_len ||
            memcmp(name, p->type_name, stem_len) != 0) continue;

        /* The stem matches the cached TYPE name, so the hint applies. */
        *type = p->type_cached;

        /* _bucket/_sum/_count only denote sub-series of an aggregate family.
         * On any other type they are just part of the metric's own name. */
        bool aggregate = (p->type_cached == PROM_TYPE_HISTOGRAM ||
                          p->type_cached == PROM_TYPE_GAUGEHISTOGRAM ||
                          p->type_cached == PROM_TYPE_SUMMARY);
        if (sx->role != PROM_ROLE_PLAIN && aggregate) {
            *role     = sx->role;
            *base     = name;
            *base_len = (uint16_t)stem_len;
        }
        return;
    }
}

/* ------------------------------------------------------------- sample lines */

static void handle_sample(prom_text_parser_t *p, char *s, size_t len)
{
    size_t i = 0;

    /* --- metric name --- */
    if (!is_name_start(s[0])) { p->st.err_bad_value++; return; }
    while (i < len && is_name_char(s[i])) i++;
    const char *name    = s;
    size_t      name_len = i;
    if (name_len == 0 || name_len > PROM_NAME_MAX) { p->st.err_bad_value++; return; }

    prom_sample_t smp;
    memset(&smp, 0, sizeof(smp));   /* PVAL_ABSENT == 0, so le/quantile/value
                                     * all start out correctly as "no value" */

    /* --- optional label set --- */
    /* The spec puts '{' hard against the name, but Prometheus's own parser
     * tolerates whitespace there and so do we -- skipping it here is free and
     * the value scan below re-skips whatever is left. */
    while (i < len && is_ws(s[i])) i++;
    if (i < len && s[i] == '{') {
        prom_lbl_res_t r = prom_scan_labels(s, len, &i, smp.labels,
                                            PROM_MAX_LABELS, &smp.n_labels,
                                            &smp.le, &smp.quantile);
        if (r == PROM_LBL_MALFORMED) { p->st.err_bad_label++; return; }
        if (r == PROM_LBL_TOO_MANY) {
            /* Dropping labels would silently change the series identity, so
             * the whole sample is skipped and counted instead. */
            p->st.err_too_many_labels++;
            return;
        }
    }

    /* --- value --- */
    while (i < len && is_ws(s[i])) i++;
    if (i >= len) { p->st.err_bad_value++; return; }
    size_t v0 = i;
    while (i < len && !is_ws(s[i])) i++;
    smp.value = prom_parse_value(s + v0, i - v0);
    if (prom_is_absent(smp.value)) { p->st.err_bad_value++; return; }

    /* --- optional timestamp, then an optional exemplar we discard --- */
    while (i < len && is_ws(s[i])) i++;
    if (i < len && s[i] != '#') {
        size_t t0 = i;
        while (i < len && !is_ws(s[i])) i++;
        prom_value_t ts = prom_parse_value(s + t0, i - t0);
        if (prom_is_num(ts)) { smp.has_ts = true; smp.ts_ms = (int64_t)ts.num; }
    }
    /*
     * Anything from here is an OpenMetrics exemplar (trace ids), which a panel
     * has no use for. Note the '#' can only be found AFTER the value token has
     * been consumed, which makes a '#' inside a label value structurally
     * unreachable as a comment marker -- no quoting logic, no exemplar buffer,
     * no allocation.
     */

    prom_sort_labels(smp.labels, smp.n_labels);

    smp.name     = name;
    smp.name_len = (uint16_t)name_len;
    resolve_role(p, name, name_len, &smp.base_name, &smp.base_len,
                 &smp.role, &smp.type);

    /* A quantile label on a summary's own name marks the quantile sub-series. */
    if (smp.role == PROM_ROLE_PLAIN && !prom_is_absent(smp.quantile) &&
        smp.type == PROM_TYPE_SUMMARY) {
        smp.role = PROM_ROLE_QUANTILE;
    }

    p->st.samples++;
    if (p->sink && p->sink->on_sample && !p->sink->on_sample(p->ctx, &smp)) {
        p->aborted = true;
    }
}

/* ------------------------------------------------------------- '#' lines */

static void handle_meta(prom_text_parser_t *p, char *s, size_t len)
{
    size_t i = 1;                                   /* past '#' */
    while (i < len && is_ws(s[i])) i++;

    size_t k0 = i;
    while (i < len && !is_ws(s[i])) i++;
    size_t k_len = i - k0;

    bool is_help = (k_len == 4 && memcmp(s + k0, "HELP", 4) == 0);
    bool is_type = (k_len == 4 && memcmp(s + k0, "TYPE", 4) == 0);
    bool is_eof  = (k_len == 3 && memcmp(s + k0, "EOF",  3) == 0);

    if (is_eof) { p->saw_eof = true; return; }
    if (!is_help && !is_type) { p->st.comments++; return; }

    while (i < len && is_ws(s[i])) i++;
    size_t n0 = i;
    while (i < len && !is_ws(s[i])) i++;
    size_t n_len = i - n0;
    if (n_len == 0 || n_len > PROM_NAME_MAX) { p->st.comments++; return; }

    while (i < len && is_ws(s[i])) i++;

    if (is_type) {
        p->st.type_lines++;
        prom_type_t t = prom_type_from_name(s + i, len - i);
        memcpy(p->type_name, s + n0, n_len);
        p->type_name_len = (uint16_t)n_len;
        p->type_cached   = t;
        p->have_type     = true;
        if (p->sink && p->sink->on_type) p->sink->on_type(p->ctx, s + n0, n_len, t);
        return;
    }

    /* HELP: the rest of the line, with \\ and \n decoded (the only two
     * escapes the spec defines for help text). */
    p->st.help_lines++;
    uint16_t h_len = prom_unescape_inplace(s + i, len - i);
    if (p->sink && p->sink->on_help) {
        p->sink->on_help(p->ctx, s + n0, n_len, s + i, h_len);
    }
}

static void handle_line(prom_text_parser_t *p, char *s, size_t len)
{
    p->st.lines++;

    if (len > 0 && s[len - 1] == '\r') len--;              /* CRLF bodies */
    while (len > 0 && is_ws(*s)) { s++; len--; }
    while (len > 0 && is_ws(s[len - 1])) len--;
    if (len == 0) return;

    if (*s == '#') handle_meta(p, s, len);
    else           handle_sample(p, s, len);
}

/* --------------------------------------------------------------- public API */

prom_text_parser_t *prom_text_new(const prom_text_sink_t *sink, void *ctx,
                                  void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (alloc == NULL)   alloc   = malloc;
    if (dealloc == NULL) dealloc = free;

    prom_text_parser_t *p = alloc(sizeof(*p));
    if (p == NULL) return NULL;
    memset(p, 0, sizeof(*p));

    p->line = alloc(PROM_LINE_MAX + 1);
    if (p->line == NULL) { dealloc(p); return NULL; }

    p->sink = sink; p->ctx = ctx; p->alloc = alloc; p->dealloc = dealloc;
    return p;
}

void prom_text_reset(prom_text_parser_t *p)
{
    if (p == NULL) return;
    p->line_len = 0;
    p->overlong = p->aborted = p->saw_eof = p->have_type = false;
    p->type_name_len = 0;
    memset(&p->st, 0, sizeof(p->st));
}

bool prom_text_feed(prom_text_parser_t *p, const char *data, size_t len)
{
    if (p == NULL || data == NULL) return false;
    if (p->aborted) return false;

    const char *cur = data, *end = data + len;
    p->st.bytes += len;

    while (cur < end) {
        const char *nl = memchr(cur, '\n', (size_t)(end - cur));
        size_t seg = nl ? (size_t)(nl - cur) : (size_t)(end - cur);

        if (p->line_len + seg <= PROM_LINE_MAX) {
            memcpy(p->line + p->line_len, cur, seg);
            p->line_len += seg;
        } else {
            /* Keep scanning for the newline but drop the bytes: an overlong
             * line costs you that one line, never the rest of the stream. */
            p->overlong = true;
        }

        if (nl == NULL) return true;    /* chunk ended mid-line; state persists */

        if (p->overlong) p->st.err_too_long++;
        else             handle_line(p, p->line, p->line_len);

        p->line_len = 0;
        p->overlong = false;
        cur = nl + 1;

        if (p->aborted) return false;
        if (p->saw_eof) return true;
    }
    return true;
}

void prom_text_finish(prom_text_parser_t *p, prom_text_stats_t *out)
{
    if (p == NULL) return;
    if (p->line_len > 0 && !p->overlong && !p->aborted && !p->saw_eof) {
        handle_line(p, p->line, p->line_len);   /* body with no trailing \n */
    }
    p->line_len = 0;
    p->overlong = false;
    if (out) *out = p->st;
}

void prom_text_free(prom_text_parser_t *p)
{
    if (p == NULL) return;
    void (*dealloc)(void *) = p->dealloc;
    dealloc(p->line);
    dealloc(p);
}
