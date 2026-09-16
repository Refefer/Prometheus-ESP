#include "config.h"
#include "prom_ident.h"
#include "storage.h"
#include "ui_layout.h"

#include "cJSON.h"
#include "esp_log.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stdarg.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "config";

#define PATH_CUR  STORAGE_CFG_PATH "/config.json"
#define PATH_NEW  STORAGE_CFG_PATH "/config.new"
#define PATH_BAK  STORAGE_CFG_PATH "/config.bak"

static config_t    s_cfg;
static uint32_t    s_generation;
static bool        s_dirty;
static bool        s_was_reset;
static lv_timer_t *s_flush_timer;

/* ------------------------------------------------------------- defaults */

static void set_defaults_into(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema  = CFG_SCHEMA_VERSION;
    cfg->next_id = 1;
    strncpy(cfg->device.theme, "night_ops", sizeof(cfg->device.theme) - 1);
    cfg->device.poll_default_s = 10;
    cfg->device.rotate_dwell_s = 20;

    cfg->n_screens = 1;
    strncpy(cfg->screens[0].title, "Home", sizeof(cfg->screens[0].title) - 1);
}

static void set_defaults(void) { set_defaults_into(&s_cfg); }

/* ------------------------------------------------------- names not numbers */

/*
 * The pushed config is meant to be hand-edited, and "kind": 4 is not
 * something anyone should have to look up -- getting it wrong silently
 * produces a plausible tile showing the right number in the wrong units,
 * which is the worst kind of mistake to debug.
 *
 * Names are written; both names and the old integers are read, so a config
 * saved by an earlier build still loads.
 */
typedef struct { int v; const char *name; } enum_name_t;

static const enum_name_t k_kinds[] = {
    { TILE_STAT, "stat" }, { TILE_SPARK, "sparkline" }, { TILE_CHART, "chart" },
    { TILE_BAR, "bar" }, { TILE_GAUGE, "gauge" }, { TILE_STATUS, "status" },
    { TILE_HIST, "histogram" }, { TILE_MULTI, "multi" }, { 0, NULL },
};
/*
 * Prefix pins. The number is the ladder exponent and means the same in both
 * ladders -- 1 is k on an SI panel and Ki on a byte count -- so one table
 * serves both. "1" is the empty prefix, spelled as a multiplier because an
 * empty string is invisible in a config file.
 */
static const enum_name_t k_scales[] = {
    { FMT_PIN_AUTO, "auto" },
    { 0, "1" }, { 1, "k" }, { 2, "M" }, { 3, "G" }, { 4, "T" }, { 5, "P" },
    { -1, "m" }, { -2, "u" }, { -3, "n" }, { -4, "p" },
    { 0, NULL },
};

static const enum_name_t k_fmts[] = {
    { FMT_AUTO, "auto" }, { FMT_RAW, "raw" }, { FMT_SI, "si" },
    { FMT_IEC, "bytes" }, { FMT_PCT_01, "percent" },
    { FMT_PCT_100, "percent100" }, { FMT_DURATION, "duration" },
    { FMT_RATE_SI, "rate" }, { FMT_RATE_IEC, "rate_bytes" },
    { FMT_BOOL, "bool" }, { FMT_RATE_HOUR, "rate_hour" }, { 0, NULL },
};
static const enum_name_t k_reduces[] = {
    { RED_SUM, "sum" }, { RED_AVG, "avg" }, { RED_MIN, "min" },
    { RED_MAX, "max" }, { RED_COUNT, "count" }, { RED_FIRST, "first" },
    { 0, NULL },
};
static const enum_name_t k_aggs[] = {
    { AGG_LAST, "last" }, { AGG_RATE, "rate" }, { AGG_DELTA, "delta" },
    { AGG_AVG, "avg" }, { 0, NULL },
};
static const enum_name_t k_ops[] = {
    { OP_NONE, "none" }, { OP_SHARE, "share" }, { OP_RATIO, "ratio" },
    { OP_DIFF, "diff" }, { OP_SUM, "sum" }, { 0, NULL },
};

/*
 * Where a rejected document is described from.
 *
 * Carried through the parse rather than kept in a static, because two tasks
 * can parse at once -- a push on the HTTP task and a layout activation on the
 * LVGL task -- and the first error wins so the message names the first thing
 * wrong rather than the last.
 */
typedef struct { char *msg; size_t cap; bool bad; } parse_err_t;

static void perr(parse_err_t *pe, const char *fmt, ...)
{
    if (pe == NULL || pe->bad) return;
    pe->bad = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pe->msg, pe->cap, fmt, ap);
    va_end(ap);
}

/* The accepted names, for saying so in a rejection. */
static void enum_list(const enum_name_t *tab, char *out, size_t cap)
{
    size_t w = 0;
    out[0] = '\0';
    for (int i = 0; tab[i].name && w + 24 < cap; i++) {
        w += (size_t)snprintf(out + w, cap - w, "%s%s", i ? " " : "", tab[i].name);
    }
}

static const char *enum_to_name(const enum_name_t *tab, int v)
{
    for (int i = 0; tab[i].name; i++) if (tab[i].v == v) return tab[i].name;
    return tab[0].name;
}

/* Accepts a name or, for configs written by an earlier build, an integer. */
/*
 * An enum by name, and a rejection for anything else.
 *
 * Both of the ways this used to be lenient were silent: an unrecognised name
 * fell back to the default, so "chartt" produced a working big-number tile
 * and nothing said why; and a raw integer was honoured, so a config that
 * meant percent (4) and typed duration (6) rendered a hit rate as
 * milliseconds. Numbers are refused outright -- they are position-dependent
 * and shift whenever the enum gains a member, and every config this firmware
 * writes uses names.
 */
static int name_to_enum_ck(const cJSON *o, const char *key,
                           const enum_name_t *tab, int def,
                           parse_err_t *pe, const char *where)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (v == NULL || cJSON_IsNull(v)) return def;

    if (cJSON_IsString(v) && v->valuestring) {
        for (int i = 0; tab[i].name; i++) {
            if (strcasecmp(tab[i].name, v->valuestring) == 0) return tab[i].v;
        }
        char list[176];
        enum_list(tab, list, sizeof(list));
        perr(pe, "%s: \"%s\" is not a %s; expected one of: %s",
             where, v->valuestring, key, list);
        return def;
    }
    if (cJSON_IsNumber(v)) {
        char list[176];
        enum_list(tab, list, sizeof(list));
        perr(pe, "%s: %s must be a name, not a number; expected one of: %s",
             where, key, list);
        return def;
    }
    perr(pe, "%s: %s must be a string", where, key);
    return def;
}

