#include "webcfg.h"

#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"
#include "poller.h"
#include "prom_ident.h"
#include "prom_text.h"
#include "secrets.h"
#include "storage.h"
#include "ui_layout.h"
#include "wifi_mgr.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "webcfg";

static httpd_handle_t s_server;
static bool           s_ever_used;

/*
 * Escape a string for embedding in JSON.
 *
 * Not optional here: a selector is metric{label="value"}, so every one of
 * them carries quotes. Emitting them raw produced a document that no JSON
 * parser would accept -- which an agent calling this API would hit on its
 * very first request.
 */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    if (cap == 0) return;
    for (const char *p = in; *p && w + 7 < cap; p++) {
        switch (*p) {
        case '"':  out[w++] = '\\'; out[w++] = '"';  break;
        case '\\': out[w++] = '\\'; out[w++] = '\\'; break;
        case '\n': out[w++] = '\\'; out[w++] = 'n';  break;
        case '\r': out[w++] = '\\'; out[w++] = 'r';  break;
        case '\t': out[w++] = '\\'; out[w++] = 't';  break;
        default:
            if ((unsigned char)*p < 0x20) {
                w += (size_t)snprintf(out + w, cap - w, "\\u%04x", *p);
            } else {
                out[w++] = *p;
            }
        }
    }
    out[w] = '\0';
}

/*
 * The biggest config we will accept. A full screen of twelve panels with four
 * terms each serialises to roughly 12KB; 64KB is generous headroom and still
 * small enough that the buffer is a rounding error against 5MB of PSRAM.
 */
#define WEBCFG_MAX_BODY (64 * 1024)

/* ------------------------------------------------------------------- auth */

static bool authorised(httpd_req_t *req)
{
    char want[SECRETS_TOKEN_MAX];
    if (secrets_get_token(want, sizeof(want)) != ESP_OK || want[0] == '\0') {
        return false;
    }

    char got[SECRETS_TOKEN_MAX] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Auth", got, sizeof(got)) != ESP_OK) {
        return false;
    }

    /*
     * Constant-time compare. The timing signal here is tiny and the attacker
     * is already on your LAN, but a length-independent compare costs three
     * lines and removes the question.
     */
    size_t n = strlen(want);
    if (strlen(got) != n) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned)(got[i] ^ want[i]);
    if (diff == 0) s_ever_used = true;
    return diff == 0;
}

bool webcfg_ever_used(void) { return s_ever_used; }

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"error\":\"missing or wrong X-Auth header\","
        "\"hint\":\"the token is shown on the device under the endpoint screen\"}\n");
    return ESP_OK;
}

/* --------------------------------------------------------------- handlers */

static esp_err_t get_config(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    FILE *f = fopen(STORAGE_CFG_PATH "/config.json", "rb");
    if (f == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no configuration stored yet\"}\n");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    /* Streamed in chunks rather than slurped: the file is the one thing here
     * whose size is not bounded by us. */
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/*
 * Writes the config to flash before the response goes out.
 *
 * Everything on the glass writes through a debounced flush, because ticking
 * forty checkboxes should not mean forty flash writes. A push is the opposite
 * shape: one atomic document, acknowledged once. Leaving it to the debounce
 * meant a 200 promised nothing -- a reset inside the next two seconds lost
 * the whole push, which is exactly how this was found. Something driving this
 * API cannot see the debounce, so the acknowledgement has to mean it landed.
 */
static bool persist(httpd_req_t *req)
{
    esp_err_t rc = config_flush_sync();
    if (rc == ESP_OK) return true;

    ESP_LOGE(TAG, "applied but could not save: %s", esp_err_to_name(rc));
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"error\":\"applied to the running panel but could not be saved; "
        "it will not survive a restart\"}\n");
    return false;
}

