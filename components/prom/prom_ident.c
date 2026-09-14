#include "prom_ident.h"

#include <string.h>

/* ------------------------------------------------------------------ shared */

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static bool is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}
static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '.';
}

uint16_t prom_unescape_inplace(char *s, size_t len)
{
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        char c = s[r];
        if (c == '\\' && r + 1 < len) {
            char n = s[++r];
            switch (n) {
            case 'n':  s[w++] = '\n'; break;
            case 't':  s[w++] = '\t'; break;
            case '\\': s[w++] = '\\'; break;
            case '"':  s[w++] = '"';  break;
            /* The spec calls any other escape an error. Emitting the literal
             * instead means one exporter bug costs one odd label value rather
             * than the whole series vanishing from the panel. */
            default:   s[w++] = n;    break;
            }
        } else {
            s[w++] = c;
        }
    }
    return (uint16_t)w;
}

void prom_sort_labels(prom_label_t *l, uint8_t n)
{
    for (uint8_t i = 1; i < n; i++) {
        prom_label_t key = l[i];
        int j = (int)i - 1;
        while (j >= 0) {
            size_t m = l[j].key_len < key.key_len ? l[j].key_len : key.key_len;
            int c = memcmp(l[j].key, key.key, m);
            if (c == 0) c = (int)l[j].key_len - (int)key.key_len;
            if (c <= 0) break;
            l[j + 1] = l[j];
            j--;
        }
        l[j + 1] = key;
    }
}

prom_lbl_res_t prom_scan_labels(char *s, size_t len, size_t *pos,
                                prom_label_t *out, uint8_t max, uint8_t *n_out,
                                prom_value_t *le_out, prom_value_t *quantile_out)
{
    size_t i = *pos;
    bool too_many = false;

    if (i >= len || s[i] != '{') return PROM_LBL_MALFORMED;
    i++;

    for (;;) {
        /* Leniently skip separators: whitespace around '=' and ',' is legal,
         * and OpenMetrics permits a trailing comma before '}'. */
        while (i < len && (is_ws(s[i]) || s[i] == ',')) i++;
        if (i >= len) return PROM_LBL_MALFORMED;
        if (s[i] == '}') { i++; break; }

        if (!is_name_start(s[i])) return PROM_LBL_MALFORMED;
        size_t k0 = i;
        while (i < len && is_name_char(s[i])) i++;
        size_t k_len = i - k0;

        while (i < len && is_ws(s[i])) i++;
        if (i >= len || s[i] != '=') return PROM_LBL_MALFORMED;
        i++;
        while (i < len && is_ws(s[i])) i++;
        if (i >= len || s[i] != '"') return PROM_LBL_MALFORMED;
        i++;

        size_t v0 = i;
        bool closed = false;
        while (i < len) {
            if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
            if (s[i] == '"') { closed = true; break; }
            i++;
        }
        if (!closed) return PROM_LBL_MALFORMED;
        size_t v_raw = i - v0;
        i++;                                   /* past the closing quote */

        uint16_t v_len = prom_unescape_inplace(s + v0, v_raw);
        if (k_len > PROM_LABEL_KEY_MAX || v_len > PROM_LABEL_VAL_MAX) continue;

        if (k_len == 2 && memcmp(s + k0, "le", 2) == 0) {
            if (le_out) *le_out = prom_parse_value(s + v0, v_len);
            continue;
        }
        if (k_len == 8 && memcmp(s + k0, "quantile", 8) == 0) {
            if (quantile_out) *quantile_out = prom_parse_value(s + v0, v_len);
            continue;
        }

        if (*n_out >= max) { too_many = true; continue; }
        prom_label_t *L = &out[(*n_out)++];
        L->key = s + k0; L->key_len = (uint16_t)k_len;
        L->val = s + v0; L->val_len = v_len;
    }

    *pos = i;
    return too_many ? PROM_LBL_TOO_MANY : PROM_LBL_OK;
}

/* ---------------------------------------------------------------- identity */

