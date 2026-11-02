#include "hyperpipe/hp_json_ser.h"
#include "hyperpipe/hp_cpuid.h"
#include "hyperpipe/hp_keymap.h"

size_t hp_json_scan_escapes_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            return i;
        }
        i++;
    }
    return len;
}

/* ---- NDJSON input span scanners (scalar reference implementations) ------
 *
 * Three primitives drive the line parser:
 *   scan_str - index of the first '"' or '\\' (end of a key/string span)
 *   scan_lit - index of the first value terminator (',', '}' or whitespace)
 *   scan_ws  - index of the first byte that is NOT JSON whitespace
 *
 * The SIMD twins in hp_json_avx2.c / hp_json_avx512.c return identical
 * results and are selected through the dispatch table. */

size_t hp_json_scan_str_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len && p[i] != '"' && p[i] != '\\') i++;
    return i;
}

size_t hp_json_scan_lit_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        i++;
    }
    return i;
}

/* Builds the quote/backslash bitmaps for one line chunk with a byte loop.
 * This is both the portable reference and the fallback for short chunks on
 * SIMD builds (see hp_json_struct_masks_avx2/avx512). */
void hp_json_struct_masks_scalar(const uint8_t *p, size_t chunk_len,
                                 uint64_t *qm, uint64_t *bm) {
    uint64_t q = 0, b = 0;
    for (size_t i = 0; i < chunk_len; i++) {
        uint64_t bit = 1ull << i;
        q |= (p[i] == '"') ? bit : 0;
        b |= (p[i] == '\\') ? bit : 0;
    }
    *qm = q;
    *bm = b;
}

size_t hp_json_scan_ws_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    return i;
}

/* Scalar fallback emitter. Clean values (the overwhelming majority in real
 * data) take a single memcpy; anything containing an escapable byte copies
 * its clean prefix and expands the remainder through the shared expander. */
size_t hp_json_emit_body_scalar(char *dst, const char *str, size_t len) {
    size_t k = 0;
    while (k < len) {
        uint8_t c = (uint8_t)str[k];
        if (c == '"' || c == '\\' || c < 0x20) break;
        k++;
    }
    memcpy(dst, str, k);
    size_t w = k;
    for (size_t i = k; i < len; i++) {
        w += hp_json_escape_one(dst + w, (uint8_t)str[i]);
    }
    return w;
}

static hp_error_t hp_json_ser_grow(hp_json_ser_t *ser, size_t needed) {
    if (ser->pos + needed <= ser->capacity) return HP_OK;
    size_t new_cap = (ser->capacity == 0) ? 4096 : ser->capacity * 2;
    while (ser->pos + needed > new_cap) {
        new_cap *= 2;
    }
    char *new_buf = (char *)realloc(ser->out_buf, new_cap);
    if (!new_buf) return HP_ERR_NOMEM;
    ser->out_buf = new_buf;
    ser->capacity = new_cap;
    return HP_OK;
}

hp_error_t hp_json_ser_init(hp_json_ser_t *ser, size_t initial_capacity, hp_simd_mode_t simd) {
    (void)simd;
    if (!ser) return HP_ERR_INVALID_ARG;
    if (initial_capacity < 1024) initial_capacity = 1024;
    ser->out_buf = (char *)malloc(initial_capacity);
    if (!ser->out_buf) return HP_ERR_NOMEM;
    ser->capacity = initial_capacity;
    ser->pos = 0;
    ser->scan_escapes = hp_json_scan_escapes_scalar;
    ser->emit_string = hp_json_emit_body_scalar;
    ser->frag_buf = NULL;
    ser->frag_cap = 0;
    ser->frag_count = 0;
    return HP_OK;
}

void hp_json_ser_free(hp_json_ser_t *ser) {
    if (!ser) return;
    if (ser->out_buf) {
        free(ser->out_buf);
        ser->out_buf = NULL;
    }
    if (ser->frag_buf) {
        free(ser->frag_buf);
        ser->frag_buf = NULL;
    }
    ser->frag_cap = 0;
    ser->frag_count = 0;
    ser->capacity = 0;
    ser->pos = 0;
}

void hp_json_ser_reset(hp_json_ser_t *ser) {
    if (ser) ser->pos = 0;
}

