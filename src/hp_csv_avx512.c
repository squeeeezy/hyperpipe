#include "hyperpipe/hp_csv_parser.h"
#include <immintrin.h>

/* AVX-512 CSV row parser.
 *
 * Strategy: rows WITHOUT quote characters (the overwhelming majority in
 * machine-generated data) are parsed entirely with vectorized chunk scans.
 * The loop walks 64-byte chunks, pops field boundaries out of the delimiter
 * bitmask, and stops at the first CR/LF. A single quote anywhere in the row
 * discards the partial state and falls back to the RFC 4180 scalar state
 * machine, which owns all quoting edge cases (doubled quotes, quoted
 * delimiters, quoted newlines). This keeps two code paths and two behaviors:
 * SIMD for clean rows, scalar for tricky rows. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_csv_scan_special_avx512(const uint8_t *p, size_t len, uint8_t delim) {
    size_t i = 0;
    const __m512i v_delim = _mm512_set1_epi8((char)delim);
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_lf    = _mm512_set1_epi8('\n');
    const __m512i v_cr    = _mm512_set1_epi8('\r');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        __mmask64 m1 = _mm512_cmpeq_epi8_mask(chunk, v_delim);
        __mmask64 m2 = _mm512_cmpeq_epi8_mask(chunk, v_quote);
        __mmask64 m3 = _mm512_cmpeq_epi8_mask(chunk, v_lf);
        __mmask64 m4 = _mm512_cmpeq_epi8_mask(chunk, v_cr);
        uint64_t mask = (uint64_t)(m1 | m2 | m3 | m4);

        if (mask != 0) {
            return i + (size_t)__builtin_ctzll(mask);
        }
        i += 64;
    }

    while (i < len) {
        uint8_t c = p[i];
        if (c == delim || c == '"' || c == '\n' || c == '\r') {
            return i;
        }
        i++;
    }
    return len;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_csv_scan_quote_avx512(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m512i v_quote = _mm512_set1_epi8('"');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        uint64_t mask = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, v_quote);

        if (mask != 0) {
            return i + (size_t)__builtin_ctzll(mask);
        }
        i += 64;
    }

    while (i < len) {
        if (p[i] == '"') {
            return i;
        }
        i++;
    }
    return len;
}

/* Emit one zero-copy field into out_row. Returns false when the row ran out
 * of column slots (caller maps that to HP_ERR_BUFFER_OVERFLOW). */
