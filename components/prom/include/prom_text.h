/*
 * Streaming Prometheus text exposition parser.
 *
 * Push-mode: the caller feeds HTTP body chunks as they arrive and samples are
 * delivered through a sink. Peak memory is INDEPENDENT of body size -- a 5MB
 * cAdvisor scrape and a 4KB up{} scrape cost the same RAM, because the only
 * buffer that scales with anything is the single-line reassembly buffer.
 *
 * Allocation is injected so the host test harness can pass malloc/free while
 * the device passes a MALLOC_CAP_SPIRAM wrapper -- on this board
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 would otherwise silently put the
 * 4KB line buffer in the scarce internal heap.
 */
#ifndef PROM_TEXT_H
#define PROM_TEXT_H

#include "prom_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct prom_text_parser prom_text_parser_t;

typedef struct {
    /* All pointers are valid only for the duration of the call. Copy to keep. */
    void (*on_help)(void *ctx, const char *name, size_t name_len,
                    const char *help, size_t help_len);
    void (*on_type)(void *ctx, const char *name, size_t name_len, prom_type_t t);
    /* Return false to abort the whole parse (e.g. the caller has what it
     * needs). prom_text_feed then returns false and the HTTP read should be
     * torn down -- see the note about aborted reads in http_util. */
    bool (*on_sample)(void *ctx, const prom_sample_t *s);
} prom_text_sink_t;

prom_text_parser_t *prom_text_new(const prom_text_sink_t *sink, void *ctx,
                                  void *(*alloc)(size_t), void (*dealloc)(void *));

/* Feed one chunk. Returns false only if the sink aborted. Partial lines are
 * carried across calls, so chunk boundaries are invisible to the sink. */
bool prom_text_feed(prom_text_parser_t *p, const char *data, size_t len);

/* Flush a trailing line with no newline (real exporters behind some proxies
 * do this) and copy out the stats. */
void prom_text_finish(prom_text_parser_t *p, prom_text_stats_t *out);

/* Reuse the parser for another scrape without reallocating the line buffer. */
void prom_text_reset(prom_text_parser_t *p);

void prom_text_free(prom_text_parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* PROM_TEXT_H */