/*
 * Every field this parser reads, so a typo is refused rather than ignored.
 *
 * Ignoring unknown keys is what makes schema migration additive, and that is
 * still true for keys this build has not heard of yet -- but in practice the
 * unknown key is a misspelling of a known one, and silently dropping "colum"
 * puts the tile at column zero with a cheerful 200 OK.
 */
static void check_keys(const cJSON *o, const char *const *known,
                       parse_err_t *pe, const char *where)
{
    for (const cJSON *m = o ? o->child : NULL; m; m = m->next) {
        if (m->string == NULL) continue;
        bool ok = false;
        for (int i = 0; known[i]; i++) {
            if (strcmp(known[i], m->string) == 0) { ok = true; break; }
        }
        if (!ok) {
            perr(pe, "%s: unknown field \"%s\"", where, m->string);
            return;
        }
    }
}

static const char *const k_keys_root[] = {
    "schema", "next_id", "active_layout", "id_hash",
    "device", "endpoints", "screens", "panels", NULL,
};
static const char *const k_keys_device[] = {
    "theme", "poll_default_s", "rotate_enabled", "rotate_dwell_s", NULL,
};
static const char *const k_keys_endpoint[] = {
    "id", "name", "kind", "url", "poll_s", "timeout_ms",
    "auth", "insecure_tls", "enabled", NULL,
};
static const char *const k_keys_screen[] = { "title", "pinned", NULL };
static const char *const k_keys_panel[] = {
    "id", "ep", "title", "unit", "kind", "fmt", "op", "scale", "group", "terms",
    "vmin", "vmax", "warn", "crit", "multi", "lower_is_worse",
    "screen", "col", "row", "w", "h",
    /* schema 1 spelled a panel's single term inline; still accepted */
    "sel", "sel_b", "agg", "window_s", "q", NULL,
};
static const char *const k_keys_term[] = {
    "sel", "reduce", "agg", "window_s", "q", NULL,
};

/* ------------------------------------------------------------- writing */

/*
 * A hand-rolled streaming writer rather than cJSON_Print.
 *
 * Printing this document builds a whole cJSON tree plus an output string in
 * PSRAM every time anything changes -- the one recurring allocation spike in
 * otherwise steady-state operation. fprintf straight to the file has neither.
 * Loading still uses cJSON, where the spike happens once at boot and the
 * leniency is worth having.
 */
static void write_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
            else fputc(*s, f);
        }
    }
    fputc('"', f);
}

static void write_float(FILE *f, float v)
{
    /* NAN means "unset" throughout the config; JSON has no NaN literal, so it
     * round-trips as null. */
    if (isnan(v)) fputs("null", f);
    else          fprintf(f, "%.6g", (double)v);
}

/* The presentation half -- screens and panels -- shared by config.json and
 * every layout file, so the two can never drift apart. */
static void write_presentation(FILE *f)
{
    fputs("  \"screens\": [\n", f);
    for (int i = 0; i < s_cfg.n_screens; i++) {
        fputs("    { \"title\": ", f);
        write_escaped(f, s_cfg.screens[i].title);
        fprintf(f, ", \"pinned\": %s }%s\n",
                s_cfg.screens[i].pinned ? "true" : "false",
                i + 1 < s_cfg.n_screens ? "," : "");
    }
    fputs("  ],\n", f);

    fputs("  \"panels\": [\n", f);
    for (int i = 0; i < s_cfg.n_panels; i++) {
        const cfg_panel_t *p = &s_cfg.panels[i];
        fprintf(f, "    { \"id\": %u, \"ep\": %u", (unsigned)p->id,
                (unsigned)p->ep_id);
        fputs(", \"title\": ", f);
        write_escaped(f, p->title);
        fputs(", \"unit\": ", f);
        write_escaped(f, p->unit);
        fputs(", \"kind\": ", f);  write_escaped(f, enum_to_name(k_kinds, p->kind));
        fputs(", \"fmt\": ", f);   write_escaped(f, enum_to_name(k_fmts, p->fmt));
        fputs(", \"op\": ", f);    write_escaped(f, enum_to_name(k_ops, p->op));

        fputs(", \"terms\": [", f);
        for (int k = 0; k < p->n_terms; k++) {
            const cfg_term_t *tm = &p->terms[k];
            if (k) fputs(", ", f);
            fputs("{ \"sel\": ", f);
            write_escaped(f, tm->sel);
            fputs(", \"reduce\": ", f);
            write_escaped(f, enum_to_name(k_reduces, tm->reduce));
            fputs(", \"agg\": ", f);
            write_escaped(f, enum_to_name(k_aggs, tm->agg));
            fprintf(f, ", \"window_s\": %u", (unsigned)tm->window_s);
            fputs(", \"q\": ", f); write_float(f, tm->q);
            fputs(" }", f);
        }
        fputs("]", f);

        fputs(", \"vmin\": ", f); write_float(f, p->vmin);
        fputs(", \"vmax\": ", f); write_float(f, p->vmax);
        fputs(", \"warn\": ", f); write_float(f, p->warn);
        fputs(", \"crit\": ", f); write_float(f, p->crit);
        fprintf(f, ", \"multi\": %s", p->multi ? "true" : "false");
        fputs(", \"scale\": ", f); write_escaped(f, enum_to_name(k_scales, p->scale));
        fprintf(f, ", \"group\": %s", p->group ? "true" : "false");
        fprintf(f, ", \"lower_is_worse\": %s, \"screen\": %u,"
                   " \"col\": %u, \"row\": %u, \"w\": %u, \"h\": %u }%s\n",
                p->lower_is_worse ? "true" : "false",
                (unsigned)p->screen, (unsigned)p->col, (unsigned)p->row,
                (unsigned)p->w, (unsigned)p->h,
                i + 1 < s_cfg.n_panels ? "," : "");
    }
    fputs("  ]\n", f);
}

