/*
 * Host tests for the `prom` component.
 *
 * The headline test is replay equivalence: parsing a corpus file as one big
 * buffer must produce a byte-identical event stream to parsing it one byte at
 * a time. That is the direct regression test for the chunk-boundary logic in
 * prom_text_feed, and it is worth more than any amount of on-device debugging
 * -- a real HTTP body arrives in arbitrary chunks, and a token split across
 * two of them is the failure nobody reproduces by hand.
 */
#include "prom_text.h"
#include "prom_ident.h"
#include "prom_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static int g_fail = 0;
static int g_ran  = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_ran++;                                                              \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* ----------------------------------------------------- event-stream digest */

typedef struct {
    char  *buf;
    size_t len, cap;
} digest_t;

static void dig_add(digest_t *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void dig_add(digest_t *d, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    size_t need = d->len + (size_t)n + 1;
    if (need > d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 4096;
        while (nc < need) nc *= 2;
        d->buf = realloc(d->buf, nc);
        d->cap = nc;
    }
    memcpy(d->buf + d->len, tmp, (size_t)n);
    d->len += (size_t)n;
    d->buf[d->len] = '\0';
}

static const char *role_name(prom_role_t r)
{
    switch (r) {
    case PROM_ROLE_BUCKET:   return "bucket";
    case PROM_ROLE_QUANTILE: return "quantile";
    case PROM_ROLE_SUM:      return "sum";
    case PROM_ROLE_COUNT:    return "count";
    case PROM_ROLE_CREATED:  return "created";
    default:                 return "plain";
    }
}

static void fmt_val(char *out, size_t cap, prom_value_t v)
{
    switch (v.kind) {
    case PVAL_ABSENT:  snprintf(out, cap, "absent");   break;
    case PVAL_POS_INF: snprintf(out, cap, "+Inf");     break;
    case PVAL_NEG_INF: snprintf(out, cap, "-Inf");     break;
    case PVAL_NAN:     snprintf(out, cap, "NaN");      break;
    case PVAL_NUM:     snprintf(out, cap, "%.17g", v.num); break;
    }
}

static void on_help(void *ctx, const char *n, size_t nl, const char *h, size_t hl)
{
    dig_add((digest_t *)ctx, "HELP|%.*s|%.*s\n", (int)nl, n, (int)hl, h);
}

static void on_type(void *ctx, const char *n, size_t nl, prom_type_t t)
{
    dig_add((digest_t *)ctx, "TYPE|%.*s|%s\n", (int)nl, n, prom_type_name(t));
}

static bool on_sample(void *ctx, const prom_sample_t *s)
{
    digest_t *d = (digest_t *)ctx;
    char rendered[1024];
    char v[64], le[64], q[64];

    size_t rn = prom_render(rendered, sizeof(rendered), s->name, s->name_len,
                            s->labels, s->n_labels);
    fmt_val(v,  sizeof(v),  s->value);
    fmt_val(le, sizeof(le), s->le);
    fmt_val(q,  sizeof(q),  s->quantile);

    dig_add(d, "S|%s|base=%.*s|type=%s|role=%s|v=%s|le=%s|q=%s|ts=%s:%lld\n",
            rn ? rendered : "<render-overflow>",
            (int)s->base_len, s->base_name,
            prom_type_name(s->type), role_name(s->role),
            v, le, q, s->has_ts ? "y" : "n", (long long)s->ts_ms);
    return true;
}

static const prom_text_sink_t k_sink = { on_help, on_type, on_sample };

/* ------------------------------------------------------------ replay tests */

static char *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    *len_out = rd;
    return buf;
}

/* Parse the whole body in chunks of `chunk` bytes (0 == one single feed). */
static void replay(const char *body, size_t len, size_t chunk,
                   digest_t *d, prom_text_stats_t *st)
{
    prom_text_parser_t *p = prom_text_new(&k_sink, d, NULL, NULL);
    if (chunk == 0) {
        prom_text_feed(p, body, len);
    } else {
        for (size_t off = 0; off < len; off += chunk) {
            size_t n = len - off < chunk ? len - off : chunk;
            if (!prom_text_feed(p, body + off, n)) break;
        }
    }
    prom_text_finish(p, st);
    prom_text_free(p);
}