static esp_err_t post_config(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    if (req->content_len <= 0 || req->content_len > WEBCFG_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"error\":\"body missing or over 64KB\"}\n");
        return ESP_OK;
    }

    char *body = heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}\n");
        return ESP_OK;
    }

    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (r <= 0) {
            heap_caps_free(body);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    /*
     * Parse into a scratch config and only swap it in if it is whole. A push
     * that half-applies would leave the panel in a state that matches neither
     * the old config nor the new one, which is worse than rejecting it.
     */
    /* Roomy on purpose: a rejection quotes the offending selector or lists
     * every accepted enum name, and a truncated reason is a reason you have
     * to guess at. */
    char err[CFG_ERR_MAX] = "";
    esp_err_t rc = config_apply_json(body, (size_t)got, err, sizeof(err));
    heap_caps_free(body);

    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "rejected a pushed config: %s", err);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char eerr[CFG_ERR_MAX * 2];
        json_escape(err[0] ? err : "could not parse the configuration",
                    eerr, sizeof(eerr));
        char out[CFG_ERR_MAX * 2 + 32];
        snprintf(out, sizeof(out), "{\"error\":\"%s\"}\n", eerr);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

    if (!persist(req)) return ESP_OK;

    const config_t *c = config_get();
    ESP_LOGI(TAG, "applied a pushed config: %u panels, %u endpoints",
             (unsigned)c->n_panels, (unsigned)c->n_endpoints);

    httpd_resp_set_type(req, "application/json");
    char out[160];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"panels\":%u,\"endpoints\":%u,\"screens\":%u}\n",
             (unsigned)c->n_panels, (unsigned)c->n_endpoints,
             (unsigned)c->n_screens);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* --------------------------------------------------------------- layouts */

/* Last path segment, for /layouts/<name> and /layouts/<name>/activate. */
static void path_tail(const char *uri, const char *prefix,
                      char *name, size_t cap, char *verb, size_t vcap)
{
    name[0] = '\0';
    if (verb) verb[0] = '\0';
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return;

    const char *p = uri + plen;
    if (*p == '/') p++;           /* the separator after the prefix, not part
                                   * of the name -- without this every name
                                   * parsed as empty */
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(name, p, n);
    name[n] = '\0';
    if (slash && verb) {
        strncpy(verb, slash + 1, vcap - 1);
        verb[vcap - 1] = '\0';
    }
}