hp_error_t hp_json_ser_write_string(hp_json_ser_t *ser, const char *str, size_t len) {
    if (!ser || (!str && len > 0)) return HP_ERR_INVALID_ARG;

    /* Reserve worst-case space (6 output bytes per input byte) plus quotes */
    hp_error_t err = hp_json_ser_grow(ser, len * 6 + 2);
    if (err != HP_OK) return err;

    /* Short-string fast path: values that fit in the inline window and are
     * free of escapes skip the indirect emit_string call entirely. Eight
     * bytes cover most real-world short strings (IDs, flags, status words). */
    if (len <= 8) {
        const uint8_t *q = (const uint8_t *)str;
        size_t k = 0;
        while (k < len && q[k] >= 0x20 && q[k] != '"' && q[k] != '\\') k++;
        if (k == len) {
            ser->out_buf[ser->pos++] = '"';
            memcpy(ser->out_buf + ser->pos, str, len);
            ser->pos += len;
            ser->out_buf[ser->pos++] = '"';
            return HP_OK;
        }
    }

    ser->out_buf[ser->pos++] = '"';
    ser->pos += ser->emit_string(ser->out_buf + ser->pos, str, len);
    ser->out_buf[ser->pos++] = '"';
    return HP_OK;
}

bool hp_json_is_number(const char *s, size_t len) {
    if (len == 0) return false;
    /* Cheap pre-filter: a JSON number can only start with a digit or '-'
     * (a leading '.' is invalid JSON). One byte-sized test rejects the vast
     * majority of string fields before the full grammar check below. */
    uint8_t c0 = (uint8_t)s[0];
    if ((c0 < '0' || c0 > '9') && c0 != '-') return false;
    size_t i = 0;
    if (s[0] == '-') i++;
    if (i >= len) return false;

    /* Fast path for unsigned and signed integers */
    size_t digits_start = i;
    while (i < len && (uint8_t)(s[i] - '0') <= 9) i++;
    if (i == len && i > digits_start) {
        if (s[digits_start] == '0' && (len - digits_start) > 1) return false;
        return true;
    }

    /* Reset and check full floating-point specification */
    i = (s[0] == '-') ? 1 : 0;
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < len && (s[i] >= '0' && s[i] <= '9')) i++;
    } else {
        return false;
    }

    if (i < len && s[i] == '.') {
        i++;
        if (i >= len || !(s[i] >= '0' && s[i] <= '9')) return false;
        while (i < len && (s[i] >= '0' && s[i] <= '9')) i++;
    }

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= len || !(s[i] >= '0' && s[i] <= '9')) return false;
        while (i < len && (s[i] >= '0' && s[i] <= '9')) i++;
    }

    return i == len;
}

/* Converts a numeric span to double. The span is copied into a bounded stack
 * buffer and handed to strtod, so rounding and locale behavior stay with the
 * C library; use hp_json_is_number first when the strict JSON grammar is
 * required (hp_json_number_as_double also accepts what strtod accepts, e.g.
 * "inf" - matching the filter engine's historical numeric semantics). */
bool hp_json_number_as_double(const char *s, size_t len, double *out) {
    if (len == 0 || len > 63) return false;
    char buf[64];
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf || *end != '\0') return false;
    if (out) *out = v;
    return true;
}

/* Worst-case serialized size of one row. Every present field can expand to
 * 6 bytes per input byte, plus quotes; keys use their escaped fragment
 * length when fragments are bound, strlen*6+3 otherwise; plus braces, the
 * trailing newline and one comma per field. Computing the bound once lets
 * both hp_json_ser_row and hp_json_ser_row_direct guarantee that the emit
 * loop below never overruns its destination. */
static size_t ser_row_bound(const hp_json_ser_t *ser,
                            const hp_header_t *header,
                            const hp_row_t *row) {
    const size_t count = row->field_count;
    const bool use_fragments = (ser->frag_count == count && count > 0);

    size_t bound = 3 + count; /* '{', '}', '\n', worst-case commas */
    for (size_t i = 0; i < count; i++) {
        if (!row->fields[i].present) continue;
        size_t key_len;
        if (use_fragments) {
            key_len = ser->frag_lens[i];
        } else if (header && i < header->count && header->names[i]) {
            key_len = strlen(header->names[i]) * 6 + 3; /* quotes + colon */
        } else {
            key_len = 40; /* "col_1023": fits, escaped: still << 40 */
        }
        bound += key_len + row->fields[i].len * 6 + 2;
    }
    return bound;
}