static void test_corpus(const char *path)
{
    size_t len = 0;
    char *body = slurp(path, &len);
    if (!body) { g_fail++; return; }

    digest_t whole = {0}, single = {0}, odd = {0};
    prom_text_stats_t s_whole, s_single, s_odd;

    replay(body, len, 0, &whole,  &s_whole);
    replay(body, len, 1, &single, &s_single);
    replay(body, len, 7, &odd,    &s_odd);   /* 7 is coprime with everything
                                              * interesting, so tokens land
                                              * across boundaries at odd spots */

    printf("  %-28s %5u samples, %3u help, %3u type, %3u comments, "
           "errs: len=%u lbl=%u val=%u nlbl=%u\n",
           path, s_whole.samples, s_whole.help_lines, s_whole.type_lines,
           s_whole.comments, s_whole.err_too_long, s_whole.err_bad_label,
           s_whole.err_bad_value, s_whole.err_too_many_labels);

    g_ran++;
    if (whole.len != single.len || memcmp(whole.buf, single.buf, whole.len) != 0) {
        g_fail++;
        printf("  FAIL %s: byte-at-a-time replay differs from whole-buffer\n", path);
        /* Print the first differing line so the failure is actionable. */
        size_t i = 0, min = whole.len < single.len ? whole.len : single.len;
        while (i < min && whole.buf[i] == single.buf[i]) i++;
        size_t ls = i;
        while (ls > 0 && whole.buf[ls - 1] != '\n') ls--;
        printf("    whole : %.120s\n", whole.buf + ls);
        printf("    single: %.120s\n", single.buf + (ls < single.len ? ls : 0));
    }

    g_ran++;
    if (whole.len != odd.len || memcmp(whole.buf, odd.buf, whole.len) != 0) {
        g_fail++;
        printf("  FAIL %s: 7-byte-chunk replay differs from whole-buffer\n", path);
    }

    free(whole.buf); free(single.buf); free(odd.buf); free(body);
}

/* -------------------------------------------------------------- unit tests */

static prom_value_t pv(const char *s) { return prom_parse_value(s, strlen(s)); }

static void test_values(void)
{
    printf("value parsing\n");
    CHECK(pv("42").kind == PVAL_NUM && pv("42").num == 42.0, "plain int");
    CHECK(pv("1.5e-9").kind == PVAL_NUM, "scientific");
    CHECK(pv("+Inf").kind == PVAL_POS_INF, "+Inf");
    CHECK(pv("inf").kind == PVAL_POS_INF, "bare inf");
    CHECK(pv("-Inf").kind == PVAL_NEG_INF, "-Inf");
    CHECK(pv("NaN").kind == PVAL_NAN, "NaN");
    CHECK(pv("nan").kind == PVAL_NAN, "lowercase nan");
    CHECK(pv("").kind == PVAL_ABSENT, "empty");
    CHECK(pv("  ").kind == PVAL_ABSENT, "whitespace only");
    /* Trailing garbage must be rejected outright: silently accepting the
     * numeric prefix of "1.5kb" would put a confidently wrong number on a
     * panel, which is worse than showing nothing. */
    CHECK(pv("1.5kb").kind == PVAL_ABSENT, "trailing garbage rejected");
    CHECK(pv("abc").kind == PVAL_ABSENT, "non-numeric");
    CHECK(pv("-0").kind == PVAL_NUM, "negative zero");
    /* A counter big enough that float32 could not difference it. */
    CHECK(pv("12345678901234").num == 12345678901234.0, "1e13 exact in double");
}

