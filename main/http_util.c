#include "http_util.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "http";

typedef struct {
    http_chunk_cb cb;
    void         *ctx;
    uint64_t      bytes;
    bool          aborted;
    bool          gzip;
    bool          saw_first;
} sink_t;

static esp_http_client_handle_t s_slot[HTTP_KEEPALIVE_SLOTS];
static char s_slot_host[HTTP_KEEPALIVE_SLOTS][64];

const char *http_err_text(http_err_class_t k)
{
    switch (k) {
    case HTTP_ERR_DNS:       return "unreachable";
    case HTTP_ERR_TIMEOUT:   return "timeout";
    case HTTP_ERR_TLS:       return "TLS";
    case HTTP_ERR_AUTH:      return "auth";
    case HTTP_ERR_NOTFOUND:  return "not found";
    case HTTP_ERR_SERVER:    return "server error";
    case HTTP_ERR_TRANSPORT: return "connection failed";
    case HTTP_ERR_GZIP:      return "gzip not supported";
    case HTTP_ERR_ABORTED:   return "aborted";
    case HTTP_ERR_NONE:
    default:                 return "ok";
    }
}

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    sink_t *s = evt->user_data;
    if (s == NULL || s->aborted) return ESP_OK;

    const char *d = (const char *)evt->data;
    size_t n = (size_t)evt->data_len;

    /*
     * We send Accept-Encoding: identity, but some servers compress anyway on
     * a permissive or absent header. esp_http_client cannot inflate, so
     * without this check the parser gets 0x1f 0x8b binary and reports
     * "no metrics found" -- a mystifying error to debug from the panel.
     */
    if (!s->saw_first) {
        s->saw_first = true;
        if (n >= 2 && (unsigned char)d[0] == 0x1f && (unsigned char)d[1] == 0x8b) {
            s->gzip = true;
            s->aborted = true;
            return ESP_FAIL;
        }
    }

    s->bytes += n;
    if (!s->cb(s->ctx, d, n)) {
        s->aborted = true;
        return ESP_FAIL;      /* propagates out of esp_http_client_perform */
    }
    return ESP_OK;
}

static http_err_class_t classify(esp_err_t err, int status)
{
    if (status == 401 || status == 403) return HTTP_ERR_AUTH;
    if (status == 404)                  return HTTP_ERR_NOTFOUND;
    if (status >= 500)                  return HTTP_ERR_SERVER;

    switch (err) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: return HTTP_ERR_DNS;
    case ESP_ERR_HTTP_EAGAIN:                     return HTTP_ERR_TIMEOUT;
    default: break;
    }
    /* The esp_tls error space is contiguous; anything in it that is not a DNS
     * failure is a handshake or certificate problem. */
    if (err >= ESP_ERR_ESP_TLS_BASE && err <= ESP_ERR_ESP_TLS_BASE + 0xFF) {
        return HTTP_ERR_TLS;
    }
    if (err != ESP_OK) return HTTP_ERR_TRANSPORT;
    if (status < 200 || status >= 300) return HTTP_ERR_TRANSPORT;
    return HTTP_ERR_NONE;
}

static void host_of(const char *url, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < cap) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

void http_drop_slot(int slot)
{
    if (slot < 0 || slot >= HTTP_KEEPALIVE_SLOTS) return;
    if (s_slot[slot]) {
        esp_http_client_close(s_slot[slot]);
        esp_http_client_cleanup(s_slot[slot]);
        s_slot[slot] = NULL;
        s_slot_host[slot][0] = '\0';
    }
}

