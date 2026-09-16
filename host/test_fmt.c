#include "ui_fmt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_ran, g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_ran++;                                                             \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

static const char *J(double v, fmt_mode_t m, const char *u, int8_t pin,
                     bool group)
{
    static char buf[96];
    fmt_style_t sy = { m, u, pin, group, NULL, NULL };
    ui_fmt_join(v, &sy, buf, sizeof(buf));
    return buf;
}

static const char *JA(double v, fmt_mode_t m, int8_t pin, bool group,
                      const char *pre, const char *suf, bool *numeric)
{
    static char num[96], sfx[24];
    fmt_style_t sy = { m, "", pin, group, pre, suf };
    ui_fmt_value(v, &sy, NULL, num, sizeof(num), sfx, sizeof(sfx), numeric);
    return num;
}
#define EQA(v, m, pin, group, pre, suf, want) do { \
    bool numeric_; const char *got_ = JA(v, m, pin, group, pre, suf, &numeric_); \
    CHECK(strcmp(got_, want) == 0, "affixed -> \"%s\", want \"%s\"", got_, want); \
} while (0)
#define EQ(v, m, u, want) \
    CHECK(strcmp(J(v, m, u, FMT_PIN_AUTO, false), want) == 0, \
          "%g -> \"%s\", want \"%s\"", \
          (double)(v), J(v, m, u, FMT_PIN_AUTO, false), want)

/* Same, with the prefix pinned. */
#define EQP(v, m, u, pin, want) \
    CHECK(strcmp(J(v, m, u, pin, false), want) == 0, \
          "%g pinned %d -> \"%s\", want \"%s\"", \
          (double)(v), (int)(pin), J(v, m, u, pin, false), want)

/* Same, with thousands separators on. */
#define EQG(v, m, u, pin, want) \
    CHECK(strcmp(J(v, m, u, pin, true), want) == 0, \
          "%g grouped -> \"%s\", want \"%s\"", \
          (double)(v), J(v, m, u, pin, true), want)

static void test_sigfigs(void)
{
    printf("significant digits\n");
    EQ(847.0,  FMT_RAW, "", "847");
    EQ(42.7,   FMT_RAW, "", "42.7");
    EQ(3.14159,FMT_RAW, "", "3.14");
    EQ(0.5,    FMT_RAW, "", "0.500");
    EQ(0.0,    FMT_RAW, "", "0.000");
    EQ(-42.75, FMT_RAW, "", "-42.8");
}

static void test_si_iec(void)
{
    printf("SI and IEC prefixes\n");
    EQ(1500.0,        FMT_SI, "", "1.50 k");
    EQ(1234567.0,     FMT_SI, "", "1.23 M");
    /* No prefix and no base unit means no suffix, hence no separator. */
    EQ(999.0,         FMT_SI, "", "999");
    EQ(0.0042,        FMT_SI, "", "4.20 m");
    EQ(1.2345678e13,  FMT_SI, "", "12.3 T");

    EQ(1024.0,        FMT_IEC, "", "1.00 KiB");
    EQ(11.4 * 1073741824.0, FMT_IEC, "", "11.4 GiB");
    /* Whole bytes below 1 KiB: "512.000 B" would be absurd. */
    EQ(512.0,         FMT_IEC, "", "512 B");

    EQ(8400000.0,     FMT_RATE_IEC, "", "8.01 MiB/s");
    EQ(1240.0,        FMT_RATE_SI,  "", "1.24 k/s");
}

/*
 * The headline anti-jitter test. A value wandering across a prefix boundary
 * must NOT flap between "999" and "1.00k" -- each flap is a repaint, and on a
 * wall panel it reads as the number being unstable rather than the display.
 */