static esp_err_t layouts_get(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char name[CFG_LAYOUT_NAME_MAX], verb[16];
    path_tail(req->uri, "/layouts", name, sizeof(name), verb, sizeof(verb));

    httpd_resp_set_type(req, "application/json");

    if (name[0] == '\0') {
        /* The index: which layouts exist and which is on screen. */
        char names[CFG_MAX_LAYOUTS][CFG_LAYOUT_NAME_MAX];
        int n = config_layout_list(names, CFG_MAX_LAYOUTS);
        char out[512];
        size_t w = (size_t)snprintf(out, sizeof(out), "{\"active\":\"%s\",\"layouts\":[",
                                    config_active_layout());
        for (int i = 0; i < n && w < sizeof(out) - 32; i++) {
            w += (size_t)snprintf(out + w, sizeof(out) - w, "%s\"%s\"",
                                  i ? "," : "", names[i]);
        }
        snprintf(out + w, sizeof(out) - w, "]}\n");
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

    /* One layout's document, streamed so a large one needs no buffer. */
    char path[96];
    snprintf(path, sizeof(path), "%s/layouts/%s.json", STORAGE_CFG_PATH, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no such layout\"}\n");
        return ESP_OK;
    }
    char buf[512];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)got) != ESP_OK) {
            fclose(f); return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t layouts_post(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    char name[CFG_LAYOUT_NAME_MAX], verb[16];
    path_tail(req->uri, "/layouts", name, sizeof(name), verb, sizeof(verb));
    if (name[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"POST /layouts/<name>\"}\n");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    /* POST /layouts/<name>/activate   -- switch to a stored layout
     * POST /layouts/<name>/save       -- store what is on screen under <name>
     * POST /layouts/<name>            -- replace <name> with the posted body */
    if (strcmp(verb, "activate") == 0) {
        esp_err_t rc = config_layout_load(name);
        if (rc != ESP_OK) {
            httpd_resp_set_status(req, "404 Not Found");
            httpd_resp_sendstr(req, "{\"error\":\"no such layout\"}\n");
            return ESP_OK;
        }
        if (!persist(req)) return ESP_OK;
        char out[128];
        snprintf(out, sizeof(out), "{\"ok\":true,\"active\":\"%s\",\"panels\":%u}\n",
                 name, (unsigned)config_get()->n_panels);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

    if (strcmp(verb, "save") == 0) {
        if (config_layout_save(name) != ESP_OK) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req,
                "{\"error\":\"name must be letters, digits, - or _\"}\n");
            return ESP_OK;
        }
        if (!persist(req)) return ESP_OK;
        char out[128];
        snprintf(out, sizeof(out), "{\"ok\":true,\"saved\":\"%s\"}\n", name);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

    /* Body is a layout document: apply it live, then store it. */
    if (req->content_len <= 0 || req->content_len > WEBCFG_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "{\"error\":\"body missing or over 64KB\"}\n");
        return ESP_OK;
    }
    char *body = heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}\n");
        return ESP_OK;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, (size_t)(req->content_len - got));
        if (r <= 0) { heap_caps_free(body); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    char err[CFG_ERR_MAX] = "";
    esp_err_t rc = config_layout_apply_json(body, (size_t)got, err, sizeof(err));
    heap_caps_free(body);
    if (rc != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        /* Escaped, like every other reason: these quote selectors and titles,
         * which are full of the one character that would break the reply. */
        char eerr[CFG_ERR_MAX * 2];
        json_escape(err[0] ? err : "could not apply the layout",
                    eerr, sizeof(eerr));
        char out[CFG_ERR_MAX * 2 + 32];
        snprintf(out, sizeof(out), "{\"error\":\"%s\"}\n", eerr);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }
    config_layout_save(name);
    if (!persist(req)) return ESP_OK;

    char out[144];
    snprintf(out, sizeof(out), "{\"ok\":true,\"active\":\"%s\",\"panels\":%u}\n",
             name, (unsigned)config_get()->n_panels);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t layouts_delete(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);
    char name[CFG_LAYOUT_NAME_MAX], verb[16];
    path_tail(req->uri, "/layouts", name, sizeof(name), verb, sizeof(verb));

    httpd_resp_set_type(req, "application/json");
    if (config_layout_delete(name) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"no such layout\"}\n");
        return ESP_OK;
    }
    /* Deleting the active layout clears the name in the live config, so this
     * has to reach flash too. */
    if (!persist(req)) return ESP_OK;
    httpd_resp_sendstr(req, "{\"ok\":true}\n");
    return ESP_OK;
}

/* Unauthenticated on purpose: it carries nothing sensitive, and being able to
 * confirm the device is alive and find its version without a token is what
 * you want when something is wrong. */