/* Core emitter shared by both public entry points. Requires that dst has
 * room for at least ser_row_bound() bytes; callers enforce that. */
static void ser_row_emit(const hp_json_ser_t *ser,
                         const hp_header_t *header,
                         const hp_row_t *row,
                         char *dst,
                         size_t *out_len) {
    const size_t count = row->field_count;
    const bool use_fragments = (ser->frag_count == count && count > 0);

    size_t w = 0;
    dst[w++] = '{';

    bool first = true;
    for (size_t i = 0; i < count; i++) {
        if (!row->fields[i].present) continue;   /* sparse row: omit absent keys */
        if (!first) {
            dst[w++] = ',';
        }
        first = false;

        /* ---- key ---- */
        if (use_fragments) {
            /* Memoized `"key":` fragment: one memcpy per key */
            memcpy(dst + w, ser->frag_buf + ser->frag_offsets[i], ser->frag_lens[i]);
            w += ser->frag_lens[i];
        } else {
            const char *key = (header && i < header->count) ? header->names[i] : NULL;
            char default_key[32];
            if (!key) {
                snprintf(default_key, sizeof(default_key), "col_%zu", i);
                key = default_key;
            }

            dst[w++] = '"';
            w += hp_json_emit_body_scalar(dst + w, key, strlen(key));
            dst[w++] = '"';
            dst[w++] = ':';
        }

        /* ---- value ---- */
        const hp_str_t *f = &row->fields[i];
        if (!f->has_escapes && hp_json_is_number(f->ptr, f->len)) {
            memcpy(dst + w, f->ptr, f->len);
            w += f->len;
        } else if (!f->has_escapes && f->len == 4 && memcmp(f->ptr, "true", 4) == 0) {
            memcpy(dst + w, "true", 4);
            w += 4;
        } else if (!f->has_escapes && f->len == 5 && memcmp(f->ptr, "false", 5) == 0) {
            memcpy(dst + w, "false", 5);
            w += 5;
        } else if (!f->has_escapes && f->len == 4 && memcmp(f->ptr, "null", 4) == 0) {
            memcpy(dst + w, "null", 4);
            w += 4;
        } else {
            dst[w++] = '"';
            if (f->len <= 8) {
                /* Short-value fast path: an inline scan avoids the indirect
                 * emit_string call for the many tiny strings in real data
                 * (flags, "-" placeholders, short IDs). */
                const uint8_t *q = (const uint8_t *)f->ptr;
                size_t k = 0;
                while (k < f->len && q[k] >= 0x20 && q[k] != '"' && q[k] != '\\') k++;
                if (k == f->len) {
                    memcpy(dst + w, f->ptr, f->len);
                    w += f->len;
                } else {
                    for (size_t r = 0; r < f->len; r++) {
                        w += hp_json_escape_one(dst + w, (uint8_t)f->ptr[r]);
                    }
                }
            } else {
                w += ser->emit_string(dst + w, f->ptr, f->len);
            }
            dst[w++] = '"';
        }
    }

    dst[w++] = '}';
    dst[w++] = '\n';
    *out_len = w;
}

hp_error_t hp_json_ser_row_direct(const hp_json_ser_t *ser,
                                  const hp_header_t *header,
                                  const hp_row_t *row,
                                  char *dst,
                                  size_t cap,
                                  size_t *out_len) {
    if (!ser || !row || !dst || !out_len) return HP_ERR_INVALID_ARG;

    size_t need = ser_row_bound(ser, header, row);
    if (need > cap) return HP_ERR_BUFFER_OVERFLOW;

    ser_row_emit(ser, header, row, dst, out_len);
    return HP_OK;
}

hp_error_t hp_json_ser_row(hp_json_ser_t *ser,
                           const hp_header_t *header,
                           const hp_row_t *row,
                           const char **out_line,
                           size_t *out_line_len) {
    if (!ser || !row || !out_line || !out_line_len) return HP_ERR_INVALID_ARG;

    size_t need = ser_row_bound(ser, header, row);
    hp_error_t err = hp_json_ser_grow(ser, need);
    if (err != HP_OK) return err;

    size_t line_len = 0;
    ser_row_emit(ser, header, row, ser->out_buf, &line_len);
    ser->pos = line_len;

    *out_line = ser->out_buf;
    *out_line_len = line_len;
    return HP_OK;
}