esp_err_t http_get_stream(int slot, const char *url, const char *auth_header,
                          http_chunk_cb cb, void *ctx, int timeout_ms,
                          http_result_t *out)
{
    http_result_t r = {0};
    if (url == NULL || cb == NULL) {
        r.err = ESP_ERR_INVALID_ARG;
        if (out) *out = r;
        return r.err;
    }

    int64_t t0 = esp_timer_get_time();
    bool use_slot = (slot >= 0 && slot < HTTP_KEEPALIVE_SLOTS);

    /* A cached connection is only reusable for the same host. */
    if (use_slot && s_slot[slot]) {
        char h[64];
        host_of(url, h, sizeof(h));
        if (strcmp(h, s_slot_host[slot]) != 0) http_drop_slot(slot);
    }

    bool reused = use_slot && s_slot[slot] != NULL;

retry:;
    sink_t sink = { .cb = cb, .ctx = ctx };
    esp_http_client_handle_t client = use_slot ? s_slot[slot] : NULL;

    if (client == NULL) {
        esp_http_client_config_t cfg = {
            .url                   = url,
            .event_handler         = on_event,
            .user_data             = &sink,
            .timeout_ms            = timeout_ms > 0 ? timeout_ms : 8000,
            .crt_bundle_attach     = esp_crt_bundle_attach,
            .keep_alive_enable     = use_slot,
            .disable_auto_redirect = false,
            /* The default of 10 invites a redirect loop that eats the whole
             * timeout budget before anything is reported. */
            .max_redirection_count = 3,
            .user_agent            = "prometheus-esp32/0.1",
            /*
             * These buffers are malloc'd internally, and
             * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 puts anything this
             * size in the scarce internal heap. The defaults (4096 each) would
             * hold 8KB of internal RAM per slot in use; 2048/1024 holds 3KB.
             */
            .buffer_size           = 2048,
            .buffer_size_tx        = 1024,
        };
        client = esp_http_client_init(&cfg);
        if (client == NULL) {
            r.err = ESP_ERR_NO_MEM;
            r.klass = HTTP_ERR_TRANSPORT;
            if (out) *out = r;
            return r.err;
        }
        if (use_slot) {
            s_slot[slot] = client;
            host_of(url, s_slot_host[slot], sizeof(s_slot_host[slot]));
        }
    } else {
        esp_http_client_set_url(client, url);
        esp_http_client_set_user_data(client, &sink);
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "text/plain;version=0.0.4");
    /* Explicit: esp_http_client cannot gunzip, and some servers compress on an
     * absent Accept-Encoding. */
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (auth_header) esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    r.err    = err;
    r.status = status;
    r.bytes  = sink.bytes;

    if (sink.gzip) {
        r.klass = HTTP_ERR_GZIP;
    } else if (sink.aborted) {
        r.klass = HTTP_ERR_ABORTED;
    } else {
        r.klass = classify(err, status);
    }

    bool ok = (r.klass == HTTP_ERR_NONE);

    if (use_slot) {
        /*
         * Three reasons to drop a cached connection, all learned the hard way
         * in the reference implementation this is modelled on:
         *
         * 1. Any failure at all -- a half-failed connection is not reusable.
         * 2. After a TLS-level error the session context may already be gone,
         *    and reusing the handle crashes inside mbedtls_ssl_write.
         * 3. After WE aborted the read, unread bytes are still queued on the
         *    socket. Reusing it silently corrupts the NEXT response, which is
         *    a genuinely horrible bug to track down.
         */
        if (!ok) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            s_slot[slot] = NULL;
            s_slot_host[slot][0] = '\0';
        }
    } else {
        esp_http_client_cleanup(client);
    }

    /*
     * Stale-socket retry. Servers close idle keep-alive connections between
     * our polls -- nginx's keepalive_timeout defaults to 75s and Go's
     * IdleTimeout often lands near 60s -- so a 60s poll interval sits exactly
     * on the edge and EVERY OTHER poll would fail. One immediate
     * fresh-connection attempt makes that invisible.
     *
     * Only for transport failures on a reused socket: a 404 or a 401 will say
     * the same thing the second time.
     */
    if (!ok && reused && !sink.aborted && !sink.gzip && sink.bytes == 0 &&
        (r.klass == HTTP_ERR_TRANSPORT || r.klass == HTTP_ERR_TIMEOUT)) {
        ESP_LOGD(TAG, "stale keep-alive socket, retrying fresh");
        reused = false;
        goto retry;
    }

    r.duration_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (!ok) {
        ESP_LOGW(TAG, "GET %s -> %s (esp_err=%s http=%d, %u bytes, %ums)",
                 url, http_err_text(r.klass), esp_err_to_name(err), status,
                 (unsigned)r.bytes, (unsigned)r.duration_ms);
    } else {
        ESP_LOGD(TAG, "GET %s -> %d, %u bytes in %ums",
                 url, status, (unsigned)r.bytes, (unsigned)r.duration_ms);
    }

    if (out) *out = r;
    return ok ? ESP_OK : (err != ESP_OK ? err : ESP_FAIL);
}