static esp_err_t write_config(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_FAIL;
    }

    fprintf(f, "{\n  \"schema\": %u,\n", (unsigned)s_cfg.schema);
    fprintf(f, "  \"next_id\": %u,\n", (unsigned)s_cfg.next_id);
    fputs("  \"active_layout\": ", f);
    write_escaped(f, s_cfg.active_layout);
    fputs(",\n", f);

    fputs("  \"device\": { \"theme\": ", f);
    write_escaped(f, s_cfg.device.theme);
    fprintf(f, ", \"poll_default_s\": %u, \"rotate_enabled\": %s,"
               " \"rotate_dwell_s\": %u },\n",
            (unsigned)s_cfg.device.poll_default_s,
            s_cfg.device.rotate_enabled ? "true" : "false",
            (unsigned)s_cfg.device.rotate_dwell_s);

    fputs("  \"endpoints\": [\n", f);
    for (int i = 0; i < s_cfg.n_endpoints; i++) {
        const cfg_endpoint_t *e = &s_cfg.endpoints[i];
        fprintf(f, "    { \"id\": %u, \"name\": ", (unsigned)e->id);
        write_escaped(f, e->name);
        fputs(", \"kind\": ", f);
        write_escaped(f, e->kind == EP_PROMAPI ? "promapi" : "text");
        fputs(", \"url\": ", f);
        write_escaped(f, e->url);
        fprintf(f, ", \"poll_s\": %u, \"timeout_ms\": %u, \"auth\": %u,"
                   " \"insecure_tls\": %s, \"enabled\": %s }%s\n",
                (unsigned)e->poll_s, (unsigned)e->timeout_ms, (unsigned)e->auth,
                e->insecure_tls ? "true" : "false",
                e->enabled ? "true" : "false",
                i + 1 < s_cfg.n_endpoints ? "," : "");
    }
    fputs("  ],\n", f);

    write_presentation(f);
    fputs("}\n", f);

    /* fflush + fsync before close: rename is only atomic with respect to data
     * that has actually reached the medium. */
    if (fflush(f) != 0) { fclose(f); return ESP_FAIL; }
    fsync(fileno(f));
    fclose(f);
    return ESP_OK;
}

/*
 * One writer at a time, across tasks.
 *
 * Both writers stage through the same /config.new and rename it into place,
 * so two at once means one of them renames a file the other still has open.
 * LittleFS refuses it and the loser reports failure -- for a push, that was a
 * 200 turning into "could not be saved" while the config had in fact been
 * saved by the other writer a millisecond earlier.
 *
 * The flag stays as the "an async write is already pending" check; the mutex
 * is what actually serialises the file operations.
 */
static volatile bool s_writing;
static SemaphoreHandle_t s_write_mux;

esp_err_t config_flush_sync(void)
{
    if (!storage_cfg_ready()) return ESP_ERR_INVALID_STATE;
    if (s_write_mux && xSemaphoreTake(s_write_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "a config write is stuck; not saving");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = write_config(PATH_NEW);
    if (err == ESP_OK) {
        /* LittleFS rename is atomic over an existing file: a power cut yields
         * either the whole old file or the whole new one, never a torn one. */
        if (rename(PATH_NEW, PATH_CUR) != 0) {
            ESP_LOGE(TAG, "rename failed; config not updated");
            remove(PATH_NEW);
            err = ESP_FAIL;
        } else {
            s_dirty = false;
            ESP_LOGI(TAG, "saved (%u endpoints, %u panels, %u screens)",
                     (unsigned)s_cfg.n_endpoints, (unsigned)s_cfg.n_panels,
                     (unsigned)s_cfg.n_screens);
        }
    }

    if (s_write_mux) xSemaphoreGive(s_write_mux);
    return err;
}


static void flush_task(void *arg)
{
    (void)arg;
    config_flush_sync();
    s_writing = false;
    vTaskDelete(NULL);
}

void config_flush(void)
{
    if (s_writing) return;           /* one writer at a time */
    s_writing = true;
    /*
     * 4KB is comfortable for LittleFS + stdio and is freed as soon as the
     * write completes. Doing this on the LVGL task instead risks its 6KB
     * stack and stalls rendering for the duration of a flash erase.
     */
    if (xTaskCreate(flush_task, "cfg_write", 4096, NULL, 4, NULL) != pdPASS) {
        s_writing = false;
        ESP_LOGE(TAG, "could not spawn the config writer");
    }
}

static void flush_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_dirty) config_flush();
}

uint32_t config_generation(void) { return s_generation; }

void config_touch(void)
{
    s_dirty = true;
    s_generation++;
    if (s_flush_timer) lv_timer_reset(s_flush_timer);
}

/* ------------------------------------------------------------- reading */

static void get_str(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    }
}

