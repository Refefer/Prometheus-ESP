#include "webcfg.h"

#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "poller.h"
#include "secrets.h"
#include "storage.h"
#include "wifi_mgr.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "webcfg";

static httpd_handle_t s_server;
static bool           s_ever_used;

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
    char err[96] = "";
    esp_err_t rc = config_apply_json(body, (size_t)got, err, sizeof(err));
    heap_caps_free(body);

    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "rejected a pushed config: %s", err);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char out[160];
        snprintf(out, sizeof(out), "{\"error\":%s%s%s}\n",
                 "\"", err[0] ? err : "could not parse the configuration", "\"");
        httpd_resp_sendstr(req, out);
        return ESP_OK;
    }

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

/* ------------------------------------------------------------------ start */

esp_err_t webcfg_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 6;
    cfg.stack_size       = 6144;    /* JSON parse happens on the caller's heap,
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

    httpd_register_uri_handler(s_server, &k_get_cfg);
    httpd_register_uri_handler(s_server, &k_post_cfg);
    httpd_register_uri_handler(s_server, &k_status);

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