static esp_err_t get_status(httpd_req_t *req)
{
    char ip[16] = ""; int8_t rssi = 0;
    wifi_mgr_info(ip, sizeof(ip), &rssi);
    const config_t *c = config_get();

    char out[320];
    snprintf(out, sizeof(out),
             "{\"app\":\"prometheus-panel\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime_s\":%llu,\"panels\":%u,\"endpoints\":%u,"
             "\"schema\":%u,\"free_internal\":%u,\"free_psram\":%u}\n",
             ip, (int)rssi,
             (unsigned long long)(esp_timer_get_time() / 1000000),
             (unsigned)c->n_panels, (unsigned)c->n_endpoints,
             (unsigned)CFG_SCHEMA_VERSION,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* --------------------------------------------------- what is out there */

/*
 * Scrape the configured endpoint and report the metric families it exposes.
 *
 * This is the half of interrogability that the schema cannot provide: an
 * agent can learn the config FORMAT from /schema, but it cannot write a
 * useful panel without knowing which metrics exist, what type they are, and
 * how many series each has.
 */
#define SEEN_MAX_NAMES 384
#define SEEN_NAME_MAX  112

typedef struct {
    char        name[SEEN_NAME_MAX];
    prom_type_t type;
    uint16_t    series;
    double      value;
    bool        has_value;
    char        sel[CFG_SEL_MAX];
    /* Last selector seen in this family. Samples of one series are contiguous
     * in the exposition, so comparing against the previous one counts LABEL
     * SETS rather than samples -- without it a 20-bucket histogram with two
     * label sets reports 42 series, which would tell an agent to reach for a
     * multi-series tile over a distribution that has two members. */
    char        last[CFG_SEL_MAX];
} seen_t;

typedef struct { seen_t *v; int n; } seen_ctx_t;

static void *psram_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void  psram_free(void *p)   { heap_caps_free(p); }

static bool seen_sample(void *ctx, const prom_sample_t *s)
{
    seen_ctx_t *c = ctx;

    /* Families are contiguous in the exposition, so the entry being added is
     * almost always the last one. */
    seen_t *e = NULL;
    if (c->n > 0 && strlen(c->v[c->n - 1].name) == s->base_len &&
        memcmp(c->v[c->n - 1].name, s->base_name, s->base_len) == 0) {
        e = &c->v[c->n - 1];
    } else {
        for (int i = 0; i < c->n && !e; i++) {
            if (strlen(c->v[i].name) == s->base_len &&
                memcmp(c->v[i].name, s->base_name, s->base_len) == 0) e = &c->v[i];
        }
    }
    if (e == NULL) {
        if (c->n >= SEEN_MAX_NAMES) return true;
        e = &c->v[c->n++];
        memset(e, 0, sizeof(*e));
        size_t n = s->base_len < SEEN_NAME_MAX ? s->base_len : SEEN_NAME_MAX - 1;
        memcpy(e->name, s->base_name, n);
    }

    if (s->type != PROM_TYPE_UNTYPED) e->type = s->type;

    char sel[CFG_SEL_MAX];
    if (prom_render(sel, sizeof(sel), s->base_name, s->base_len,
                    s->labels, s->n_labels) == 0) {
        return true;
    }
    if (strcmp(sel, e->last) != 0) {
        strncpy(e->last, sel, sizeof(e->last) - 1);
        if (e->series < 0xFFFF) e->series++;
    }

    if (!e->has_value && prom_is_num(s->value)) {
        e->value = s->value.num;
        e->has_value = true;
        strncpy(e->sel, sel, sizeof(e->sel) - 1);
    }
    return true;
}

static bool seen_chunk(void *ctx, const char *d, size_t n)
{
    return prom_text_feed((prom_text_parser_t *)ctx, d, n);
}

static esp_err_t get_metrics_seen(httpd_req_t *req)
{
    if (!authorised(req)) return deny(req);

    const config_t *c = config_get();
    if (c->n_endpoints == 0 || c->endpoints[0].url[0] == '\0') {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"no endpoint configured\"}\n");
        return ESP_OK;
    }

    seen_ctx_t ctx = { .v = heap_caps_malloc(sizeof(seen_t) * SEEN_MAX_NAMES,
                                             MALLOC_CAP_SPIRAM), .n = 0 };
    if (ctx.v == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}\n");
        return ESP_OK;
    }

    const prom_text_sink_t sink = { NULL, NULL, seen_sample };
    prom_text_parser_t *p = prom_text_new(&sink, &ctx, psram_alloc, psram_free);
    http_result_t res = {0};
    prom_text_stats_t st = {0};
    if (p) {
        /* Slot -1: a one-shot connection, so interrogating never disturbs the
         * socket the live poller is using. */
        http_get_stream(-1, c->endpoints[0].url, NULL, seen_chunk, p, 12000, &res);
        prom_text_finish(p, &st);
        prom_text_free(p);
    }

    if (res.klass != HTTP_ERR_NONE) {
        heap_caps_free(ctx.v);
        httpd_resp_set_status(req, "502 Bad Gateway");
        char eurl[CFG_URL_MAX * 2];
        json_escape(c->endpoints[0].url, eurl, sizeof(eurl));
        char out[CFG_URL_MAX * 2 + 80];
        snprintf(out, sizeof(out), "{\"error\":\"%s\",\"url\":\"%s\"}\n",
                 http_err_text(res.klass), eurl);
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    char esc_url[CFG_URL_MAX * 2];
    json_escape(c->endpoints[0].url, esc_url, sizeof(esc_url));

    char head[CFG_URL_MAX * 2 + 128];
    snprintf(head, sizeof(head),
             "{\"url\":\"%s\",\"samples\":%u,\"bytes\":%u,\"families\":%d,"
             "\"metrics\":[\n",
             esc_url, (unsigned)st.samples, (unsigned)res.bytes, ctx.n);
    httpd_resp_sendstr_chunk(req, head);

    /* Streamed a family at a time: 384 of them with selectors would be a
     * ~100KB buffer otherwise. */
    for (int i = 0; i < ctx.n; i++) {
        const seen_t *e = &ctx.v[i];
        char esc_sel[CFG_SEL_MAX * 2];
        json_escape(e->sel, esc_sel, sizeof(esc_sel));

        char row[CFG_SEL_MAX * 2 + 220];
        snprintf(row, sizeof(row),
                 "%s{\"name\":\"%s\",\"type\":\"%s\",\"series\":%u,"
                 "\"sample_sel\":\"%s\",\"sample_value\":%.10g}",
                 i ? ",\n" : "", e->name, prom_type_name(e->type),
                 (unsigned)e->series, esc_sel, e->has_value ? e->value : 0.0);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "\n]}\n");
    httpd_resp_send_chunk(req, NULL, 0);

    heap_caps_free(ctx.v);
    return ESP_OK;
}

/* ------------------------------------------------- describing the API */

/*
 * An index and a schema, so something that has never seen this device can
 * work out what it can do without reading the firmware.
 *
 * Enum values come from the parser's own tables, so a value documented here
 * is a value that will be accepted -- documentation that drifts from the
 * parser is worse than none.
 */
static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
"{\n"
"  \"app\": \"prometheus-panel\",\n"
"  \"describe\": \"GET /schema for the config format\",\n"
"  \"auth\": \"all routes except / and /status need the X-Auth header; the token is on the device under the gear button\",\n"
"  \"routes\": [\n"
"    { \"method\": \"GET\",    \"path\": \"/status\",                  \"auth\": false, \"desc\": \"identity, uptime, free memory\" },\n"
"    { \"method\": \"GET\",    \"path\": \"/schema\",                  \"auth\": false, \"desc\": \"the configuration format and its legal values\" },\n"
"    { \"method\": \"GET\",    \"path\": \"/config\",                  \"auth\": true,  \"desc\": \"the whole running configuration\" },\n"
"    { \"method\": \"POST\",   \"path\": \"/config\",                  \"auth\": true,  \"desc\": \"replace it; validated whole or rejected\" },\n"
"    { \"method\": \"GET\",    \"path\": \"/metrics-seen\",            \"auth\": true,  \"desc\": \"what the polled endpoint currently exposes\" },\n"
"    { \"method\": \"GET\",    \"path\": \"/layouts\",                 \"auth\": true,  \"desc\": \"stored layouts and which is active\" },\n"
"    { \"method\": \"GET\",    \"path\": \"/layouts/{name}\",          \"auth\": true,  \"desc\": \"one layout's screens and panels\" },\n"
"    { \"method\": \"POST\",   \"path\": \"/layouts/{name}\",          \"auth\": true,  \"desc\": \"apply the posted layout and store it under {name}\" },\n"
"    { \"method\": \"POST\",   \"path\": \"/layouts/{name}/save\",     \"auth\": true,  \"desc\": \"store what is on screen as {name}\" },\n"
"    { \"method\": \"POST\",   \"path\": \"/layouts/{name}/activate\", \"auth\": true,  \"desc\": \"put {name} on screen\" },\n"
"    { \"method\": \"DELETE\", \"path\": \"/layouts/{name}\",          \"auth\": true,  \"desc\": \"remove a stored layout\" }\n"
"  ]\n"
"}\n");
    return ESP_OK;
}