HP_INLINE bool avx512_emit_field(hp_row_t *out_row, size_t *fc,
                                 const uint8_t *base, size_t start, size_t end) {
    if (*fc >= HP_MAX_COLUMNS) return false;
    out_row->fields[*fc].ptr = (const char *)(base + start);
    out_row->fields[*fc].len = end - start;
    out_row->fields[*fc].has_escapes = false;
    out_row->fields[*fc].present = true;
    (*fc)++;
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
hp_error_t hp_csv_parse_row_avx512(hp_csv_parser_t *parser,
                                   const uint8_t *src,
                                   size_t len,
                                   size_t *consumed,
                                   hp_row_t *out_row,
                                   bool is_eof) {
    if (!parser || !src || !consumed || !out_row) return HP_ERR_INVALID_ARG;
    if (len < 64) {
        return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
    }

    *consumed = 0;
    out_row->field_count = 0;

    size_t pos = 0;
    if (__builtin_expect(src[0] == '\r' || src[0] == '\n', 0)) {
        while (pos < len && (src[pos] == '\r' || src[pos] == '\n')) pos++;
        if (pos >= len) {
            *consumed = pos;
            return HP_OK;
        }
    }

    const uint8_t delim = parser->delim;
    const __m512i v_delim = _mm512_set1_epi8((char)delim);
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_lf    = _mm512_set1_epi8('\n');
    const __m512i v_cr    = _mm512_set1_epi8('\r');

    size_t fc = 0;
    size_t field_start = pos;

    /* ---- Phase 1: vectorized scan over full 64-byte chunks ---- */
    while (pos + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(src + pos));

        if (__builtin_expect((uint64_t)_mm512_cmpeq_epi8_mask(chunk, v_quote) != 0, 0)) {
            /* Quoted row: discard partial fields, let the scalar state
             * machine re-parse the whole row with quote semantics. */
            out_row->field_count = 0;
            return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
        }

        uint64_t delims = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, v_delim);
        uint64_t ends = (uint64_t)(_mm512_cmpeq_epi8_mask(chunk, v_lf) |
                                   _mm512_cmpeq_epi8_mask(chunk, v_cr));

        if (ends == 0) {
            while (delims) {
                uint32_t d_idx = (uint32_t)__builtin_ctzll(delims);
                if (!avx512_emit_field(out_row, &fc, src, field_start, pos + d_idx)) {
                    return HP_ERR_BUFFER_OVERFLOW;
                }
                field_start = pos + d_idx + 1;
                delims &= delims - 1;
            }
            pos += 64;
            continue;
        }

        /* Row terminator found inside this chunk: emit the fields before it
         * plus the trailing field, then report the consumed byte count. */
        uint32_t end_rel = (uint32_t)__builtin_ctzll(ends);
        uint64_t before = (end_rel < 64) ? (delims & ((1ULL << end_rel) - 1)) : delims;
        while (before) {
            uint32_t d_idx = (uint32_t)__builtin_ctzll(before);
            if (!avx512_emit_field(out_row, &fc, src, field_start, pos + d_idx)) {
                return HP_ERR_BUFFER_OVERFLOW;
            }
            field_start = pos + d_idx + 1;
            before &= before - 1;
        }
        if (!avx512_emit_field(out_row, &fc, src, field_start, pos + end_rel)) {
            return HP_ERR_BUFFER_OVERFLOW;
        }
        out_row->field_count = fc;

        if (src[pos + end_rel] == '\r' && pos + end_rel + 1 < len && src[pos + end_rel + 1] == '\n') {
            *consumed = pos + end_rel + 2;   /* CRLF */
        } else {
            *consumed = pos + end_rel + 1;   /* LF or lone CR */
        }
        return HP_OK;
    }

    /* ---- Phase 2: byte-wise tail (< 64 bytes) ----
     * Same rules as phase 1; a quote here also defers to the scalar parser. */
    while (pos < len) {
        uint8_t c = src[pos];
        if (__builtin_expect(c == '"', 0)) {
            out_row->field_count = 0;
            return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
        }
        if (c == delim) {
            if (!avx512_emit_field(out_row, &fc, src, field_start, pos)) {
                return HP_ERR_BUFFER_OVERFLOW;
            }
            field_start = pos + 1;
        } else if (c == '\n' || c == '\r') {
            if (!avx512_emit_field(out_row, &fc, src, field_start, pos)) {
                return HP_ERR_BUFFER_OVERFLOW;
            }
            out_row->field_count = fc;
            if (c == '\r' && pos + 1 < len && src[pos + 1] == '\n') {
                *consumed = pos + 2;
            } else {
                *consumed = pos + 1;
            }
            return HP_OK;
        }
        pos++;
    }

    /* ---- Phase 3: no row terminator before end of buffer ---- */
    if (!is_eof) {
        /* The record may continue in the next chunk: wait for more data.
         * Partial fields are discarded; the caller re-parses from scratch. */
        out_row->field_count = 0;
        *consumed = 0;
        return HP_OK;
    }

    /* EOF: the trailing (possibly empty, when the file ends on a delimiter)
     * field closes the row, mirroring the scalar parser's semantics. */
    if (field_start < len || fc > 0) {
        if (!avx512_emit_field(out_row, &fc, src, field_start, len)) {
            return HP_ERR_BUFFER_OVERFLOW;
        }
    }
    out_row->field_count = fc;
    *consumed = len;
    return HP_OK;
}

/* ---- Chunk planner bitmaps ------------------------------------------------
 *
 * AVX-512 twin of hp_csv_plan_masks_scalar: newline and quote bitmaps for
 * one chunk of up to 64 bytes, one compare per bitmap. The masked load
 * touches only enabled lanes, so a short tail chunk is read without any
 * padding guarantee from the caller. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
void hp_csv_plan_masks_avx512(const uint8_t *p, size_t chunk_len,
                              uint64_t *nl, uint64_t *qm) {
    __mmask64 len_mask = (chunk_len >= 64) ? ~0ull : ((1ull << chunk_len) - 1);
    __m512i chunk = _mm512_maskz_loadu_epi8(len_mask, (const void *)p);
    *nl = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, _mm512_set1_epi8('\n')) & len_mask;
    *qm = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, _mm512_set1_epi8('"')) & len_mask;
}