static void test_hysteresis(void)
{
    printf("prefix hysteresis\n");
    fmt_state_t st = {0};
    char num[32], suf[16];
    bool numeric;

    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(990.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "") == 0, "990 starts unprefixed (suf=\"%s\")", suf);

    /* Just over 1000 must NOT promote yet -- the threshold is 1000*1.05. */
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(1010.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "") == 0, "1010 stays unprefixed (suf=\"%s\" num=\"%s\")", suf, num);

    /* Comfortably past the threshold: promote. */
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(1200.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "k") == 0, "1200 promotes to k (suf=\"%s\")", suf);

    /* Dropping just under 1000 must NOT demote immediately. */
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(980.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "k") == 0, "980 holds at k (suf=\"%s\")", suf);

    /* Well under: demote. */
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(500.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "") == 0, "500 demotes (suf=\"%s\")", suf);

    /* Now the real scenario: oscillate around the boundary and count changes.
     * Without hysteresis this produces a change on every single sample. */
    fmt_state_t st2 = {0};
    char prev[32] = "";
    int changes = 0;
    const double wobble[] = { 995, 1005, 998, 1002, 999, 1001, 997, 1003 };
    for (size_t i = 0; i < sizeof(wobble)/sizeof(wobble[0]); i++) {
        { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(wobble[i], &sy, &st2, num, sizeof(num), suf, sizeof(suf), &numeric); }
        char joined[48];
        snprintf(joined, sizeof(joined), "%s%s", num, suf);
        if (i > 0 && strcmp(joined, prev) != 0) changes++;
        strcpy(prev, joined);
    }
    CHECK(changes <= 7, "wobbling around 1000 must not change scale (changes=%d)",
          changes);
    /* The scale specifically must never have moved. */
    CHECK(st2.exp == 0, "scale stayed put across the wobble (exp=%d)", (int)st2.exp);

    /* A zero sample must not reset the scale: a series that idles at 0 should
     * not jump scale the moment it wakes up. */
    fmt_state_t st3 = {0};
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(5e6, &sy, &st3, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "M") == 0, "5e6 -> M");
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(0.0, &sy, &st3, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(suf, "M") == 0, "zero holds the prefix (suf=\"%s\")", suf);
}

/*
 * Counts are integers. "3.00 req" and "0.000 req" read as a broken display,
 * which is exactly how this looked against a real inference server's
 * num_running_reqs / num_queue_reqs.
 */
static void test_integral_series(void)
{
    printf("integral series\n");
    char num[32], suf[16]; bool numeric;

    fmt_state_t st = {0};
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(3.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "3") == 0, "3 -> \"%s\", want \"3\"", num);
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(0.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "0") == 0, "0 -> \"%s\", want \"0\"", num);
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(12.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "12") == 0, "12 -> \"%s\"", num);

    /* One fractional sample switches the series to decimals permanently, so a
     * measurement that merely lands on a round number is not mistaken for a
     * count. */
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(0.84, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "0.840") == 0, "0.84 -> \"%s\"", num);
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(1.0, &sy, &st, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "1.00") == 0, "after a fraction, 1.0 keeps decimals -> \"%s\"", num);

    /* Without state there is nothing to track, so the sig-fig rule stands. */
    { fmt_style_t sy = { FMT_RAW, "", FMT_PIN_AUTO, false };
      ui_fmt_value(3.0, &sy, NULL, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "3.00") == 0, "stateless keeps sig-figs -> \"%s\"", num);

    /* Scaling still applies: an integral byte count is not forced to 0 dp
     * once it has a prefix. */
    fmt_state_t st2 = {0};
    { fmt_style_t sy = { FMT_IEC, "", FMT_PIN_AUTO, false };
      ui_fmt_value(11.4 * 1073741824.0, &sy, &st2, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "11.4") == 0, "scaled IEC keeps decimals -> \"%s\"", num);
}

static void test_duration(void)
{
    printf("durations\n");
    char b[32];
    struct { double s; const char *want; } cases[] = {
        { 0.00082,   "820 us" },
        { 0.82,      "820 ms" },
        { 5.5,       "5.5 s"  },
        { 42.7,      "43 s"   },
        { 750.0,     "12m 30s"},
        { 15120.0,   "4h 12m" },
        { 273600.0,  "3d 4h"  },
        { 1555200.0, "18d"    },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ui_fmt_duration(cases[i].s, b, sizeof(b));
        CHECK(strcmp(b, cases[i].want) == 0, "%g s -> \"%s\", want \"%s\"",
              cases[i].s, b, cases[i].want);
    }
    /* Never three units. */
    ui_fmt_duration(273600.0 + 754.0, b, sizeof(b));
    int spaces = 0;
    for (const char *p = b; *p; p++) if (*p == ' ') spaces++;
    CHECK(spaces <= 1, "at most two units, got \"%s\"", b);
}