/* Parses the 4 hex digits of a \uXXXX escape; -1 on malformed input. */
static int json_hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return -1;
    }
    return v;
}

/* Encode codepoint as UTF-8 into dst (capacity avail). Returns bytes written,
 * 0 when the buffer is too small. */
static size_t json_utf8_encode(char *dst, size_t avail, unsigned cp) {
    size_t n = (cp <= 0x7F) ? 1 : (cp <= 0x7FF) ? 2 : (cp <= 0xFFFF) ? 3 : 4;
    if (n > avail) return 0;
    switch (n) {
        case 1: dst[0] = (char)cp; break;
        case 2: dst[0] = (char)(0xC0 | (cp >> 6));
                dst[1] = (char)(0x80 | (cp & 0x3F)); break;
        case 3: dst[0] = (char)(0xE0 | (cp >> 12));
                dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[2] = (char)(0x80 | (cp & 0x3F)); break;
        default: dst[0] = (char)(0xF0 | (cp >> 18));
                 dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                 dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                 dst[3] = (char)(0x80 | (cp & 0x3F)); break;
    }
    return n;
}

/* JSON allows exactly four whitespace characters (RFC 8259 section 2). */
static inline bool json_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Hybrid span scanners: SIMD for spans of 32+ bytes, an inline scalar walk
 * for the short keys and values that dominate real NDJSON. Both paths return
 * identical results; the threshold only avoids call overhead on short spans. */

HP_INLINE size_t j_scan_str(size_t (*simd)(const uint8_t *, size_t),
                                   const char *p, size_t len) {
    if (len >= 32) return simd((const uint8_t *)p, len);
    size_t i = 0;
    while (i < len && p[i] != '"' && p[i] != '\\') i++;
    return i;
}