size_t prom_canon(char *dst, size_t cap, const char *name, size_t name_len,
                  const prom_label_t *labels, size_t n)
{
    size_t w = 0;
    if (name_len + 1 > cap) return 0;
    memcpy(dst, name, name_len); w = name_len;
    dst[w++] = '\0';

    for (size_t i = 0; i < n; i++) {
        size_t need = (size_t)labels[i].key_len + 1u +
                      (size_t)labels[i].val_len + 1u;
        if (w + need > cap) return 0;
        memcpy(dst + w, labels[i].key, labels[i].key_len); w += labels[i].key_len;
        dst[w++] = '\0';
        memcpy(dst + w, labels[i].val, labels[i].val_len); w += labels[i].val_len;
        dst[w++] = '\0';
    }
    return w;
}

prom_series_id_t prom_id_from_canon(const char *canon, size_t len)
{
    /* FNV-1a 64. ~120 cycles for a 120-byte key, so 8000 samples is ~4ms at
     * 240MHz. Birthday collision probability at 10k live series is ~3e-12 --
     * and collisions are DETECTED anyway by comparing the interned render
     * string, so the hash is an index and the string is the truth. */
    uint64_t h = 14695981039346656037ULL;   /* FNV-1a 64 offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)canon[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;     /* 0 is reserved for "invalid" */
}

prom_series_id_t prom_series_id(const char *name, size_t name_len,
                                const prom_label_t *labels, size_t n,
                                char *scratch, size_t scratch_cap)
{
    size_t len = prom_canon(scratch, scratch_cap, name, name_len, labels, n);
    if (len == 0) return 0;
    return prom_id_from_canon(scratch, len);
}

size_t prom_render(char *dst, size_t cap, const char *name, size_t name_len,
                   const prom_label_t *labels, size_t n)
{
    size_t w = 0;
    if (name_len + 1 > cap) return 0;
    memcpy(dst, name, name_len); w = name_len;

    if (n > 0) {
        if (w + 1 >= cap) return 0;
        dst[w++] = '{';
        for (size_t i = 0; i < n; i++) {
            if (i > 0) { if (w + 1 >= cap) return 0; dst[w++] = ','; }
            if (w + labels[i].key_len + 2 >= cap) return 0;
            memcpy(dst + w, labels[i].key, labels[i].key_len); w += labels[i].key_len;
            dst[w++] = '=';
            dst[w++] = '"';
            for (uint16_t j = 0; j < labels[i].val_len; j++) {
                char c = labels[i].val[j];
                const char *esc = NULL;
                switch (c) {
                case '\\': esc = "\\\\"; break;
                case '"':  esc = "\\\""; break;
                case '\n': esc = "\\n";  break;
                default: break;
                }
                if (esc) {
                    if (w + 2 >= cap) return 0;
                    dst[w++] = esc[0]; dst[w++] = esc[1];
                } else {
                    if (w + 1 >= cap) return 0;
                    dst[w++] = c;
                }
            }
            if (w + 1 >= cap) return 0;
            dst[w++] = '"';
        }
        if (w + 1 >= cap) return 0;
        dst[w++] = '}';
    }
    if (w >= cap) return 0;
    dst[w] = '\0';
    return w;
}

bool prom_parse_selector(const char *sel, char *scratch, size_t scratch_cap,
                         const char **name_out, uint16_t *name_len_out,
                         prom_label_t *labels, uint8_t max, uint8_t *n_out)
{
    if (sel == NULL || scratch == NULL) return false;
    size_t len = strlen(sel);
    if (len + 1 > scratch_cap) return false;
    memcpy(scratch, sel, len + 1);

    size_t i = 0;
    while (i < len && is_ws(scratch[i])) i++;
    if (i >= len || !is_name_start(scratch[i])) return false;
    size_t n0 = i;
    while (i < len && is_name_char(scratch[i])) i++;

    *name_out     = scratch + n0;
    *name_len_out = (uint16_t)(i - n0);
    *n_out        = 0;

    while (i < len && is_ws(scratch[i])) i++;
    if (i < len && scratch[i] == '{') {
        prom_value_t le = prom_absent(), q = prom_absent();
        if (prom_scan_labels(scratch, len, &i, labels, max, n_out, &le, &q)
            == PROM_LBL_MALFORMED) {
            return false;
        }
    }

    prom_sort_labels(labels, *n_out);
    return true;
}
