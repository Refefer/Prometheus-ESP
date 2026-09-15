#include "config.h"
#include "storage.h"
#include "ui_layout.h"

#include "cJSON.h"
#include "esp_log.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "config";

#define PATH_CUR  STORAGE_CFG_PATH "/config.json"
#define PATH_NEW  STORAGE_CFG_PATH "/config.new"
#define PATH_BAK  STORAGE_CFG_PATH "/config.bak"

static config_t    s_cfg;
static bool        s_dirty;
static bool        s_was_reset;
static lv_timer_t *s_flush_timer;

/* ------------------------------------------------------------- defaults */

static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.schema  = CFG_SCHEMA_VERSION;
    s_cfg.next_id = 1;
    strncpy(s_cfg.device.theme, "night_ops", sizeof(s_cfg.device.theme) - 1);
    s_cfg.device.poll_default_s = 10;
    s_cfg.device.rotate_dwell_s = 20;

    s_cfg.n_screens = 1;
    strncpy(s_cfg.screens[0].title, "Home", sizeof(s_cfg.screens[0].title) - 1);
}

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

static esp_err_t write_config(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return ESP_FAIL;
    }

    fprintf(f, "{\n  \"schema\": %u,\n", (unsigned)s_cfg.schema);
    fprintf(f, "  \"next_id\": %u,\n", (unsigned)s_cfg.next_id);

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
        fprintf(f, "    { \"id\": %u, \"ep\": %u, \"sel\": ",
                (unsigned)p->id, (unsigned)p->ep_id);
        write_escaped(f, p->sel);
        fprintf(f, ", \"op\": %u", (unsigned)p->op);
        fputs(", \"title\": ", f);
        write_escaped(f, p->title);
        fputs(", \"unit\": ", f);
        write_escaped(f, p->unit);
        fprintf(f, ", \"kind\": %u, \"fmt\": %u", (unsigned)p->kind,
                (unsigned)p->fmt);

        fputs(", \"terms\": [", f);
        for (int k = 0; k < p->n_terms; k++) {
            const cfg_term_t *tm = &p->terms[k];
            if (k) fputs(", ", f);
            fputs("{ \"sel\": ", f);
            write_escaped(f, tm->sel);
            fprintf(f, ", \"reduce\": %u, \"agg\": %u, \"window_s\": %u",
                    (unsigned)tm->reduce, (unsigned)tm->agg,
                    (unsigned)tm->window_s);
            fputs(", \"q\": ", f); write_float(f, tm->q);
            fputs(" }", f);
        }
        fputs("]", f);
        fputs(", \"vmin\": ", f); write_float(f, p->vmin);
        fputs(", \"vmax\": ", f); write_float(f, p->vmax);
        fputs(", \"warn\": ", f); write_float(f, p->warn);
        fputs(", \"crit\": ", f); write_float(f, p->crit);
        fprintf(f, ", \"multi\": %s", p->multi ? "true" : "false");
        fprintf(f, ", \"lower_is_worse\": %s, \"screen\": %u,"
                   " \"col\": %u, \"row\": %u, \"w\": %u, \"h\": %u }%s\n",
                p->lower_is_worse ? "true" : "false",
                (unsigned)p->screen, (unsigned)p->col, (unsigned)p->row,
                (unsigned)p->w, (unsigned)p->h,
                i + 1 < s_cfg.n_panels ? "," : "");
    }
    fputs("  ]\n}\n", f);

    /* fflush + fsync before close: rename is only atomic with respect to
     * data that has actually reached the medium. */
    if (fflush(f) != 0) { fclose(f); return ESP_FAIL; }
    fsync(fileno(f));
    fclose(f);
    return ESP_OK;
}