HP_INLINE size_t j_scan_lit(size_t (*simd)(const uint8_t *, size_t),
                                   const char *p, size_t len) {
    if (len >= 32) return simd((const uint8_t *)p, len);
    size_t i = 0;
    while (i < len) {
        char c = p[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        i++;
    }
    return i;
}

HP_INLINE size_t j_scan_ws(size_t (*simd)(const uint8_t *, size_t),
                                  const char *p, size_t len) {
    if (len >= 32) return simd((const uint8_t *)p, len);
    size_t i = 0;
    while (i < len && json_is_space(p[i])) i++;
    return i;
}

/* ---- SIMD structural fast path ------------------------------------------
 *
 * Machine-generated NDJSON lines are escape-free "key":value chains. For
 * them the parser does not need to branch on most bytes: one vector pass
 * per 64-byte chunk yields the quote positions, and every key and string
 * value becomes pure span arithmetic between structural quotes. Literal
 * values (numbers, true/false/null) are short and keep a small scalar scan.
 *
 * The walker never mutates shared state before the whole line is known to
 * fit its shape: unknown keys, a backslash anywhere, or unexpected
 * structure abort the walk and the byte-span scanner below re-parses the
 * line from scratch with the exact historical semantics. Escape-free lines
 * skip per-byte branching entirely; escaped lines pay one wasted scan. */

typedef struct hp_struct_scan_s {
    const char *line;
    size_t len;
    size_t c_off;      /* offset of the loaded chunk */
    size_t c_len;      /* bytes covered by the chunk masks (<= 64) */
    uint64_t qm, bm;   /* quote / backslash bitmaps, bit i <-> line[c_off+i] */
} hp_struct_scan_t;

/* Loads the chunk starting at `off`. Returns false when a backslash appears
 * (escape handling belongs to the general path) or the line ended. */
static bool struct_load_chunk(hp_struct_scan_t *s, size_t off) {
    if (off >= s->len) return false;
    s->c_off = off;
    s->c_len = (s->len - off > 64) ? 64 : s->len - off;
    g_hp_dispatch.json_struct_masks((const uint8_t *)s->line + off,
                                    s->c_len, &s->qm, &s->bm);
    return s->bm == 0;
}

/* Absolute offset of the first structural quote at or after `from`, or -1
 * when the line ends first. Chunks are loaded on demand. */
static long struct_next_quote(hp_struct_scan_t *s, size_t from) {
    size_t pos = from;
    for (;;) {
        if (pos >= s->len) return -1;
        if (pos >= s->c_off + s->c_len) {
            if (!struct_load_chunk(s, s->c_off + s->c_len)) return -1;
            if (pos < s->c_off) pos = s->c_off;
        }
        size_t rel = pos - s->c_off;
        if (rel < s->c_len) {
            uint64_t m = s->qm >> rel;
            if (m) return (long)(s->c_off + rel + (size_t)__builtin_ctzll(m));
        }
        pos = s->c_off + s->c_len;
    }
}

/* Walks one escape-free line. Returns true when the line matched the
 * structural shape and all fields are assigned; false means "fall back". */
static bool struct_walk(hp_struct_scan_t *s,
                        hp_header_t *header,
                        hp_keymap_t *keymap,
                        hp_row_t *out_row) {
    const char *line = s->line;
    const size_t len = s->len;
    size_t pos = 1; /* the caller verified line[0] == '{' */

    for (;;) {
        while (pos < len && json_is_space(line[pos])) pos++;
        if (pos >= len) return false;
        if (line[pos] == '}') return true;
        if (line[pos] != '"') return false;

        long q0 = struct_next_quote(s, pos);
        if (q0 < 0 || (size_t)q0 != pos) return false;
        long q1 = struct_next_quote(s, pos + 1);
        if (q1 < 0) return false;
        size_t key_len = (size_t)q1 - (pos + 1);
        if (key_len > 255) return false; /* truncation is general-path work */

        /* colon region: ws ':' ws */
        size_t p = (size_t)q1 + 1;
        while (p < len && json_is_space(line[p])) p++;
        if (p >= len || line[p] != ':') return false;
        p++;
        while (p < len && json_is_space(line[p])) p++;
        if (p >= len) return false;

        const char *val_ptr;
        size_t val_len;
        bool val_is_string = false;

        if (line[p] == '"') {
            long qv = struct_next_quote(s, p + 1);
            if (qv < 0) return false;
            val_ptr = line + p + 1;
            val_len = (size_t)qv - (p + 1);
            val_is_string = true;
            p = (size_t)qv + 1;
        } else {
            /* literal: numbers and true/false/null are short; the scalar
             * terminator scan costs less than a vector round trip */
            size_t vstart = p;
            while (p < len && line[p] != ',' && line[p] != '}' &&
                   !json_is_space(line[p])) {
                p++;
            }
            val_ptr = line + vstart;
            val_len = p - vstart;
        }

        /* Resolve by key. Unknown keys grow the header, which is
         * general-path work, so they fall back. */
        int64_t hit = hp_keymap_find(keymap, header, line + q0 + 1,
                                     key_len, false);
        if (hit < 0) return false;
        size_t idx = (size_t)hit;

        out_row->fields[idx].ptr = val_ptr;
        out_row->fields[idx].len = val_len;
        out_row->fields[idx].has_escapes = val_is_string;
        out_row->fields[idx].present = true;
        if (idx + 1 > out_row->field_count) out_row->field_count = idx + 1;

        while (p < len && json_is_space(line[p])) p++;
        if (p >= len) return false;
        if (line[p] == '}') return true;
        if (line[p] != ',') return false;
        pos = p + 1;
    }
}

/* Reverse conversion: parses an NDJSON line into an hp_row_t and extends the
 * running header.
 *
 * Architecture (v1.5):
 *  - Key and string spans are located with the dispatchable span scanners
 *    above, not byte-at-a-time branching.
 *  - Values are ZERO-COPY whenever they need no unescaping: a field points
 *    straight into the caller's line buffer, which must stay valid until the
 *    row has been consumed (the transcoder serializes or filters each row
 *    before advancing the ring buffer). Only strings containing backslash
 *    escapes are decoded into value_arena.
 *  - Field slots are resolved BY KEY against the running header. With a
 *    keymap bound, lookup is a hashed probe with one confirming memcmp;
 *    without one it is the historical linear strcmp walk. Reordered keys
 *    land on the correct column, unknown keys extend the header (and the
 *    keymap), and keys absent from this line keep present = false. */
hp_error_t hp_json_parse_line_ex(const char *line,
                                 size_t len,
                                 hp_header_t *header,
                                 hp_keymap_t *keymap,
                                 hp_row_t *out_row,
                                 char *value_arena,
                                 size_t arena_size) {
    if (!line || !out_row) return HP_ERR_INVALID_ARG;

    /* Span scanners for this call; scalar fallbacks are always valid, so the
     * dispatch pointers need no NULL checks. */
    size_t (*scan_str)(const uint8_t *, size_t) = g_hp_dispatch.json_scan_str;
    size_t (*scan_lit)(const uint8_t *, size_t) = g_hp_dispatch.json_scan_lit;
    size_t (*scan_ws)(const uint8_t *, size_t)  = g_hp_dispatch.json_scan_ws;

    const uint8_t *u = (const uint8_t *)line;

    size_t pos = j_scan_ws(scan_ws, line, len);
    if (pos >= len || line[pos] != '{') return HP_ERR_PARSE_QUOTE;
    pos++;

    size_t arena_pos = 0;

    /* Reset schema slots for this row so absent keys are detectable */
    if (header) {
        for (size_t i = 0; i < header->count; i++) {
            out_row->fields[i].ptr = "";
            out_row->fields[i].len = 0;
            out_row->fields[i].has_escapes = false;
            out_row->fields[i].present = false;
        }
        out_row->field_count = header->count;
    } else {
        out_row->field_count = 0;
    }

    /* ---- SIMD structural fast path ----
     * Engages once a keymap is bound (the general path below builds it from
     * the first row) and the line is long enough for the chunked bitmap
     * pass to pay off. Any structural surprise falls back to the general
     * scanner, which re-parses the line from scratch. */
    if (header && keymap && len >= 32 && g_hp_dispatch.json_struct_masks) {
        hp_struct_scan_t ss;
        ss.line = line;
        ss.len = len;
        ss.c_off = 0;
        ss.c_len = 0;
        ss.qm = 0;
        ss.bm = 0;
        if (struct_walk(&ss, header, keymap, out_row)) {
            return HP_OK;
        }
        /* Re-run the slot reset: the walker may have assigned fields before
         * falling back. It never grew the header or the keymap. */
        for (size_t i = 0; i < header->count; i++) {
            out_row->fields[i].ptr = "";
            out_row->fields[i].len = 0;
            out_row->fields[i].has_escapes = false;
            out_row->fields[i].present = false;
        }
        out_row->field_count = header->count;
    }

    while (pos < len) {
        pos += j_scan_ws(scan_ws, line + pos, len - pos);
        if (pos < len && line[pos] == '}') break;

        /* ---- key: raw span scan; escapes are skipped, never decoded ---- */
        if (pos >= len || line[pos] != '"') return HP_ERR_PARSE_QUOTE;
        pos++;
        size_t key_start = pos;
        while (pos < len) {
            pos += j_scan_str(scan_str, line + pos, len - pos);
            if (pos >= len) return HP_ERR_PARSE_QUOTE; /* unterminated key */
            if (line[pos] == '"') break;
            /* backslash: the escape pair (and its quote) stays inside the
             * key; a trailing lone backslash terminates the line */
            pos += (pos + 1 < len) ? 2 : 1;
        }
        if (pos >= len) return HP_ERR_PARSE_QUOTE;
        size_t key_len = pos - key_start;
        pos++; /* closing quote */

        /* Keys longer than the lookup buffer are truncated exactly as in
         * previous versions, so long-key behavior stays bit-identical. */
        size_t key_cmp_len = key_len;
        char key_buf[256];
        if (key_cmp_len >= sizeof(key_buf)) key_cmp_len = sizeof(key_buf) - 1;

        pos += j_scan_ws(scan_ws, line + pos, len - pos);
        if (pos >= len || line[pos] != ':') return HP_ERR_PARSE_QUOTE;
        pos++; /* skip ':' */
        pos += j_scan_ws(scan_ws, line + pos, len - pos);

        /* ---- resolve slot: existing column by key, else extend header ---- */
        size_t idx;
        if (header) {
            if (keymap) {
                /* padded_key = true: the ring buffer guarantees 64 readable
                 * bytes past the line, so the 8-byte fast-path load is safe */
                int64_t hit = hp_keymap_find(keymap, header, line + key_start,
                                             key_cmp_len, true);
                if (hit >= 0) {
                    idx = (size_t)hit;
                } else {
                    if (header->count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                    memcpy(key_buf, line + key_start, key_cmp_len);
                    key_buf[key_cmp_len] = '\0';
                    header->names[header->count] = strdup(key_buf);
                    if (!header->names[header->count]) return HP_ERR_NOMEM;
                    idx = header->count++;
                    out_row->fields[idx].ptr = "";
                    out_row->fields[idx].len = 0;
                    out_row->fields[idx].has_escapes = false;
                    out_row->fields[idx].present = false;
                    hp_error_t ierr = hp_keymap_insert(keymap, header, idx);
                    if (ierr != HP_OK) return ierr;
                }
            } else {
                memcpy(key_buf, line + key_start, key_cmp_len);
                key_buf[key_cmp_len] = '\0';
                size_t h = 0;
                for (; h < header->count; h++) {
                    if (strcmp(header->names[h], key_buf) == 0) break;
                }
                if (h == header->count) {
                    if (h >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                    header->names[h] = strdup(key_buf);
                    if (!header->names[h]) return HP_ERR_NOMEM;
                    header->count++;
                    out_row->fields[h].ptr = "";
                    out_row->fields[h].len = 0;
                    out_row->fields[h].has_escapes = false;
                    out_row->fields[h].present = false;
                }
                idx = h;
            }
        } else {
            if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
            idx = out_row->field_count;
        }

        /* ---- value ---- */
        const char *val_ptr;
        size_t val_len;
        bool val_is_string = false;
        size_t val_start_arena = arena_pos;

        if (pos < len && line[pos] == '"') {
            /* String value: has_escapes = true keeps the write path
             * type-faithful (e.g. "null" stays "null", not null). */
            val_is_string = true;
            pos++;
            size_t vbegin = pos;

            /* One span scan to the first '"' or '\\'. A terminated clean
             * string is zero-copy; anything containing a backslash decodes
             * into the arena below. */
            size_t stop = vbegin + j_scan_str(scan_str, line + vbegin, len - vbegin);
            if (stop < len && u[stop] == '"') {
                val_ptr = line + vbegin;
                val_len = stop - vbegin;
                pos = stop + 1;
            } else if (stop >= len) {
                /* Unterminated, escape-free tail: the raw rest is the value
                 * (bit-identical to the previous byte-wise decoder). */
                val_ptr = line + vbegin;
                val_len = len - vbegin;
                pos = len;
            } else {
                /* Escaped string: copy the clean prefix, then decode the
                 * remainder through the exact same rules as before. */
                size_t prefix = stop - vbegin;
                if (prefix > arena_size - arena_pos) return HP_ERR_BUFFER_OVERFLOW;
                memcpy(value_arena + arena_pos, line + vbegin, prefix);
                arena_pos += prefix;
                pos = stop;

                while (pos < len && line[pos] != '"') {
                    if (line[pos] == '\\' && pos + 1 < len) {
                        char esc = line[pos + 1];
                        if (esc == 'u') {
                            int hi = (pos + 5 < len) ? json_hex4(line + pos + 2) : -1;
                            if (hi >= 0) {
                                unsigned cp = (unsigned)hi;
                                pos += 6;
                                if (cp >= 0xD800 && cp <= 0xDBFF &&
                                    pos + 1 < len && line[pos] == '\\' && line[pos + 1] == 'u') {
                                    int lo = (pos + 5 < len) ? json_hex4(line + pos + 2) : -1;
                                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (unsigned)(lo - 0xDC00);
                                        pos += 6;
                                    } else {
                                        cp = 0xFFFDu; /* lone high surrogate */
                                    }
                                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                                    cp = 0xFFFDu; /* lone surrogate */
                                }
                                size_t n = json_utf8_encode(value_arena + arena_pos,
                                                            arena_size - arena_pos, cp);
                                if (n == 0) return HP_ERR_BUFFER_OVERFLOW;
                                arena_pos += n;
                                continue;
                            }
                            /* invalid \u sequence -> fall through, lenient */
                        }
                        char unescaped = esc;
                        if (esc == 'n') unescaped = '\n';
                        else if (esc == 'r') unescaped = '\r';
                        else if (esc == 't') unescaped = '\t';
                        else if (esc == 'b') unescaped = '\b';
                        else if (esc == 'f') unescaped = '\f';
                        else if (esc == '"') unescaped = '"';
                        else if (esc == '\\') unescaped = '\\';
                        if (arena_pos == arena_size) return HP_ERR_BUFFER_OVERFLOW;
                        value_arena[arena_pos++] = unescaped;
                        pos += 2;
                    } else {
                        if (arena_pos == arena_size) return HP_ERR_BUFFER_OVERFLOW;
                        value_arena[arena_pos++] = line[pos];
                        pos++;
                    }
                }
                if (pos < len && line[pos] == '"') pos++;

                val_ptr = value_arena + val_start_arena;
                val_len = arena_pos - val_start_arena;
            }
        } else {
            /* Literal/number value: scan the span, keep it zero-copy */
            size_t lit_start = pos;
            pos += j_scan_lit(scan_lit, line + pos, len - pos);
            val_ptr = line + lit_start;
            val_len = pos - lit_start;
        }

        if (idx < HP_MAX_COLUMNS) {
            out_row->fields[idx].ptr = val_ptr;
            out_row->fields[idx].len = val_len;
            out_row->fields[idx].has_escapes = val_is_string;
            out_row->fields[idx].present = true;
            if (idx >= out_row->field_count) out_row->field_count = idx + 1;
        }

        pos += j_scan_ws(scan_ws, line + pos, len - pos);
        if (pos < len && line[pos] == ',') {
            pos++;
        }
    }

    return HP_OK;
}

/* Compatibility wrapper: the historical entry point without a keymap. */
hp_error_t hp_json_parse_line(const char *line,
                              size_t len,
                              hp_header_t *header,
                              hp_row_t *out_row,
                              char *value_arena,
                              size_t arena_size) {
    return hp_json_parse_line_ex(line, len, header, NULL, out_row,
                                 value_arena, arena_size);
}

/* ---- Key fragment cache ------------------------------------------------
 *
 * Building `"name":` per key per row costs a strlen plus two scalar stores
 * per field; over millions of rows that is measurable. The header changes
 * rarely (once at start, plus occasional NDJSON schema growth), so the
 * fragments are memoized and rebuilt only when this function is called.
 * Keys go through the same escaping rules as values, so header names that
 * contain quotes, backslashes or control bytes still produce valid JSON. */
static hp_error_t hp_json_ser_bind_keys_impl(hp_json_ser_t *ser, const hp_header_t *header) {
    if (!ser || !header) return HP_ERR_INVALID_ARG;

    /* Worst-case space: 6 output bytes per input byte, plus quotes/colon */
    size_t need = 0;
    for (size_t i = 0; i < header->count && i < HP_MAX_COLUMNS; i++) {
        const char *n = header->names[i] ? header->names[i] : "";
        need += strlen(n) * 6 + 8;
    }

    if (need > ser->frag_cap) {
        size_t new_cap = ser->frag_cap ? ser->frag_cap : 1024;
        while (new_cap < need) new_cap *= 2;
        char *nb = (char *)realloc(ser->frag_buf, new_cap);
        if (!nb) return HP_ERR_NOMEM;
        ser->frag_buf = nb;
        ser->frag_cap = new_cap;
    }

    size_t w = 0;
    size_t n_keys = 0;
    for (size_t i = 0; i < header->count && i < HP_MAX_COLUMNS; i++) {
        const char *name = header->names[i] ? header->names[i] : "";
        size_t name_len = strlen(name);
        ser->frag_offsets[i] = w;
        ser->frag_buf[w++] = '"';
        w += hp_json_emit_body_scalar(ser->frag_buf + w, name, name_len);
        ser->frag_buf[w++] = '"';
        ser->frag_buf[w++] = ':';
        ser->frag_lens[i] = w - ser->frag_offsets[i];
        n_keys++;
    }

    ser->frag_count = n_keys;
    return HP_OK;
}

hp_error_t hp_json_ser_bind_keys(hp_json_ser_t *ser, const hp_header_t *header) {
    hp_error_t err = hp_json_ser_bind_keys_impl(ser, header);
    if (err != HP_OK) {
        /* On allocation failure fall back to the general strlen path */
        ser->frag_count = 0;
    }
    return err;
}
