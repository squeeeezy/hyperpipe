#include "hyperpipe/hp_csv_parser.h"
#include <immintrin.h>

/* AVX2 CSV row parser - the 256-bit twin of hp_csv_avx512.c.
 *
 * Same contract as the AVX-512 parser: quote-free rows are parsed entirely
 * with vectorized chunk scans; a single quote discards partial state and
 * defers the whole row to the RFC 4180 scalar state machine. This gives
 * AVX2-only hosts (Intel client cores, Zen 1-3) the same clean-row fast
 * path that AVX-512 hosts get, at half the vector width. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_csv_scan_special_avx2(const uint8_t *p, size_t len, uint8_t delim) {
    size_t i = 0;
    const __m256i v_delim = _mm256_set1_epi8((char)delim);
    const __m256i v_quote = _mm256_set1_epi8('"');
    const __m256i v_lf    = _mm256_set1_epi8('\n');
    const __m256i v_cr    = _mm256_set1_epi8('\r');

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        __m256i m1 = _mm256_cmpeq_epi8(chunk, v_delim);
        __m256i m2 = _mm256_cmpeq_epi8(chunk, v_quote);
        __m256i m3 = _mm256_cmpeq_epi8(chunk, v_lf);
        __m256i m4 = _mm256_cmpeq_epi8(chunk, v_cr);
        __m256i special = _mm256_or_si256(_mm256_or_si256(m1, m2), _mm256_or_si256(m3, m4));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(special);

        if (mask != 0) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }

    /* Scalar tail for trailing < 32 bytes */
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
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_csv_scan_quote_avx2(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m256i v_quote = _mm256_set1_epi8('"');

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        __m256i m = _mm256_cmpeq_epi8(chunk, v_quote);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);

        if (mask != 0) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }

    while (i < len) {
        if (p[i] == '"') {
            return i;
        }
        i++;
    }
    return len;
}

/* Emit one zero-copy field into out_row (mirrors hp_csv_avx512.c). */
HP_INLINE bool avx2_emit_field(hp_row_t *out_row, size_t *fc,
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
__attribute__((target("avx2,bmi2")))
#endif
hp_error_t hp_csv_parse_row_avx2(hp_csv_parser_t *parser,
                                 const uint8_t *src,
                                 size_t len,
                                 size_t *consumed,
                                 hp_row_t *out_row,
                                 bool is_eof) {
    if (!parser || !src || !consumed || !out_row) return HP_ERR_INVALID_ARG;
    if (len < 32) {
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
    const __m256i v_delim = _mm256_set1_epi8((char)delim);
    const __m256i v_quote = _mm256_set1_epi8('"');
    const __m256i v_lf    = _mm256_set1_epi8('\n');
    const __m256i v_cr    = _mm256_set1_epi8('\r');

    size_t fc = 0;
    size_t field_start = pos;

    /* ---- Phase 1: vectorized scan over full 32-byte chunks ---- */
    while (pos + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(src + pos));

        if (__builtin_expect((uint32_t)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(chunk, v_quote)) != 0, 0)) {
            /* Quoted row: discard partial fields, scalar re-parses it all */
            out_row->field_count = 0;
            return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
        }

        uint32_t delims = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, v_delim));
        uint32_t ends = (uint32_t)_mm256_movemask_epi8(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_lf), _mm256_cmpeq_epi8(chunk, v_cr)));

        if (ends == 0) {
            while (delims) {
                uint32_t d_idx = (uint32_t)__builtin_ctz(delims);
                if (!avx2_emit_field(out_row, &fc, src, field_start, pos + d_idx)) {
                    return HP_ERR_BUFFER_OVERFLOW;
                }
                field_start = pos + d_idx + 1;
                delims &= delims - 1;
            }
            pos += 32;
            continue;
        }

        uint32_t end_rel = (uint32_t)__builtin_ctz(ends);
        uint32_t before = (end_rel < 32) ? (delims & ((1U << end_rel) - 1)) : delims;
        while (before) {
            uint32_t d_idx = (uint32_t)__builtin_ctz(before);
            if (!avx2_emit_field(out_row, &fc, src, field_start, pos + d_idx)) {
                return HP_ERR_BUFFER_OVERFLOW;
            }
            field_start = pos + d_idx + 1;
            before &= before - 1;
        }
        if (!avx2_emit_field(out_row, &fc, src, field_start, pos + end_rel)) {
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

    /* ---- Phase 2: byte-wise tail (< 32 bytes) ---- */
    while (pos < len) {
        uint8_t c = src[pos];
        if (__builtin_expect(c == '"', 0)) {
            out_row->field_count = 0;
            return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
        }
        if (c == delim) {
            if (!avx2_emit_field(out_row, &fc, src, field_start, pos)) {
                return HP_ERR_BUFFER_OVERFLOW;
            }
            field_start = pos + 1;
        } else if (c == '\n' || c == '\r') {
            if (!avx2_emit_field(out_row, &fc, src, field_start, pos)) {
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
        out_row->field_count = 0;
        *consumed = 0;
        return HP_OK;
    }

    if (field_start < len || fc > 0) {
        if (!avx2_emit_field(out_row, &fc, src, field_start, len)) {
            return HP_ERR_BUFFER_OVERFLOW;
        }
    }
    out_row->field_count = fc;
    *consumed = len;
    return HP_OK;
}

/* ---- Chunk planner bitmaps ------------------------------------------------
 *
 * AVX2 twin of hp_csv_plan_masks_scalar: newline and quote bitmaps for one
 * chunk of up to 64 bytes. movemask produces one status bit per byte, so a
 * 32-byte compare shifts into the correct half of the 64-bit bitmap. The
 * scalar tail handles the trailing < 32 bytes of a chunk. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
void hp_csv_plan_masks_avx2(const uint8_t *p, size_t chunk_len,
                            uint64_t *nl, uint64_t *qm) {
    uint64_t n = 0, q = 0;
    size_t i = 0;
    const __m256i v_lf    = _mm256_set1_epi8('\n');
    const __m256i v_quote = _mm256_set1_epi8('"');

    while (i + 32 <= chunk_len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        n |= (uint64_t)(uint32_t)_mm256_movemask_epi8(
                 _mm256_cmpeq_epi8(chunk, v_lf)) << i;
        q |= (uint64_t)(uint32_t)_mm256_movemask_epi8(
                 _mm256_cmpeq_epi8(chunk, v_quote)) << i;
        i += 32;
    }
    for (; i < chunk_len; i++) {
        uint64_t bit = 1ull << i;
        if (p[i] == '\n') {
            n |= bit;
        } else if (p[i] == '"') {
            q |= bit;
        }
    }
    *nl = n;
    *qm = q;
}