static void test_percent_delta(void)
{
    printf("percent and delta\n");
    EQ(0.0423, FMT_PCT_01,  "", "4.2 %");
    EQ(0.671,  FMT_PCT_01,  "", "67 %");
    EQ(94.5,   FMT_PCT_100, "", "94 %");

    char b[32];
    CHECK(ui_fmt_delta_pct(112.0, 100.0, b, sizeof(b)) == 1 &&
          strcmp(b, "+12 %") == 0, "rise -> \"%s\"", b);
    CHECK(ui_fmt_delta_pct(95.0, 100.0, b, sizeof(b)) == -1, "fall");
    /* Sub-half-percent is noise and must read as nothing. */
    CHECK(ui_fmt_delta_pct(100.2, 100.0, b, sizeof(b)) == 0 &&
          strcmp(b, "--") == 0, "tiny change is suppressed -> \"%s\"", b);
    CHECK(ui_fmt_delta_pct(5.0, 0.0, b, sizeof(b)) == 0, "divide by zero guarded");
}

static void test_nonfinite(void)
{
    printf("non-finite values\n");
    char num[32], suf[16];
    bool numeric;
    /* A NaN gauge is an ABSENT reading. Rendering it as 0 would look exactly
     * like a real zero, which is the worst possible outcome on a panel. */
    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(NAN, &sy, NULL, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "--") == 0, "NaN -> \"%s\"", num);
    CHECK(strcmp(num, "0") != 0, "NaN never renders as zero");

    { fmt_style_t sy = { FMT_SI, "", FMT_PIN_AUTO, false };
      ui_fmt_value(INFINITY, &sy, NULL, num, sizeof(num), suf, sizeof(suf), &numeric); }
    CHECK(strcmp(num, "+Inf") == 0, "+Inf -> \"%s\"", num);
    CHECK(numeric == false, "+Inf needs a text face");
}

/*
 * The large display faces are digits-only (tools/gen_fonts.sh). A glyph that
 * is missing from a font draws as NOTHING in LVGL, so if the formatter ever
 * puts a letter in `num` while claiming numeric_only, the value silently
 * disappears from the tile. This sweeps a wide range of inputs and modes to
 * hold that invariant.
 */
static void test_rate_hour(void)
{
    printf("per-hour rates\n");

    /* The value on the wire is per second; only the display scales. */
    EQ(1.0,   FMT_RATE_HOUR, "", "3.60 k/h");

    /* 333 tok/s is the number you cannot reason about; 1.2 M/h is the one you
     * can. That is the whole reason the mode exists. */
    EQ(333.0, FMT_RATE_HOUR, "", "1.20 M/h");

    /* The same value in both units, so a mix-up shows up here rather than on
     * the glass as a number 3600x too large. */
    EQ(333.0, FMT_RATE_SI,   "", "333 /s");

    EQ(0.0,   FMT_RATE_HOUR, "", "0.000 /h");
}

static void test_pinned_prefix(void)
{
    printf("pinned prefixes\n");

    /* The case this exists for: a value crossing 1000 changes unit under
     * auto, and does not when pinned. Both readings are correct; only one can
     * be compared at a glance. */
    EQ (847.0,  FMT_RATE_SI, "tok", "847 tok/s");
    EQ (1200.0, FMT_RATE_SI, "tok", "1.20 ktok/s");
    EQP(847.0,  FMT_RATE_SI, "tok", 1, "0.847 ktok/s");
    EQP(1200.0, FMT_RATE_SI, "tok", 1, "1.20 ktok/s");

    /* A pin out of the ladder's range is clamped, not honoured: the same pin
     * has to stay legal when a panel's format changes. */
    EQP(2048.0, FMT_IEC, "", -3, "2048 B");
    EQP(2048.0, FMT_IEC, "",  1, "2.00 KiB");

    /* Formats with no ladder ignore it rather than mangling the value. */
    EQP(0.5,  FMT_PCT_01,   "", 2, "50 %");
    EQP(90.0, FMT_DURATION, "", 2, "1m 30s");

    /* Names for a chooser come from the ladder the format actually uses. */
    CHECK(strcmp(ui_fmt_prefix_name(FMT_SI,  0), "1") == 0, "SI 0 name");
    CHECK(strcmp(ui_fmt_prefix_name(FMT_SI,  1), "k") == 0, "SI 1 name");
    CHECK(strcmp(ui_fmt_prefix_name(FMT_IEC, 0), "B") == 0, "IEC 0 name");
    CHECK(strcmp(ui_fmt_prefix_name(FMT_IEC, 1), "KiB") == 0, "IEC 1 name");
    CHECK(ui_fmt_prefix_name(FMT_DURATION, 1) == NULL, "durations have no ladder");
}