static int get_int(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static bool get_bool(const cJSON *o, const char *k, bool def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static float get_float(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;  /* null => unset */
}

/*
 * `full` distinguishes a complete configuration from a layout document.
 *
 * A layout carries only screens and panels, so it is parsed over a copy of
 * the live config: whatever it omits keeps its current value, and switching
 * layouts cannot silently drop the endpoint being polled.
 */
static bool parse_into_ex(config_t *cfg, const char *json, size_t len, bool full,
                          parse_err_t *pe)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        perr(pe, "the body is not valid JSON");
        return false;
    }
    if (!cJSON_IsObject(root)) {
        perr(pe, "the document must be a JSON object");
        cJSON_Delete(root);
        return false;
    }
    check_keys(root, k_keys_root, pe, "document");

    if (full) {
        int schema = get_int(root, "schema", 0);
        if (schema < 1 || schema > CFG_SCHEMA_VERSION) {
            ESP_LOGW(TAG, "schema %d is not readable by this build (max %d)",
                     schema, CFG_SCHEMA_VERSION);
            cJSON_Delete(root);
            return false;
        }
        set_defaults_into(cfg);
        cfg->schema  = (uint16_t)schema;
        cfg->next_id = (uint16_t)get_int(root, "next_id", 1);
        get_str(root, "active_layout", cfg->active_layout,
                sizeof(cfg->active_layout));
    }

    const cJSON *d = full ? cJSON_GetObjectItem(root, "device") : NULL;
    if (cJSON_IsObject(d)) {
        check_keys(d, k_keys_device, pe, "device");
        get_str(d, "theme", cfg->device.theme, sizeof(cfg->device.theme));
        cfg->device.poll_default_s = (uint16_t)get_int(d, "poll_default_s", 10);
        cfg->device.rotate_enabled = get_bool(d, "rotate_enabled", false);
        cfg->device.rotate_dwell_s = (uint16_t)get_int(d, "rotate_dwell_s", 20);
    }

    const cJSON *arr = full ? cJSON_GetObjectItem(root, "endpoints") : NULL;
    const cJSON *it = NULL;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cfg->n_endpoints >= CFG_MAX_ENDPOINTS) {
                perr(pe, "this build holds %d endpoints; the document has %d",
                     CFG_MAX_ENDPOINTS, cJSON_GetArraySize(arr));
                break;
            }
            check_keys(it, k_keys_endpoint, pe, "endpoint");
            cfg_endpoint_t *e = &cfg->endpoints[cfg->n_endpoints];
            memset(e, 0, sizeof(*e));
            e->id = (uint16_t)get_int(it, "id", cfg->next_id++);
            get_str(it, "name", e->name, sizeof(e->name));
            char kind[12] = "text";
            get_str(it, "kind", kind, sizeof(kind));
            e->kind = strcmp(kind, "promapi") == 0 ? EP_PROMAPI : EP_TEXT;
            get_str(it, "url", e->url, sizeof(e->url));
            e->poll_s       = (uint16_t)get_int(it, "poll_s", 0);
            e->timeout_ms   = (uint16_t)get_int(it, "timeout_ms", 8000);
            e->auth         = (auth_kind_t)get_int(it, "auth", AUTH_NONE);
            e->insecure_tls = get_bool(it, "insecure_tls", false);
            e->enabled      = get_bool(it, "enabled", true);
            if (e->url[0]) cfg->n_endpoints++;
        }
    }

    arr = cJSON_GetObjectItem(root, "screens");
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        cfg->n_screens = 0;
        cJSON_ArrayForEach(it, arr) {
            if (cfg->n_screens >= CFG_MAX_SCREENS) {
                perr(pe, "this build holds %d screens; the document has %d",
                     CFG_MAX_SCREENS, cJSON_GetArraySize(arr));
                break;
            }
            check_keys(it, k_keys_screen, pe, "screen");
            cfg_screen_t *sc = &cfg->screens[cfg->n_screens++];
            memset(sc, 0, sizeof(*sc));
            get_str(it, "title", sc->title, sizeof(sc->title));
            sc->pinned = get_bool(it, "pinned", false);
        }
    }

    arr = cJSON_GetObjectItem(root, "panels");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (cfg->n_panels >= CFG_MAX_PANELS) {
                /* Truncating is worse than refusing: the push is acknowledged
                 * and the tiles past the limit simply never appear. */
                perr(pe, "this build holds %d panels across all screens; "
                         "the document has %d",
                     CFG_MAX_PANELS, cJSON_GetArraySize(arr));
                break;
            }
            check_keys(it, k_keys_panel, pe, "panel");
            cfg_panel_t *p = &cfg->panels[cfg->n_panels];
            memset(p, 0, sizeof(*p));
            p->id    = (uint16_t)get_int(it, "id", cfg->next_id++);
            p->ep_id = (uint16_t)get_int(it, "ep", 0);
            get_str(it, "sel", p->sel, sizeof(p->sel));
            get_str(it, "title", p->title, sizeof(p->title));
            get_str(it, "unit", p->unit, sizeof(p->unit));

            /* Named in every message from here down, because "panel 3" means
             * counting array entries and "Decode TPS" does not. */
            char where[72];
            if (p->title[0]) snprintf(where, sizeof(where), "panel \"%s\"", p->title);
            else snprintf(where, sizeof(where), "panel %d", cfg->n_panels);

            p->op   = (panel_op_t)name_to_enum_ck(it, "op", k_ops, OP_NONE, pe, where);
            p->kind = (tile_kind_t)name_to_enum_ck(it, "kind", k_kinds, TILE_STAT, pe, where);
            p->fmt  = (fmt_mode_t)name_to_enum_ck(it, "fmt", k_fmts, FMT_AUTO, pe, where);

            const cJSON *terms = cJSON_GetObjectItem(it, "terms"), *tit = NULL;
            if (cJSON_IsArray(terms) && cJSON_GetArraySize(terms) > 0) {
                cJSON_ArrayForEach(tit, terms) {
                    if (p->n_terms >= CFG_MAX_TERMS) {
                        perr(pe, "%s: a panel combines at most %d terms",
                             where, CFG_MAX_TERMS);
                        break;
                    }
                    check_keys(tit, k_keys_term, pe, where);
                    cfg_term_t *tm = &p->terms[p->n_terms];
                    memset(tm, 0, sizeof(*tm));
                    get_str(tit, "sel", tm->sel, sizeof(tm->sel));
                    tm->reduce = (uint8_t)name_to_enum_ck(tit, "reduce", k_reduces, RED_SUM, pe, where);
                    tm->agg    = (uint8_t)name_to_enum_ck(tit, "agg", k_aggs, AGG_LAST, pe, where);
                    tm->window_s = (uint16_t)get_int(tit, "window_s", 0);
                    float tq     = get_float(tit, "q");
                    tm->q        = isnan(tq) ? 0.0f : tq;
                    if (tm->sel[0]) p->n_terms++;
                }
            } else {
                /*
                 * Schema 1 carried one or two selectors directly on the
                 * panel. Migrate rather than reject: a config written by the
                 * touch UI last week must keep working.
                 */
                cfg_term_t *a = &p->terms[0];
                memset(a, 0, sizeof(*a));
                get_str(it, "sel", a->sel, sizeof(a->sel));
                a->reduce   = RED_FIRST;   /* what schema 1 actually did */
                a->agg      = (uint8_t)name_to_enum_ck(it, "agg", k_aggs, AGG_LAST, pe, where);
                a->window_s = (uint16_t)get_int(it, "window_s", 0);
                float q     = get_float(it, "q");
                a->q        = isnan(q) ? 0.0f : q;
                if (a->sel[0]) p->n_terms = 1;

                char selb[CFG_SEL_MAX] = "";
                get_str(it, "sel_b", selb, sizeof(selb));
                if (selb[0] && p->n_terms == 1) {
                    cfg_term_t *b = &p->terms[1];
                    *b = *a;
                    strncpy(b->sel, selb, sizeof(b->sel) - 1);
                    p->n_terms = 2;
                }
            }
            p->vmin = get_float(it, "vmin");
            p->vmax = get_float(it, "vmax");
            p->warn = get_float(it, "warn");
            p->crit = get_float(it, "crit");
            p->multi          = get_bool(it, "multi", false);
            p->scale = (int8_t)name_to_enum_ck(it, "scale", k_scales, FMT_PIN_AUTO, pe, where);
            p->group = get_bool(it, "group", false);
            p->lower_is_worse = get_bool(it, "lower_is_worse", false);
            p->screen = (uint8_t)get_int(it, "screen", 0);
            p->col = (uint8_t)get_int(it, "col", 0);
            p->row = (uint8_t)get_int(it, "row", 0);
            p->w   = (uint8_t)get_int(it, "w", 1);
            p->h   = (uint8_t)get_int(it, "h", 1);
            if (p->n_terms > 0) {
                strncpy(p->sel, p->terms[0].sel, sizeof(p->sel) - 1);
                cfg->n_panels++;
            }
        }
    }

    cJSON_Delete(root);
    return pe == NULL || !pe->bad;
}