esp_err_t config_flush_sync(void)
{
    if (!storage_cfg_ready()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = write_config(PATH_NEW);
    if (err != ESP_OK) return err;

    /* LittleFS rename is atomic over an existing file: a power cut yields
     * either the whole old file or the whole new one, never a torn one. */
    if (rename(PATH_NEW, PATH_CUR) != 0) {
        ESP_LOGE(TAG, "rename failed; config not updated");
        remove(PATH_NEW);
        return ESP_FAIL;
    }

    s_dirty = false;
    ESP_LOGI(TAG, "saved (%u endpoints, %u panels, %u screens)",
             (unsigned)s_cfg.n_endpoints, (unsigned)s_cfg.n_panels,
             (unsigned)s_cfg.n_screens);
    return ESP_OK;
}

static volatile bool s_writing;

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

void config_touch(void)
{
    s_dirty = true;
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

static bool parse_into(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) return false;

    int schema = get_int(root, "schema", 0);
    if (schema < 1 || schema > CFG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "schema %d is not readable by this build (max %d)",
                 schema, CFG_SCHEMA_VERSION);
        cJSON_Delete(root);
        return false;
    }

    set_defaults();
    s_cfg.schema  = (uint16_t)schema;
    s_cfg.next_id = (uint16_t)get_int(root, "next_id", 1);

    const cJSON *d = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsObject(d)) {
        get_str(d, "theme", s_cfg.device.theme, sizeof(s_cfg.device.theme));
        s_cfg.device.poll_default_s = (uint16_t)get_int(d, "poll_default_s", 10);
        s_cfg.device.rotate_enabled = get_bool(d, "rotate_enabled", false);
        s_cfg.device.rotate_dwell_s = (uint16_t)get_int(d, "rotate_dwell_s", 20);
    }

    const cJSON *arr = cJSON_GetObjectItem(root, "endpoints"), *it = NULL;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (s_cfg.n_endpoints >= CFG_MAX_ENDPOINTS) break;
            cfg_endpoint_t *e = &s_cfg.endpoints[s_cfg.n_endpoints];
            memset(e, 0, sizeof(*e));
            e->id = (uint16_t)get_int(it, "id", s_cfg.next_id++);
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
            if (e->url[0]) s_cfg.n_endpoints++;
        }
    }

    arr = cJSON_GetObjectItem(root, "screens");
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        s_cfg.n_screens = 0;
        cJSON_ArrayForEach(it, arr) {
            if (s_cfg.n_screens >= CFG_MAX_SCREENS) break;
            cfg_screen_t *sc = &s_cfg.screens[s_cfg.n_screens++];
            memset(sc, 0, sizeof(*sc));
            get_str(it, "title", sc->title, sizeof(sc->title));
            sc->pinned = get_bool(it, "pinned", false);
        }
    }

    arr = cJSON_GetObjectItem(root, "panels");
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(it, arr) {
            if (s_cfg.n_panels >= CFG_MAX_PANELS) break;
            cfg_panel_t *p = &s_cfg.panels[s_cfg.n_panels];
            memset(p, 0, sizeof(*p));
            p->id    = (uint16_t)get_int(it, "id", s_cfg.next_id++);
            p->ep_id = (uint16_t)get_int(it, "ep", 0);
            get_str(it, "sel", p->sel, sizeof(p->sel));
            p->op = (panel_op_t)get_int(it, "op", OP_NONE);
            get_str(it, "title", p->title, sizeof(p->title));
            get_str(it, "unit", p->unit, sizeof(p->unit));
            p->kind = (tile_kind_t)get_int(it, "kind", TILE_STAT);
            p->fmt  = (fmt_mode_t)get_int(it, "fmt", FMT_AUTO);

            const cJSON *terms = cJSON_GetObjectItem(it, "terms"), *tit = NULL;
            if (cJSON_IsArray(terms) && cJSON_GetArraySize(terms) > 0) {
                cJSON_ArrayForEach(tit, terms) {
                    if (p->n_terms >= CFG_MAX_TERMS) break;
                    cfg_term_t *tm = &p->terms[p->n_terms];
                    memset(tm, 0, sizeof(*tm));
                    get_str(tit, "sel", tm->sel, sizeof(tm->sel));
                    tm->reduce   = (uint8_t)get_int(tit, "reduce", RED_SUM);
                    tm->agg      = (uint8_t)get_int(tit, "agg", AGG_LAST);
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
                a->agg      = (uint8_t)get_int(it, "agg", AGG_LAST);
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
            p->lower_is_worse = get_bool(it, "lower_is_worse", false);
            p->screen = (uint8_t)get_int(it, "screen", 0);
            p->col = (uint8_t)get_int(it, "col", 0);
            p->row = (uint8_t)get_int(it, "row", 0);
            p->w   = (uint8_t)get_int(it, "w", 1);
            p->h   = (uint8_t)get_int(it, "h", 1);
            if (p->n_terms > 0) {
                strncpy(p->sel, p->terms[0].sel, sizeof(p->sel) - 1);
                s_cfg.n_panels++;
            }
        }
    }

    cJSON_Delete(root);
    return true;
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

    bool ok = parse_into(buf, rd);
    free(buf);
    if (ok) ESP_LOGI(TAG, "loaded %s (%u bytes)", path, (unsigned)rd);
    return ok;
}

esp_err_t config_load(void)
{
    set_defaults();
    s_was_reset = false;

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

    if (load_file(PATH_CUR)) return ESP_OK;

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