static void test_grouping(void)
{
    printf("thousands separators\n");

    /* The case asked for: a pinned plain-unit count, read across a room. */
    EQG(17321.0, FMT_RAW, "", FMT_PIN_AUTO, "17,321");
    EQG(17321.0, FMT_SI,  "", 0,            "17,321");

    /* Boundaries either side of the first separator. */
    EQG(999.0,    FMT_RAW, "", FMT_PIN_AUTO, "999");
    EQG(1000.0,   FMT_RAW, "", FMT_PIN_AUTO, "1,000");
    EQG(999999.0, FMT_RAW, "", FMT_PIN_AUTO, "999,999");
    EQG(1000000.0, FMT_RAW, "", FMT_PIN_AUTO, "1,000,000");

    /* The sign and the fraction are not digits to be grouped. */
    EQG(-1234567.0, FMT_RAW, "", FMT_PIN_AUTO, "-1,234,567");
    EQG(12345.6,    FMT_SI,  "", 0,            "12,346");

    /* Grouping never fires when the prefix already shortened the number,
     * which is why it only matters alongside a pin. */
    EQG(1234567.0, FMT_SI, "", FMT_PIN_AUTO, "1.23 M");

    /* Modes that do not produce bare digits are left alone. */
    EQG(90.0,  FMT_DURATION, "", FMT_PIN_AUTO, "1m 30s");
    EQG(1.0,   FMT_BOOL,     "", FMT_PIN_AUTO, "UP");

    /* And off is still off. */
    EQ(17321.0, FMT_RAW, "", "17321");
}

static void test_affixes(void)
{
    printf("prefix and suffix\n");

    EQA(1234.0, FMT_RAW, 0, true,  "$",  NULL, "$1,234");
    EQA(1234.0, FMT_RAW, 0, true,  NULL, " \u20ac", "1,234 \u20ac");
    EQA(1234.0, FMT_RAW, 0, true,  "\u00a3", NULL, "\u00a31,234");
    EQA(-42.0,  FMT_RAW, 0, false, "$",  NULL, "$-42.0");

    /* Grouping runs before the affixes, so a symbol is never counted as a
     * digit -- "$1234" must not become "$1,23,4" or similar. */
    EQA(1000000.0, FMT_RAW, 0, true, "$", NULL, "$1,000,000");

    /* The charset is what decides whether the big faces can be used. */
    CHECK(ui_fmt_digits_safe("$1,234") == true,  "dollar is carried");
    CHECK(ui_fmt_digits_safe("\u20ac") == true,  "euro is carried");
    CHECK(ui_fmt_digits_safe("\u00a2\u00a3\u00a5") == true, "cent pound yen carried");
    CHECK(ui_fmt_digits_safe("USD") == false,    "letters are not");
    CHECK(ui_fmt_digits_safe("\u00b5") == false, "micro sign is not");
    CHECK(ui_fmt_digits_safe(NULL) == true,      "nothing is safe");

    /* A symbol outside the charset drops the value to a text face rather than
     * vanishing -- the flag the tile reads to choose the font. */
    bool numeric;
    JA(12.0, FMT_RAW, FMT_PIN_AUTO, false, "$", NULL, &numeric);
    CHECK(numeric == true, "a dollar keeps the digit face");
    JA(12.0, FMT_RAW, FMT_PIN_AUTO, false, "USD ", NULL, &numeric);
    CHECK(numeric == false, "letters fall back to a text face");
}

