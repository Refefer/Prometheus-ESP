/*
 * Series identity: canonical form, 64-bit id, and the human/persisted
 * rendering that round-trips back to the same id.
 */
#ifndef PROM_IDENT_H
#define PROM_IDENT_H

#include "prom_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t prom_series_id_t;     /* 0 is reserved as "invalid" */

/* --------------------------------------------------- shared label scanning */

typedef enum {
    PROM_LBL_OK        =  0,
    PROM_LBL_MALFORMED = -1,
    PROM_LBL_TOO_MANY  = -2,
} prom_lbl_res_t;

/*
 * Scan a '{...}' label set in place, starting at *pos (which must index the
 * '{') and leaving *pos just past the '}'.
 *
 * Values are unescaped in place, so `s` must be writable and the returned
 * label pointers alias it. `le` and `quantile` are lifted out of the label
 * array into the out-params -- they select within a family instance rather
 * than identifying a series, which is what lets one histogram selection cover
 * all of its buckets plus _sum and _count.
 *
 * Shared by the exposition parser and the selector parser deliberately: two
 * implementations of the escape/quoting rules is exactly how a render->parse
 * round trip stops being exact.
 */
prom_lbl_res_t prom_scan_labels(char *s, size_t len, size_t *pos,
                                prom_label_t *out, uint8_t max, uint8_t *n_out,
                                prom_value_t *le_out, prom_value_t *quantile_out);

/* Decode \\ \" \n \t in place; returns the new length (never longer). */
uint16_t prom_unescape_inplace(char *s, size_t len);

/* Insertion sort by key, bytewise ascending. Required before canonicalising. */
void prom_sort_labels(prom_label_t *l, uint8_t n);

/* ---------------------------------------------------------------- identity */

/*
 * Canonical bytes:  name '\0' ( key '\0' value '\0' )*   with labels sorted.
 * Returns the number of bytes written, or 0 if it would not fit.
 */
size_t prom_canon(char *dst, size_t cap, const char *name, size_t name_len,
                  const prom_label_t *labels, size_t n);

/* FNV-1a 64 over the canonical bytes. Never returns 0 (folds to 1). */
prom_series_id_t prom_id_from_canon(const char *canon, size_t len);

/* Convenience: canonicalise into caller scratch, then hash. 0 on overflow. */
prom_series_id_t prom_series_id(const char *name, size_t name_len,
                                const prom_label_t *labels, size_t n,
                                char *scratch, size_t scratch_cap);

/*
 * Display and persistence form:  name{key="value",key2="value2"}
 * Values are re-escaped. This is what goes in config.json as "sel", and it is
 * the identity of record -- the hash is only an index into it, so a future
 * change to the hash function costs nothing.
 * Returns bytes written excluding the NUL, or 0 on overflow.
 */
size_t prom_render(char *dst, size_t cap, const char *name, size_t name_len,
                   const prom_label_t *labels, size_t n);

/*
 * Inverse of prom_render. Copies `sel` into `scratch` and parses in place, so
 * the returned pointers alias scratch and are valid as long as it is.
 */
bool prom_parse_selector(const char *sel, char *scratch, size_t scratch_cap,
                         const char **name_out, uint16_t *name_len_out,
                         prom_label_t *labels, uint8_t max, uint8_t *n_out);

#ifdef __cplusplus
}
#endif
#endif /* PROM_IDENT_H */