static void test_ident(void)
{
    printf("identity round-trip\n");
    char scratch[1024], rendered[1024], canon[1024];
    const char *name; uint16_t nlen; prom_label_t lbl[PROM_MAX_LABELS]; uint8_t n;

    const char *cases[] = {
        "node_load1",
        "node_cpu_seconds_total{cpu=\"0\",mode=\"idle\"}",
        "m{b=\"2\",a=\"1\"}",                      /* unsorted input */
        "esc{path=\"a\\\\b\",quote=\"say \\\"hi\\\"\"}",
        "empty{}",
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool ok = prom_parse_selector(cases[i], scratch, sizeof(scratch),
                                      &name, &nlen, lbl, PROM_MAX_LABELS, &n);
        CHECK(ok, "parse_selector(%s)", cases[i]);
        if (!ok) continue;

        size_t rl = prom_render(rendered, sizeof(rendered), name, nlen, lbl, n);
        CHECK(rl > 0, "render(%s)", cases[i]);

        prom_series_id_t id1 = prom_series_id(name, nlen, lbl, n,
                                              canon, sizeof(canon));

        /* render -> parse -> id must reproduce the same id, or a selection
         * would not survive a reboot. */
        char scratch2[1024];
        const char *n2; uint16_t nl2; prom_label_t l2[PROM_MAX_LABELS]; uint8_t c2;
        CHECK(prom_parse_selector(rendered, scratch2, sizeof(scratch2),
                                  &n2, &nl2, l2, PROM_MAX_LABELS, &c2),
              "reparse(%s)", rendered);
        prom_series_id_t id2 = prom_series_id(n2, nl2, l2, c2, canon, sizeof(canon));
        CHECK(id1 == id2, "id round-trip for %s (%llu vs %llu)",
              cases[i], (unsigned long long)id1, (unsigned long long)id2);
    }

    /* Label order must not affect identity. */
    const char *a = "m{a=\"1\",b=\"2\"}", *b = "m{b=\"2\",a=\"1\"}";
    const char *na, *nb; uint16_t la, lb; uint8_t ca, cb;
    prom_label_t Aa[PROM_MAX_LABELS], Bb[PROM_MAX_LABELS];
    char s1[512], s2[512];
    prom_parse_selector(a, s1, sizeof(s1), &na, &la, Aa, PROM_MAX_LABELS, &ca);
    prom_parse_selector(b, s2, sizeof(s2), &nb, &lb, Bb, PROM_MAX_LABELS, &cb);
    CHECK(prom_series_id(na, la, Aa, ca, canon, sizeof(canon)) ==
          prom_series_id(nb, lb, Bb, cb, canon, sizeof(canon)),
          "label order does not change identity");

    /* Different label VALUES must differ. */
    const char *c = "m{a=\"1\",b=\"3\"}";
    const char *nc; uint16_t lc; uint8_t cc; prom_label_t Cc[PROM_MAX_LABELS];
    char s3[512];
    prom_parse_selector(c, s3, sizeof(s3), &nc, &lc, Cc, PROM_MAX_LABELS, &cc);
    CHECK(prom_series_id(na, la, Aa, ca, canon, sizeof(canon)) !=
          prom_series_id(nc, lc, Cc, cc, canon, sizeof(canon)),
          "different label values give different ids");

    /* The classic ambiguity: {a="1",b="2"} vs {a="1|b", ...}-style packing.
     * The NUL-separated canonical form must keep these distinct. */
    const char *d1 = "m{a=\"x\",b=\"y\"}", *d2 = "m{a=\"x\\\"b=\\\"y\"}";
    const char *nd1, *nd2; uint16_t ld1, ld2; uint8_t cd1, cd2;
    prom_label_t D1[PROM_MAX_LABELS], D2[PROM_MAX_LABELS];
    char s4[512], s5[512];
    prom_parse_selector(d1, s4, sizeof(s4), &nd1, &ld1, D1, PROM_MAX_LABELS, &cd1);
    prom_parse_selector(d2, s5, sizeof(s5), &nd2, &ld2, D2, PROM_MAX_LABELS, &cd2);
    CHECK(prom_series_id(nd1, ld1, D1, cd1, canon, sizeof(canon)) !=
          prom_series_id(nd2, ld2, D2, cd2, canon, sizeof(canon)),
          "separator injection does not collide");

    /* Overflow must be reported, never truncated into a different identity. */
    prom_label_t big = { "k", 1, "v", 1 };
    CHECK(prom_canon(canon, 2, "averylongmetricname", 19, &big, 1) == 0,
          "canon reports overflow");
    CHECK(prom_render(rendered, 4, "averylongmetricname", 19, &big, 1) == 0,
          "render reports overflow");
}