static bool parse_into(config_t *cfg, const char *json, size_t len,
                       parse_err_t *pe)
{
    return parse_into_ex(cfg, json, len, true, pe);
}

static bool parse_into_partial(config_t *cfg, const char *json, size_t len,
                               parse_err_t *pe)
{
    return parse_into_ex(cfg, json, len, false, pe);
}

static bool load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 256 * 1024) { fclose(f); return false; }

    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';

    char why[CFG_ERR_MAX] = "";
    parse_err_t pe = { why, sizeof(why), false };
    bool ok = parse_into(&s_cfg, buf, rd, &pe);
    free(buf);
    if (ok) ESP_LOGI(TAG, "loaded %s (%u bytes)", path, (unsigned)rd);
    else    ESP_LOGW(TAG, "%s rejected: %s", path, why[0] ? why : "unparseable");
    return ok;
}

esp_err_t config_load(void)
{
    set_defaults();
    s_was_reset = false;

    if (s_write_mux == NULL) s_write_mux = xSemaphoreCreateMutex();

    /* The flush timer lives in the LVGL task, which is where every config
     * change originates. */
    if (s_flush_timer == NULL) {
        s_flush_timer = lv_timer_create(flush_timer_cb, 2000, NULL);
    }

    if (!storage_cfg_ready()) {
        ESP_LOGE(TAG, "no config partition; settings will not persist");
        s_was_reset = true;
        return ESP_ERR_INVALID_STATE;
    }

    if (load_file(PATH_CUR)) {
        if (s_cfg.schema < CFG_SCHEMA_VERSION) {
            /* Loaded through a migration, so what is on flash is still the old
             * shape. Rewrite it, or every fetch of /config returns something
             * that does not match what the device is actually running. */
            ESP_LOGI(TAG, "migrating stored config from schema %u to %u",
                     (unsigned)s_cfg.schema, CFG_SCHEMA_VERSION);
            s_cfg.schema = CFG_SCHEMA_VERSION;
            config_touch();
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "%s missing or unreadable, trying the backup", PATH_CUR);
    if (load_file(PATH_BAK)) {
        ESP_LOGW(TAG, "recovered from %s", PATH_BAK);
        config_touch();      /* rewrite the primary from the backup */
        return ESP_OK;
    }

    /*
     * Both gone. Silently starting empty on a device whose entire state was
     * hand-entered is never acceptable, so this is reported and the UI says
     * so rather than just looking unconfigured.
     */
    ESP_LOGE(TAG, "no readable configuration; starting from defaults");
    set_defaults();
    s_was_reset = true;
    return ESP_ERR_NOT_FOUND;
}

bool config_was_reset(void) { return s_was_reset; }

config_t *config_get(void) { return &s_cfg; }

void config_mark_good_boot(void)
{
    if (!storage_cfg_ready() || s_was_reset) return;
    /* Only after the UI is up and the config has proven loadable, so the
     * backup is a known-good-BOOT config and not merely the previous save. */
    if (write_config(PATH_BAK) == ESP_OK) ESP_LOGD(TAG, "backup refreshed");
}

/*
 * Sanity-check a parsed document before it is allowed to replace the live
 * one. Rejecting with a reason beats accepting something that renders as an
 * empty screen and leaves the user guessing.
 */
static void panel_where(const cfg_panel_t *p, int i, char *out, size_t cap)
{
    if (p->title[0]) snprintf(out, cap, "panel \"%s\"", p->title);
    else             snprintf(out, cap, "panel %d (id %u)", i, (unsigned)p->id);
}

static bool validate(const config_t *c, char *err, size_t cap)
{
    if (c->schema < 1 || c->schema > CFG_SCHEMA_VERSION) {
        snprintf(err, cap, "schema %u is not supported (this build reads 1-%u)",
                 (unsigned)c->schema, CFG_SCHEMA_VERSION);
        return false;
    }
    if (c->n_screens == 0) {
        snprintf(err, cap, "at least one screen is required");
        return false;
    }

    for (int i = 0; i < c->n_panels; i++) {
        const cfg_panel_t *p = &c->panels[i];
        char w_[80];
        panel_where(p, i, w_, sizeof(w_));

        if (p->n_terms == 0 || p->terms[0].sel[0] == '\0') {
            snprintf(err, cap, "%s has no terms", w_);
            return false;
        }

        /*
         * Every selector has to parse HERE.
         *
         * The poller parses them too, and drops the panel when one does not --
         * on a worker task, after the push has been acknowledged, so the tile
         * simply never shows data and nothing ever says why.
         */
        for (int k = 0; k < p->n_terms; k++) {
            char scratch[CFG_SEL_MAX];
            const char *name; uint16_t name_len;
            prom_label_t labels[8]; uint8_t n_labels;
            strncpy(scratch, p->terms[k].sel, sizeof(scratch) - 1);
            scratch[sizeof(scratch) - 1] = '\0';
            if (!prom_parse_selector(scratch, scratch, sizeof(scratch),
                                     &name, &name_len, labels, 8, &n_labels)) {
                snprintf(err, cap, "%s term %d: cannot parse the selector "
                                   "\"%s\"; expected metric or "
                                   "metric{label=\"value\"}",
                         w_, k, p->terms[k].sel);
                return false;
            }
            if (p->terms[k].q < 0.0f || p->terms[k].q > 1.0f) {
                snprintf(err, cap, "%s term %d: q is %.3f; a quantile is 0 to 1",
                         w_, k, (double)p->terms[k].q);
                return false;
            }
        }

        uint8_t w = p->w ? p->w : 1, h = p->h ? p->h : 1;
        if (p->col + w > GRID_COLS || p->row + h > GRID_ROWS) {
            snprintf(err, cap,
                     "%s: a %ux%u tile at col %u row %u runs past the %dx%d grid",
                     w_, w, h, p->col, p->row, GRID_COLS, GRID_ROWS);
            return false;
        }

        /* The schema documents the minimum spans; without this it documented
         * a refusal that never happened and the widget quietly changed. */
        const tile_vt_t *vt = tile_vt(p->kind);
        if (vt && (w < vt->min_w || h < vt->min_h)) {
            snprintf(err, cap, "%s: %s needs at least %ux%u, not %ux%u",
                     w_, vt->name, vt->min_w, vt->min_h, w, h);
            return false;
        }

        if (p->screen >= c->n_screens) {
            snprintf(err, cap, "%s names screen %u, and only %u exist",
                     w_, p->screen, c->n_screens);
            return false;
        }
        if (p->op != OP_NONE && p->n_terms < 2) {
            snprintf(err, cap, "%s has an operator but one term", w_);
            return false;
        }
        if (!isnan(p->vmin) && !isnan(p->vmax) && p->vmin >= p->vmax) {
            snprintf(err, cap, "%s: vmin %g is not below vmax %g",
                     w_, (double)p->vmin, (double)p->vmax);
            return false;
        }
        if (c->n_endpoints > 0 && p->ep_id != 0) {
            bool found = false;
            for (int e = 0; e < c->n_endpoints; e++) {
                if (c->endpoints[e].id == p->ep_id) { found = true; break; }
            }
            if (!found) {
                snprintf(err, cap, "%s names endpoint %u, which does not exist",
                         w_, (unsigned)p->ep_id);
                return false;
            }
        }

        for (int j = 0; j < i; j++) {
            const cfg_panel_t *o = &c->panels[j];
            if (o->id == p->id) {
                snprintf(err, cap, "%s: id %u is used twice", w_, (unsigned)p->id);
                return false;
            }
            /*
             * Overlap, which the touch UI cannot produce but a push can. Two
             * tiles in one cell draw on top of each other, and which one wins
             * depends on the order they were built in.
             */
            if (o->screen != p->screen) continue;
            uint8_t ow = o->w ? o->w : 1, oh = o->h ? o->h : 1;
            bool hit = !(p->col + w <= o->col || o->col + ow <= p->col ||
                         p->row + h <= o->row || o->row + oh <= p->row);
            if (hit) {
                char o_[80];
                panel_where(o, j, o_, sizeof(o_));
                snprintf(err, cap, "%s overlaps %s on screen %u",
                         w_, o_, p->screen);
                return false;
            }
        }
    }
    return true;
}

esp_err_t config_apply_json(const char *json, size_t len, char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';

    /* ~10KB, so the heap rather than whichever stack called us. */
    config_t *tmp = malloc(sizeof(config_t));
    if (tmp == NULL) {
        snprintf(err, cap, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    parse_err_t pe = { err, cap, false };
    if (!parse_into(tmp, json, len, &pe)) {
        if (!pe.bad) snprintf(err, cap, "could not parse the document");
        free(tmp);
        return ESP_ERR_INVALID_ARG;
    }
    if (!validate(tmp, err, cap)) {
        free(tmp);
        return ESP_ERR_INVALID_ARG;
    }

    /* Whole or not at all. */
    s_cfg = *tmp;
    free(tmp);

    /*
     * Marked dirty, not flushed. The caller decides how durable the write has
     * to be -- the HTTP handlers write synchronously so a 200 means it
     * landed -- and firing the debounced writer here as well put two writers
     * on the same staging file.
     */
    config_touch();
    return ESP_OK;
}

void config_enum_values(const char *which, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    const enum_name_t *tab =
        strcmp(which, "kind")   == 0 ? k_kinds   :
        strcmp(which, "fmt")    == 0 ? k_fmts    :
        strcmp(which, "reduce") == 0 ? k_reduces :
        strcmp(which, "agg")    == 0 ? k_aggs    :
        strcmp(which, "op")     == 0 ? k_ops     :
        strcmp(which, "scale")  == 0 ? k_scales  : NULL;
    if (tab == NULL) return;

    size_t w = 0;
    for (int i = 0; tab[i].name && w < cap - 24; i++) {
        w += (size_t)snprintf(out + w, cap - w, "%s\"%s\"", i ? "," : "",
                              tab[i].name);
    }
}

/* ---------------------------------------------------------------- layouts */

#define LAYOUT_DIR STORAGE_CFG_PATH "/layouts"

/*
 * Layout names become filenames, so they are restricted to characters that
 * cannot escape the directory or confuse the filesystem. Rejecting here beats
 * sanitising: a name that silently becomes a different name is worse than one
 * that is refused.
 */
static bool layout_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    size_t n = strlen(name);
    if (n >= CFG_LAYOUT_NAME_MAX - 6) return false;   /* room for ".json" */
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

static void layout_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, LAYOUT_DIR "/%s.json", name);
}

const char *config_active_layout(void) { return s_cfg.active_layout; }

int config_layout_list(char names[][CFG_LAYOUT_NAME_MAX], int max)
{
    DIR *d = opendir(LAYOUT_DIR);
    if (d == NULL) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot == NULL || strcmp(dot, ".json") != 0) continue;
        size_t len = (size_t)(dot - e->d_name);
        if (len == 0 || len >= CFG_LAYOUT_NAME_MAX) continue;
        memcpy(names[n], e->d_name, len);
        names[n][len] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

bool config_layout_write_json(const char *name, FILE *f)
{
    if (name == NULL) {              /* the live screens and panels */
        fputs("{\n", f);
        write_presentation(f);
        fputs("}\n", f);
        return true;
    }
    if (!layout_name_ok(name)) return false;

    char path[96];
    layout_path(name, path, sizeof(path));
    FILE *in = fopen(path, "rb");
    if (in == NULL) return false;

    char buf[512];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, got, f);
    fclose(in);
    return true;
}

