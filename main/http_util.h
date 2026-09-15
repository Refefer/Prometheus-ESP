/*
 * Streaming HTTP GET with keep-alive slots.
 *
 * Streaming rather than buffer-the-whole-body is not an optimisation: a
 * node_exporter response is 150-500KB and a cAdvisor one reaches megabytes.
 * Buffering would need half a megabyte of PSRAM per poll and would spike
 * allocation on every cycle. The body is handed to the caller in chunks as it
 * arrives, so peak memory is independent of response size.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return false to abort the transfer (the sink has what it needs, or the body
 * is malformed). The connection is then closed and dropped, never reused. */
typedef bool (*http_chunk_cb)(void *ctx, const char *data, size_t len);

/* How a request failed, in terms that map to a distinct user action rather
 * than to an errno. "error 0x8006" helps nobody standing in front of a panel. */
typedef enum {
    HTTP_ERR_NONE = 0,
    HTTP_ERR_DNS,        /* hostname does not resolve */
    HTTP_ERR_TIMEOUT,    /* host down, or too slow */
    HTTP_ERR_TLS,        /* handshake failed: http vs https, or a cert */
    HTTP_ERR_AUTH,       /* 401 / 403 */
    HTTP_ERR_NOTFOUND,   /* 404: /metrics vs /api/v1/query */
    HTTP_ERR_SERVER,     /* 5xx: their problem, not ours */
    HTTP_ERR_TRANSPORT,  /* connection refused, reset, etc. */
    HTTP_ERR_GZIP,       /* body arrived compressed and we cannot inflate it */
    HTTP_ERR_ABORTED,    /* the sink stopped us */
} http_err_class_t;

typedef struct {
    esp_err_t        err;
    http_err_class_t klass;
    int              status;
    uint64_t         bytes;
    uint32_t         duration_ms;
} http_result_t;

#define HTTP_KEEPALIVE_SLOTS 3

/*
 * `slot` selects a cached connection (0..HTTP_KEEPALIVE_SLOTS-1), or -1 for a
 * one-shot connection. A slot is single-task: two tasks sharing one will
 * corrupt each other's transfer.
 *
 * `auth_header` is a complete Authorization value ("Bearer ..." /
 * "Basic ...") or NULL.
 */
esp_err_t http_get_stream(int slot, const char *url, const char *auth_header,
                          http_chunk_cb cb, void *ctx, int timeout_ms,
                          http_result_t *out);

/* Force a cached connection closed, e.g. when its endpoint is reconfigured. */
void http_drop_slot(int slot);

const char *http_err_text(http_err_class_t k);