static void test_charset_invariant(void)
{
    printf("digits-only font charset invariant\n");
    /* The same set ui_fmt_digits_safe enforces and gen_fonts.sh generates. */
    static const char *allowed = " !$%+,-./0123456789:";
    const fmt_mode_t modes[] = { FMT_RAW, FMT_SI, FMT_IEC, FMT_PCT_01,
                                 FMT_PCT_100, FMT_RATE_SI, FMT_RATE_IEC,
                                 FMT_RATE_HOUR };
    const double vals[] = {
        0.0, 1.0, -1.0, 0.5, 1e-9, 1e-6, 1e-3, 999.0, 1000.0, 1024.0,
        1e6, 1.2345678e13, 1e18, -4096.0, 0.0042, 123456.789,
    };

    int checked = 0;
    for (size_t m = 0; m < sizeof(modes)/sizeof(modes[0]); m++) {
        for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
            char num[48], suf[24];
            bool numeric;
            /* Pinned as well as auto: a pin changes which branch formats the
             * number, so the digits-only guarantee has to hold on both. */
            int8_t pin = (i % 3 == 0) ? FMT_PIN_AUTO : (int8_t)(i % 4);
            /* Grouping on for half the sweep: the comma has to be in the
             * digits-only charset too, and it is easy to forget that. */
            fmt_style_t sy = { modes[m], "", pin, (i % 2) == 0 };
            ui_fmt_value(vals[i], &sy, NULL, num, sizeof(num),
                         suf, sizeof(suf), &numeric);
            if (!numeric) continue;
            for (const char *p = num; *p; p++) {
                if (strchr(allowed, *p) == NULL) {
                    g_fail++;
                    printf("  FAIL mode %d value %g -> \"%s\" contains '%c' "
                           "which the digits-only face cannot draw\n",
                           (int)modes[m], vals[i], num, *p);
                }
            }
            checked++;
        }
    }
    g_ran++;
    printf("  (%d value/mode combinations swept)\n", checked);
}

static void test_infer(void)
{
    printf("unit inference\n");
    fmt_mode_t m; agg_mode_t a; char u[16];

#define INF(n, t) ui_fmt_infer(n, strlen(n), t, &m, u, sizeof(u), &a)

    CHECK(INF("node_memory_MemAvailable_bytes", 2) && m == FMT_IEC && a == AGG_LAST,
          "_bytes -> IEC gauge");
    CHECK(INF("node_network_receive_bytes_total", 1) && m == FMT_RATE_IEC &&
          a == AGG_RATE, "_bytes_total -> IEC rate");
    /* Rate-of-seconds is utilisation, not "0.87 s/s". */
    CHECK(INF("node_cpu_seconds_total", 1) && m == FMT_PCT_01 && a == AGG_RATE,
          "_seconds_total -> utilisation percent");
    CHECK(INF("http_request_duration_seconds", 3) && m == FMT_DURATION,
          "_seconds -> duration");
    CHECK(INF("node_hwmon_temp_celsius", 2) && m == FMT_RAW && strcmp(u, "\xC2\xB0" "C") == 0,
          "_celsius -> degrees (u=\"%s\")", u);
    CHECK(INF("node_load1", 2) && m == FMT_SI && a == AGG_LAST, "plain gauge");
    CHECK(INF("up", 2) && m == FMT_BOOL, "up -> bool");
    CHECK(INF("http_requests_total", 1) && m == FMT_RATE_SI && a == AGG_RATE,
          "_total -> rate");
    /* A counter with no recognised suffix must still become a rate: a raw
     * monotonic total is never worth a tile. */
    CHECK(INF("weird_counter_thing", 1) && a == AGG_RATE, "bare counter -> rate");
    CHECK(!INF("node_os_info", 2), "_info is excluded from the browser");
#undef INF
}

int main(void)
{
    test_sigfigs();
    test_si_iec();
    test_hysteresis();
    test_integral_series();
    test_duration();
    test_percent_delta();
    test_nonfinite();
    test_rate_hour();
    test_pinned_prefix();
    test_grouping();
    test_affixes();
    test_charset_invariant();
    test_infer();
    printf("\n%d checks, %d failures\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
