#ifndef HYPERPIPE_HP_JSON_SER_H
#define HYPERPIPE_HP_JSON_SER_H

#include "hp_common.h"
#include "hp_keymap.h"

typedef struct hp_json_ser_s {
    char *out_buf;
    size_t capacity;
    size_t pos;

    /* Vectorized scanner function pointer */
    size_t (*scan_escapes)(const uint8_t *p, size_t len);

    /* Fused JSON string emitter: writes the escaped body of str (without the
     * surrounding quotes) into dst and returns the number of bytes written.
     * dst must have room for at least 6*len + 1 bytes (worst case: every byte
     * expands to \u00XX). The AVX-512/AVX2 implementations scan and copy in
     * one pass with vector stores; the scalar implementation is the
     * correctness reference. */
    size_t (*emit_string)(char *dst, const char *str, size_t len);

    /* Memoized per-column key fragments ("name":). Built once per header by
     * hp_json_ser_bind_keys(); hp_json_ser_row emits them with a single
     * memcpy per key instead of strlen + double-store per row. */
    char *frag_buf;
    size_t frag_cap;
    size_t frag_offsets[HP_MAX_COLUMNS];
    size_t frag_lens[HP_MAX_COLUMNS];
    size_t frag_count;
} hp_json_ser_t;

hp_error_t hp_json_ser_init(hp_json_ser_t *ser, size_t initial_capacity, hp_simd_mode_t simd);
void hp_json_ser_free(hp_json_ser_t *ser);
void hp_json_ser_reset(hp_json_ser_t *ser);

/* Appends an escaped JSON string value enclosed in quotes: "..." */
hp_error_t hp_json_ser_write_string(hp_json_ser_t *ser, const char *str, size_t len);

/* Memoizes escaped key fragments for the given header. Call whenever the
 * output header is created or grows; safe to call redundantly. On failure
 * the serializer silently reverts to the general strlen path. */
hp_error_t hp_json_ser_bind_keys(hp_json_ser_t *ser, const hp_header_t *header);

/* Serializes a complete row with given header column names to NDJSON line */
hp_error_t hp_json_ser_row(hp_json_ser_t *ser,
                           const hp_header_t *header,
                           const hp_row_t *row,
                           const char **out_line,
                           size_t *out_line_len);

/* Zero-copy variant: serializes the row into the caller-provided buffer.
 * Returns HP_ERR_BUFFER_OVERFLOW (writing nothing) when the worst-case
 * expanded size exceeds cap, so callers can flush/retry cheaply. This is the
 * hot path used by the transcoder to serialize directly into its page
 * buffer, eliminating the intermediate row buffer copy. */
hp_error_t hp_json_ser_row_direct(const hp_json_ser_t *ser,
                                  const hp_header_t *header,
                                  const hp_row_t *row,
                                  char *dst,
                                  size_t cap,
                                  size_t *out_len);

/* Reverse conversion: parses an NDJSON line into an hp_row_t and extracts header */
hp_error_t hp_json_parse_line(const char *line,
                              size_t len,
                              hp_header_t *header,
                              hp_row_t *out_row,
                              char *value_arena,
                              size_t arena_size);

/* Extended parse: same semantics with an optional keymap for O(1) key
 * resolution. The keymap is kept in sync when the header grows mid-line.
 * Fields of a returned row may point into `line` (zero-copy); the caller
 * must not advance the underlying buffer until the row is consumed. */
hp_error_t hp_json_parse_line_ex(const char *line,
                                 size_t len,
                                 hp_header_t *header,
                                 hp_keymap_t *keymap,
                                 hp_row_t *out_row,
                                 char *value_arena,
                                 size_t arena_size);

/* Strict JSON number grammar check (RFC 8259): integers, fractions and
 * exponents, nothing else. */
bool hp_json_is_number(const char *s, size_t len);

/* Numeric span -> double via a bounded stack copy and strtod. Accepts the
 * strtod grammar (superset of JSON numbers); returns false on empty or
 * over-long spans and on trailing garbage. */
bool hp_json_number_as_double(const char *s, size_t len, double *out);