/*
 * How many tiles a stored layout holds, without parsing it.
 *
 * The picker needs something to tell two saved layouts apart, and a name
 * alone does not do it. Counting a key over a sliding window costs a few
 * hundred bytes of stack against cJSON's whole tree, and a wrong count here
 * mislabels a row -- it cannot corrupt anything.
 *
 * The key is "terms", not "sel": every panel writes exactly one terms array,
 * whereas a panel that combines two series writes two sel strings, and
 * counting those reports six tiles for five.
 */
int config_layout_panel_count(const char *name)
{
    if (!layout_name_ok(name)) return -1;

    char path[96];
    layout_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;

    static const char needle[] = "\"terms\"";
    const size_t nlen = sizeof(needle) - 1;

    char buf[256 + sizeof(needle)];
    size_t carry = 0;                 /* bytes kept from the previous chunk */
    int n = 0, got;
    while ((got = (int)fread(buf + carry, 1, sizeof(buf) - carry, f)) > 0) {
        size_t have = carry + (size_t)got;
        for (size_t i = 0; i + nlen <= have; i++) {
            if (memcmp(buf + i, needle, nlen) == 0) { n++; i += nlen - 1; }
        }
        /* A match can straddle a read boundary, so retain nlen-1 bytes. */
        carry = have >= nlen - 1 ? nlen - 1 : have;
        memmove(buf, buf + have - carry, carry);
    }
    fclose(f);
    return n;
}