static void test_rate(void)
{
    printf("counter rate\n");
    rate_state_t st = {0};
    float r = -1;

    CHECK(prom_rate_step(&st, prom_num(100), 1000, 45000, &r) == RATE_WARMING,
          "first sample warms up");

    r = -1;
    CHECK(prom_rate_step(&st, prom_num(200), 2000, 45000, &r) == RATE_OK && r == 100.0f,
          "100 units in 1s = 100/s (got %f)", (double)r);

    /* Counter reset: value went backwards, so the delta is the new value. */
    r = -1;
    CHECK(prom_rate_step(&st, prom_num(50), 3000, 45000, &r) == RATE_RESET && r == 50.0f,
          "reset yields v/dt (got %f)", (double)r);

    /* Gap: no point, baseline re-armed. */
    r = -1;
    CHECK(prom_rate_step(&st, prom_num(999), 100000, 45000, &r) == RATE_GAP,
          "gap emits nothing");
    r = -1;
    CHECK(prom_rate_step(&st, prom_num(1099), 101000, 45000, &r) == RATE_OK && r == 100.0f,
          "rate resumes after gap (got %f)", (double)r);

    /* dt <= 0 keeps the baseline; a repeated timestamp must not stall the
     * series forever, so the next good sample still produces a rate. */
    CHECK(prom_rate_step(&st, prom_num(1200), 101000, 45000, &r) == RATE_BAD_DT,
          "duplicate timestamp");
    r = -1;
    CHECK(prom_rate_step(&st, prom_num(1199), 102000, 45000, &r) == RATE_OK && r == 100.0f,
          "baseline intact after bad dt (got %f)", (double)r);

    /* NaN disarms; the following sample re-baselines rather than differencing
     * against a stale value. */
    prom_value_t nanv = { PVAL_NAN, 0 };
    CHECK(prom_rate_step(&st, nanv, 103000, 45000, &r) == RATE_NOT_FINITE, "NaN");
    CHECK(prom_rate_step(&st, prom_num(5), 104000, 45000, &r) == RATE_WARMING,
          "re-arms after NaN");

    /* The float32 trap: a counter above ~1.7e7 whose delta must survive. */
    rate_state_t big = {0};
    prom_rate_step(&big, prom_num(12345678901234.0), 1000, 45000, &r);
    r = -1;
    prom_rate_step(&big, prom_num(12345678901334.0), 2000, 45000, &r);
    CHECK(r == 100.0f, "delta of 100 on a 1.2e13 counter survives (got %f)", (double)r);
}

static void test_hist(void)
{
    printf("histogram quantile\n");
    /* 80 observations; buckets are cumulative. */
    double le[]  = { 0.005, 0.01, 0.025, INFINITY };
    double cum[] = { 10,    25,   60,    80       };

    double p50 = prom_hist_quantile(0.5, le, cum, 4);
    /* rank = 40 lands in the 0.025 bucket: 25 below, 60 at/below.
     * 0.01 + (0.025-0.01) * (40-25)/(60-25) = 0.01 + 0.015*15/35 = 0.016428.. */
    CHECK(fabs(p50 - 0.0164285714) < 1e-9, "p50 interpolates (got %.10f)", p50);

    /* A quantile falling in the unbounded top bucket cannot be interpolated;
     * the strongest true statement is its lower bound. */
    double p99 = prom_hist_quantile(0.99, le, cum, 4);
    CHECK(fabs(p99 - 0.025) < 1e-12, "p99 in +Inf bucket clamps to 0.025 (got %f)", p99);

    CHECK(isnan(prom_hist_quantile(0.5, le, (double[]){0,0,0,0}, 4)),
          "empty histogram is NaN");

    /* Non-monotonic counts from a scrape racing an observation. */
    double bad[] = { 10, 5, 60, 80 };
    prom_hist_repair(bad, 4);
    CHECK(bad[1] == 10, "repair clamps the dip (got %f)", bad[1]);
    double q = prom_hist_quantile(0.5, le, bad, 4);
    CHECK(q >= 0.01 && q <= 0.025, "repaired quantile stays in its bucket (got %f)", q);

    /* Missing +Inf bucket: the top bound is finite, so q>=1 returns it. */
    double le2[]  = { 1, 2, 3 };
    double cum2[] = { 1, 2, 3 };
    CHECK(prom_hist_quantile(1.0, le2, cum2, 3) == 3.0, "finite top bound");

    /* More than one infinite bound -- a buggy exporter repeating a family, or
     * a proxy concatenating scrapes. Taking n-2 blindly used to return
     * infinity, which shows on a panel as no data rather than as a number. */
    double le3[]  = { 0.005, 0.01, INFINITY, INFINITY };
    double cum3[] = { 10,    25,   80,       80        };
    double qq = prom_hist_quantile(0.99, le3, cum3, 4);
    CHECK(isfinite(qq) && fabs(qq - 0.01) < 1e-12,
          "duplicate +Inf bounds still yield a finite quantile (got %f)", qq);
    CHECK(isfinite(prom_hist_quantile(1.0, le3, cum3, 4)),
          "q=1 with duplicate +Inf is finite");

    /* Every bound infinite: genuinely nothing to say, so NaN is correct. */
    double le4[]  = { INFINITY, INFINITY };
    double cum4[] = { 5, 5 };
    CHECK(isnan(prom_hist_quantile(0.5, le4, cum4, 2)),
          "all-infinite bounds is NaN, not a bogus number");

    CHECK(isnan(prom_hist_average(1.0, 0.0)), "avg guards zero count");
    CHECK(prom_hist_average(10.0, 4.0) == 2.5, "avg");
}