/* Vectorized escape scanners: return the index of the first byte that needs
 * JSON escaping, or len when the span is clean. */
size_t hp_json_scan_escapes_scalar(const uint8_t *p, size_t len);
size_t hp_json_scan_escapes_avx2(const uint8_t *p, size_t len);
size_t hp_json_scan_escapes_avx512(const uint8_t *p, size_t len);

/* NDJSON input span scanners. All three return the index of the first byte
 * with the given property, or len when absent:
 *   scan_str - first '"' or '\\'          (end of a key/string span)
 *   scan_lit - first ',', '}' or ws byte  (end of a literal value)
 *   scan_ws  - first byte that is NOT whitespace (leading ws count)
 * Whitespace is the exact RFC 8259 set: SP, TAB, LF, CR. */
size_t hp_json_scan_str_scalar(const uint8_t *p, size_t len);
size_t hp_json_scan_lit_scalar(const uint8_t *p, size_t len);
size_t hp_json_scan_ws_scalar(const uint8_t *p, size_t len);
size_t hp_json_scan_str_avx2(const uint8_t *p, size_t len);
size_t hp_json_scan_lit_avx2(const uint8_t *p, size_t len);
size_t hp_json_scan_ws_avx2(const uint8_t *p, size_t len);
size_t hp_json_scan_str_avx512(const uint8_t *p, size_t len);
size_t hp_json_scan_lit_avx512(const uint8_t *p, size_t len);
size_t hp_json_scan_ws_avx512(const uint8_t *p, size_t len);

/* Structural fast-path bitmap builders: quote and backslash positions of
 * one chunk (up to 64 bytes) as bit i <-> p[i]. Only enabled bytes are
 * read, so any chunk_len is safe without padding. */
void hp_json_struct_masks_scalar(const uint8_t *p, size_t chunk_len,
                                 uint64_t *qm, uint64_t *bm);
void hp_json_struct_masks_avx2(const uint8_t *p, size_t chunk_len,
                               uint64_t *qm, uint64_t *bm);
void hp_json_struct_masks_avx512(const uint8_t *p, size_t chunk_len,
                                 uint64_t *qm, uint64_t *bm);

/* Fused string emitters (scan + copy in one pass). Each writes the escaped
 * JSON body of str into dst and returns the number of bytes written. dst
 * must have room for 6*len + 1 bytes. */
size_t hp_json_emit_body_scalar(char *dst, const char *str, size_t len);
size_t hp_json_emit_body_avx2(char *dst, const char *str, size_t len);
size_t hp_json_emit_body_avx512(char *dst, const char *str, size_t len);

/* Writes the JSON-safe form of a single byte into dst and returns the number
 * of bytes written (1 for plain bytes, 2 for short escapes, 6 for \u00XX).
 * Defined as an always-inline header function so every emitter (scalar,
 * AVX2, AVX-512) shares exactly these rules without a cross-TU call on the
 * per-byte tail path. */
HP_INLINE size_t hp_json_escape_one(char *dst, uint8_t c) {
    switch (c) {
        case '"':  dst[0] = '\\'; dst[1] = '"';  return 2;
        case '\\': dst[0] = '\\'; dst[1] = '\\'; return 2;
        case '\b': dst[0] = '\\'; dst[1] = 'b';  return 2;
        case '\f': dst[0] = '\\'; dst[1] = 'f';  return 2;
        case '\n': dst[0] = '\\'; dst[1] = 'n';  return 2;
        case '\r': dst[0] = '\\'; dst[1] = 'r';  return 2;
        case '\t': dst[0] = '\\'; dst[1] = 't';  return 2;
        default: break;
    }
    if (c >= 0x20) {
        dst[0] = (char)c;
        return 1;
    }
    static const char hex[] = "0123456789abcdef";
    dst[0] = '\\';
    dst[1] = 'u';
    dst[2] = '0';
    dst[3] = '0';
    dst[4] = hex[(c >> 4) & 0x0F];
    dst[5] = hex[c & 0x0F];
    return 6;
}

#endif /* HYPERPIPE_HP_JSON_SER_H */