static esp_err_t get_schema(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char ek[160], ef[200], er[100], ea[80], eo[80], es[120], ep[80];
    config_enum_values("kind",   ek, sizeof(ek));
    config_enum_values("fmt",    ef, sizeof(ef));
    config_enum_values("reduce", er, sizeof(er));
    config_enum_values("agg",    ea, sizeof(ea));
    config_enum_values("op",     eo, sizeof(eo));
    config_enum_values("scale",  es, sizeof(es));
    config_enum_values("ramp",   ep, sizeof(ep));

    char buf[1600];
    snprintf(buf, sizeof(buf),
"{\n"
"  \"schema\": %u,\n"
"  \"grid\": { \"cols\": %d, \"rows\": %d, \"max_panels\": %d, \"max_terms\": %d,\n"
"             \"max_screens\": %d, \"note\": \"max_panels is the total across every screen\" },\n"
"  \"model\": \"A panel draws one number. It has TERMS; each term selects a set of series and reduces that set to a scalar, and an op combines the terms.\",\n"
"  \"enums\": {\n"
"    \"kind\":   [%s],\n"
"    \"scale\":  [%s],\n"
"    \"ramp\":   [%s],\n"
"    \"fmt\":    [%s],\n"
"    \"reduce\": [%s],\n"
"    \"agg\":    [%s],\n"
"    \"op\":     [%s]\n"
"  },\n",
        (unsigned)CFG_SCHEMA_VERSION, GRID_COLS, GRID_ROWS,
        CFG_MAX_PANELS, CFG_MAX_TERMS, CFG_MAX_SCREENS,
        ek, es, ep, ef, er, ea, eo);
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_sendstr_chunk(req,
"  \"fields\": {\n"
"    \"panel.sel\":      \"mirror of terms[0].sel; written by the device, ignored on input\",\n"
"    \"panel.col/row\":  \"top-left cell; col+w and row+h must stay inside the grid\",\n"
"    \"panel.screen\":   \"which page the tile is on, swiped between; must be < the length of screens[]\",\n"
"    \"panel.w/h\":      \"span in cells; a widget below its minimum is refused (chart and histogram need 2x2, multi needs 2x1)\",\n"
"    \"panel.op\":       \"share = a/(a+b), ratio = a/b, diff = a-b, sum = a+b+...; none means a single term\",\n"
"    \"panel.vmin/vmax\":\"gauge and bar range, in the DISPLAYED domain: a percent panel reads 0..100 whatever its source ratio is. null means auto -- 100 for a percent, and otherwise the largest value seen so far, which is the only full scale available for something like a concurrency limit the server does not export. The peak resets when the device restarts or the panel's terms change.\",\n"
"    \"panel.warn/crit\":\"threshold colouring; null means none\",\n"
"    \"panel.multi\":    \"show every matching series as ranked rows instead of one number\",\n"
"    \"panel.prefix/suffix\": \"free text wrapped around the value -- \\\"$\\\" or \\\" EUR\\\" -- for labels the SI ladder cannot express. Distinct from unit, which is part of the magnitude and moves with the prefix. The large digit faces carry only digits, punctuation and the currency marks $ c/ L- Y= E=; anything else drops the value to a smaller text face rather than vanishing.\",\n"
"    \"panel.ramp\":     \"how a gauge or bar colours its indicator: none takes the threshold colour, which on a panel with no warn/crit set is one colour forever; heat runs ok to crit as the value rises, cool reverses it, series steps through the categorical palette. Built from the active theme's own colours, so it follows the palette rather than fighting it.\",\n"
"    \"panel.group\":    \"thousands separators in the value: 17,321 rather than 17321. Only affects formats that render bare digits, and only bites once a number is four digits long -- which in practice means alongside a scale pin, since an auto prefix keeps it to three.\",\n"
"    \"panel.scale\":    \"pins the SI/IEC prefix so the unit stops moving as the value does; auto keeps three significant digits instead. The names are the SI ladder and map by position on a byte panel: k is KiB, M is MiB. Ignored by formats with no ladder (percent, duration, bool).\",\n"
"    \"term.sel\":       \"metric{label=\\\"value\\\"}; a label value may contain * as a glob\",\n"
"    \"term.reduce\":    \"collapses the matched set to a scalar; this is the sum by() of this format\",\n"
"    \"term.agg\":       \"last takes the value; rate differences it over window_s\",\n"
"    \"term.window_s\":  \"seconds a rate or quantile covers; 0 means one poll, or all-time for a quantile\",\n"
"    \"term.q\":         \"histogram quantile, 0-1; null or 0 means this is not a quantile panel\"\n"
"  },\n"
"  \"notes\": [\n"
"    \"Each term is reduced first and rated second: rate(sum(x)).\",\n"
"    \"An all-time quantile on a long-lived process stops moving; set window_s.\",\n"
"    \"A counter whose exporter updates on a log interval steps rather than flows; a window smooths it.\",\n"
"    \"A push is applied whole or rejected, and the error names the offending panel.\"\n"
"    ,\"Cells are per screen, so two panels may share col/row if their screen differs.\"\n"
"    ,\"Every screen's panels are polled whether or not it is the one on display, so a page is warm when you swipe to it.\"\n"
"  ],\n"
"  \"example\": { \"title\": \"prefix hit rate\", \"kind\": \"gauge\", \"fmt\": \"percent\",\n"
"    \"op\": \"share\", \"col\": 0, \"row\": 0, \"w\": 1, \"h\": 1,\n"
"    \"terms\": [\n"
"      { \"sel\": \"x_total{mode=\\\"hit\\\"}\",  \"reduce\": \"sum\", \"agg\": \"rate\", \"window_s\": 60 },\n"
"      { \"sel\": \"x_total{mode=\\\"miss\\\"}\", \"reduce\": \"sum\", \"agg\": \"rate\", \"window_s\": 60 }\n"
"    ] }\n"
"}\n");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ start */

esp_err_t webcfg_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 10;
    /* /layouts/<name>/<verb> needs prefix matching. */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.stack_size       = 8192;    /* a pushed config is written to flash on
                                     * this task rather than handed to a
                                     * worker, so the acknowledgement can mean
                                     * it landed.
                                     * JSON parse happens on the caller's heap,
                                     * not its stack, so this is ample */
    cfg.core_id          = 0;       /* keep HTTP off the rendering core */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t k_get_cfg = {
        .uri = "/config", .method = HTTP_GET, .handler = get_config };
    static const httpd_uri_t k_post_cfg = {
        .uri = "/config", .method = HTTP_POST, .handler = post_config };
    static const httpd_uri_t k_status = {
        .uri = "/status", .method = HTTP_GET, .handler = get_status };
    static const httpd_uri_t k_index = {
        .uri = "/", .method = HTTP_GET, .handler = get_index };
    static const httpd_uri_t k_seen = {
        .uri = "/metrics-seen", .method = HTTP_GET, .handler = get_metrics_seen };
    static const httpd_uri_t k_schema = {
        .uri = "/schema", .method = HTTP_GET, .handler = get_schema };
    static const httpd_uri_t k_lay_get = {
        .uri = "/layouts*", .method = HTTP_GET, .handler = layouts_get };
    static const httpd_uri_t k_lay_post = {
        .uri = "/layouts*", .method = HTTP_POST, .handler = layouts_post };
    static const httpd_uri_t k_lay_del = {
        .uri = "/layouts*", .method = HTTP_DELETE, .handler = layouts_delete };

    /* Specific routes first: with wildcard matching, /config would otherwise
     * be shadowed by a broader pattern registered before it. */
    httpd_register_uri_handler(s_server, &k_get_cfg);
    httpd_register_uri_handler(s_server, &k_post_cfg);
    httpd_register_uri_handler(s_server, &k_status);
    httpd_register_uri_handler(s_server, &k_schema);
    httpd_register_uri_handler(s_server, &k_seen);
    httpd_register_uri_handler(s_server, &k_lay_get);
    httpd_register_uri_handler(s_server, &k_lay_post);
    httpd_register_uri_handler(s_server, &k_lay_del);
    httpd_register_uri_handler(s_server, &k_index);

    char ip[16] = "";
    wifi_mgr_info(ip, sizeof(ip), NULL);
    ESP_LOGI(TAG, "config endpoint on http://%s/config", ip);
    return ESP_OK;
}

void webcfg_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}

bool webcfg_running(void) { return s_server != NULL; }