static void test_parser_semantics(void)
{
    printf("parser semantics\n");
    digest_t d = {0};
    prom_text_stats_t st;
    const char *body =
        "# TYPE foo gauge\n"
        "foo_bar_baz 1\n"                       /* must NOT inherit foo's type */
        "# TYPE lat histogram\n"
        "lat_bucket{le=\"0.5\",x=\"1\"} 3\n"    /* le lifted out of labels */
        "lat_sum 1.5\n"
        "# TYPE http_requests counter\n"
        "http_requests_total{c=\"200\"} 7\n";   /* OpenMetrics _total dialect */
    replay(body, strlen(body), 0, &d, &st);

    CHECK(strstr(d.buf, "S|foo_bar_baz|base=foo_bar_baz|type=untyped|role=plain") != NULL,
          "unrelated prefix does not inherit a TYPE");
    CHECK(strstr(d.buf, "S|lat_bucket{x=\"1\"}|base=lat|type=histogram|role=bucket|"
                        "v=3|le=0.5") != NULL,
          "le is lifted out of the label set and the family is lat");
    CHECK(strstr(d.buf, "S|lat_sum|base=lat|type=histogram|role=sum") != NULL,
          "_sum resolves to the histogram family");
    CHECK(strstr(d.buf, "S|http_requests_total{c=\"200\"}|base=http_requests_total|"
                        "type=counter|role=plain") != NULL,
          "OpenMetrics _total gets the type hint but keeps its own base name");
    CHECK(st.samples == 4, "4 samples parsed (got %u)", st.samples);
    free(d.buf);

    /* An overlong line costs that line only, never the rest of the stream. */
    printf("overlong line containment\n");
    size_t huge = PROM_LINE_MAX + 500;
    char *big = malloc(huge + 64);
    memset(big, 'a', huge);
    memcpy(big, "metric_", 7);
    big[huge] = '\0';
    char *body2 = malloc(huge + 128);
    snprintf(body2, huge + 128, "good_before 1\n%s 5\ngood_after 2\n", big);

    digest_t d2 = {0};
    prom_text_stats_t st2;
    replay(body2, strlen(body2), 0, &d2, &st2);
    CHECK(st2.err_too_long == 1, "overlong line counted (got %u)", st2.err_too_long);
    CHECK(strstr(d2.buf, "S|good_before") != NULL, "line before survives");
    CHECK(strstr(d2.buf, "S|good_after")  != NULL, "line after survives");
    free(d2.buf); free(big); free(body2);

    /* A body with no trailing newline must still deliver its last sample. */
    digest_t d3 = {0};
    prom_text_stats_t st3;
    const char *b3 = "a 1\nb 2";
    replay(b3, strlen(b3), 0, &d3, &st3);
    CHECK(st3.samples == 2, "trailing line with no newline (got %u)", st3.samples);
    free(d3.buf);
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    printf("== unit tests ==\n");
    test_values();
    test_ident();
    test_rate();
    test_hist();
    test_parser_semantics();

    printf("\n== corpus replay (whole vs 1-byte vs 7-byte chunks) ==\n");
    for (int i = 1; i < argc; i++) test_corpus(argv[i]);

    printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