esp_err_t config_layout_save(const char *name)
{
    if (!layout_name_ok(name)) return ESP_ERR_INVALID_ARG;
    if (!storage_cfg_ready()) return ESP_ERR_INVALID_STATE;

    mkdir(LAYOUT_DIR, 0777);          /* harmless if it already exists */

    char path[96], tmp[96];
    layout_path(name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), LAYOUT_DIR "/.tmp.json");

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return ESP_FAIL;
    fputs("{\n", f);
    write_presentation(f);
    fputs("}\n", f);
    if (fflush(f) != 0) { fclose(f); remove(tmp); return ESP_FAIL; }
    fsync(fileno(f));
    fclose(f);

    /* Same atomic-rename discipline as the main config: a power cut leaves
     * either the old layout or the new one. */
    if (rename(tmp, path) != 0) { remove(tmp); return ESP_FAIL; }

    strncpy(s_cfg.active_layout, name, sizeof(s_cfg.active_layout) - 1);
    config_touch();
    ESP_LOGI(TAG, "saved layout '%s'", name);
    return ESP_OK;
}

esp_err_t config_layout_delete(const char *name)
{
    if (!layout_name_ok(name)) return ESP_ERR_INVALID_ARG;
    char path[96];
    layout_path(name, path, sizeof(path));
    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;
    if (strcmp(s_cfg.active_layout, name) == 0) {
        /* The screens stay on display; only the association is gone, so the
         * user is not left staring at a blank panel because of a delete. */
        s_cfg.active_layout[0] = '\0';
        config_touch();
    }
    return ESP_OK;
}

esp_err_t config_layout_load(const char *name)
{
    if (!layout_name_ok(name)) return ESP_ERR_INVALID_ARG;

    char path[96];
    layout_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128 * 1024) { fclose(f); return ESP_ERR_INVALID_SIZE; }

    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    char err[CFG_ERR_MAX] = "";
    esp_err_t rc = config_layout_apply_json(buf, rd, err, sizeof(err));
    free(buf);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "layout '%s' did not load: %s", name, err);
        return rc;
    }

    strncpy(s_cfg.active_layout, name, sizeof(s_cfg.active_layout) - 1);
    config_touch();
    ESP_LOGI(TAG, "activated layout '%s' (%u panels)", name,
             (unsigned)s_cfg.n_panels);
    return ESP_OK;
}

esp_err_t config_layout_apply_json(const char *json, size_t len,
                                   char *err, size_t cap)
{
    if (err && cap) err[0] = '\0';

    config_t *tmp = malloc(sizeof(config_t));
    if (tmp == NULL) { snprintf(err, cap, "out of memory"); return ESP_ERR_NO_MEM; }

    /*
     * A layout document has no endpoints or device block, so parse it over a
     * copy of the live config: whatever it omits keeps its current value, and
     * switching layouts cannot silently drop the endpoint you are polling.
     */
    *tmp = s_cfg;
    tmp->n_screens = 0;
    tmp->n_panels  = 0;

    parse_err_t pe = { err, cap, false };
    if (!parse_into_partial(tmp, json, len, &pe)) {
        if (!pe.bad) snprintf(err, cap, "could not parse the layout");
        free(tmp);
        return ESP_ERR_INVALID_ARG;
    }
    if (tmp->n_screens == 0) {
        tmp->n_screens = 1;
        strncpy(tmp->screens[0].title, "Home", sizeof(tmp->screens[0].title) - 1);
    }
    if (!validate(tmp, err, cap)) { free(tmp); return ESP_ERR_INVALID_ARG; }

    s_cfg = *tmp;
    free(tmp);
    config_touch();
    return ESP_OK;
}

cfg_term_t *config_term0(cfg_panel_t *p)
{
    if (p->n_terms == 0) {
        memset(&p->terms[0], 0, sizeof(p->terms[0]));
        p->terms[0].reduce = RED_SUM;
        p->terms[0].agg    = AGG_LAST;
        p->n_terms = 1;
    }
    /* Keep the panel's mirror of the selector in step, since the browser and
     * the dashboard both key off it. */
    strncpy(p->sel, p->terms[0].sel, sizeof(p->sel) - 1);
    return &p->terms[0];
}

cfg_panel_t *config_panel_add(void)
{
    if (s_cfg.n_panels >= CFG_MAX_PANELS) return NULL;
    cfg_panel_t *p = &s_cfg.panels[s_cfg.n_panels++];
    memset(p, 0, sizeof(*p));
    p->id   = s_cfg.next_id++;
    p->kind = TILE_STAT;
    p->fmt  = FMT_AUTO;
    /* Zero is a legal pin -- it means "no prefix" -- so auto has to be set
     * explicitly rather than inherited from the memset above. */
    p->scale = FMT_PIN_AUTO;
    p->vmin = p->vmax = p->warn = p->crit = NAN;
    p->w = p->h = 1;
    return p;
}

void config_panel_remove(uint16_t id)
{
    for (int i = 0; i < s_cfg.n_panels; i++) {
        if (s_cfg.panels[i].id != id) continue;
        for (int j = i; j + 1 < s_cfg.n_panels; j++) {
            s_cfg.panels[j] = s_cfg.panels[j + 1];
        }
        s_cfg.n_panels--;
        config_touch();
        return;
    }
}

bool config_has_panel(uint16_t ep_id, const char *sel)
{
    for (int i = 0; i < s_cfg.n_panels; i++) {
        if (s_cfg.panels[i].ep_id == ep_id &&
            strcmp(s_cfg.panels[i].sel, sel) == 0) return true;
    }
    return false;
}

/*
 * First-fit into a 4x3 occupancy bitmap for the panel's screen.
 *
 * Deliberately first-fit and row-major rather than anything cleverer: the
 * user can move tiles afterwards, and an auto-layout that reshuffles existing
 * tiles when a new one arrives is infuriating. Only free cells are used.
 */
uint32_t config_panel_fingerprint(const cfg_panel_t *p)
{
    uint32_t h = 2166136261u;                     /* FNV-1a */
    #define FEED(byte) do { h ^= (uint32_t)(uint8_t)(byte); h *= 16777619u; } while (0)
    for (int k = 0; k < p->n_terms && k < CFG_MAX_TERMS; k++) {
        const cfg_term_t *t = &p->terms[k];
        for (const char *c = t->sel; *c; c++) FEED(*c);
        FEED(0);
        FEED(t->reduce); FEED(t->agg);
        FEED(t->window_s & 0xFF); FEED(t->window_s >> 8);
        const unsigned char *q = (const unsigned char *)&t->q;
        for (size_t i = 0; i < sizeof(t->q); i++) FEED(q[i]);
    }
    FEED(p->op); FEED(p->multi ? 1 : 0); FEED(p->ep_id);
    #undef FEED
    return h;
}

bool config_ensure_screen(uint8_t idx)
{
    if (idx >= CFG_MAX_SCREENS) return false;
    while (s_cfg.n_screens <= idx) {
        cfg_screen_t *sc = &s_cfg.screens[s_cfg.n_screens];
        memset(sc, 0, sizeof(*sc));
        snprintf(sc->title, sizeof(sc->title), "Screen %u",
                 (unsigned)s_cfg.n_screens + 1);
        s_cfg.n_screens++;
    }
    return true;
}

bool config_place_panel(cfg_panel_t *p)
{
    bool used[GRID_ROWS][GRID_COLS];
    memset(used, 0, sizeof(used));

    for (int i = 0; i < s_cfg.n_panels; i++) {
        const cfg_panel_t *o = &s_cfg.panels[i];
        if (o == p || o->screen != p->screen) continue;
        for (int r = o->row; r < o->row + o->h && r < GRID_ROWS; r++) {
            for (int c = o->col; c < o->col + o->w && c < GRID_COLS; c++) {
                used[r][c] = true;
            }
        }
    }

    for (int r = 0; r + p->h <= GRID_ROWS; r++) {
        for (int c = 0; c + p->w <= GRID_COLS; c++) {
            bool fits = true;
            for (int rr = r; rr < r + p->h && fits; rr++) {
                for (int cc = c; cc < c + p->w; cc++) {
                    if (used[rr][cc]) { fits = false; break; }
                }
            }
            if (fits) { p->row = (uint8_t)r; p->col = (uint8_t)c; return true; }
        }
    }
    return false;
}

/* Panels on the same screen whose area covers any of the given rectangle. */
static int panels_overlapping(const cfg_panel_t *me, int col, int row,
                              int w, int h, cfg_panel_t **first)
{
    int n = 0;
    *first = NULL;
    for (int i = 0; i < s_cfg.n_panels; i++) {
        cfg_panel_t *o = &s_cfg.panels[i];
        if (o == me || !o->sel[0] || o->screen != me->screen) continue;
        int ow = o->w ? o->w : 1, oh = o->h ? o->h : 1;
        bool overlap = !(col + w <= o->col || o->col + ow <= col ||
                         row + h <= o->row || o->row + oh <= row);
        if (overlap) { if (!*first) *first = o; n++; }
    }
    return n;
}

bool config_panel_fits(const cfg_panel_t *p, int col, int row)
{
    int w = p->w ? p->w : 1, h = p->h ? p->h : 1;
    if (col < 0 || row < 0 || col + w > GRID_COLS || row + h > GRID_ROWS) {
        return false;
    }
    cfg_panel_t *first = NULL;
    return panels_overlapping(p, col, row, w, h, &first) == 0;
}

bool config_move_panel(cfg_panel_t *p, int col, int row)
{
    int w = p->w ? p->w : 1, h = p->h ? p->h : 1;
    if (col < 0 || row < 0 || col + w > GRID_COLS || row + h > GRID_ROWS) {
        return false;
    }

    cfg_panel_t *other = NULL;
    int n = panels_overlapping(p, col, row, w, h, &other);

    if (n == 0) {
        p->col = (uint8_t)col; p->row = (uint8_t)row;
        config_touch();
        return true;
    }

    /*
     * Exactly one occupant of the same shape: trade places.
     *
     * Anything else is refused rather than guessed at. Displacing two tiles to
     * make room for a 2x2 means choosing where they go, and choosing wrong
     * rearranges a layout someone built deliberately -- which is worse than
     * being told the move is not available.
     */
    if (n == 1 && other &&
        (other->w ? other->w : 1) == w && (other->h ? other->h : 1) == h) {
        other->col = p->col; other->row = p->row;
        p->col = (uint8_t)col; p->row = (uint8_t)row;
        config_touch();
        return true;
    }
    return false;
}

bool config_nudge_panel(cfg_panel_t *p, int dcol, int drow)
{
    return config_move_panel(p, (int)p->col + dcol, (int)p->row + drow);
}

cfg_endpoint_t *config_endpoint_by_id(uint16_t id)
{
    for (int i = 0; i < s_cfg.n_endpoints; i++) {
        if (s_cfg.endpoints[i].id == id) return &s_cfg.endpoints[i];
    }
    return NULL;
}

cfg_endpoint_t *config_endpoint_add(void)
{
    if (s_cfg.n_endpoints >= CFG_MAX_ENDPOINTS) return NULL;
    cfg_endpoint_t *e = &s_cfg.endpoints[s_cfg.n_endpoints++];
    memset(e, 0, sizeof(*e));
    e->id         = s_cfg.next_id++;
    e->kind       = EP_TEXT;
    e->poll_s     = 0;
    e->timeout_ms = 8000;
    e->enabled    = true;
    return e;
}

void config_endpoint_remove(uint16_t id)
{
    for (int i = 0; i < s_cfg.n_endpoints; i++) {
        if (s_cfg.endpoints[i].id != id) continue;
        for (int j = i; j + 1 < s_cfg.n_endpoints; j++) {
            s_cfg.endpoints[j] = s_cfg.endpoints[j + 1];
        }
        s_cfg.n_endpoints--;
        config_touch();
        return;
    }
}
