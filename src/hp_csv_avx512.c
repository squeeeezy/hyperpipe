#include "hyperpipe/hp_csv_parser.h"
#include <immintrin.h>

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

    size_t pos = 0;
    if (__builtin_expect(src[0] == '\r' || src[0] == '\n', 0)) {
        while (pos < len && (src[pos] == '\r' || src[pos] == '\n')) pos++;
        if (pos >= len) {
            *consumed = pos;
            out_row->field_count = 0;
            return HP_OK;
        }
    }

    const uint8_t *p = src + pos;
    size_t rem = len - pos;
    if (rem < 64) {
        hp_error_t err = hp_csv_parse_row_scalar(parser, p, rem, consumed, out_row, is_eof);
        *consumed += pos;
        return err;
    }

    const uint8_t delim = parser->delim;
    const __m512i v_delim = _mm512_set1_epi8((char)delim);
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_lf    = _mm512_set1_epi8('\n');
    const __m512i v_cr    = _mm512_set1_epi8('\r');

    __m512i chunk0 = _mm512_loadu_si512((const void *)p);
    uint64_t q0 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk0, v_quote);
    if (q0 != 0) {
        hp_error_t err = hp_csv_parse_row_scalar(parser, p, rem, consumed, out_row, is_eof);
        *consumed += pos;
        return err;
    }

    uint64_t d0 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk0, v_delim);
    uint64_t lf0 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk0, v_lf);
    uint64_t cr0 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk0, v_cr);
    uint64_t end0 = lf0 | cr0;

    size_t fc = 0;
    size_t field_start = 0;

    if (end0 != 0) {
        uint32_t end_idx = (uint32_t)__builtin_ctzll(end0);
        uint64_t delims = d0 & ((end_idx < 64) ? ((1ULL << end_idx) - 1) : ~0ULL);

        while (delims) {
            uint32_t d_idx = (uint32_t)__builtin_ctzll(delims);
            out_row->fields[fc].ptr = (const char *)(p + field_start);
            out_row->fields[fc].len = d_idx - field_start;
            out_row->fields[fc].has_escapes = false;
            fc++;
            field_start = d_idx + 1;
            delims &= delims - 1;
        }

        out_row->fields[fc].ptr = (const char *)(p + field_start);
        out_row->fields[fc].len = end_idx - field_start;
        out_row->fields[fc].has_escapes = false;
        fc++;
        out_row->field_count = fc;

        if (p[end_idx] == '\r' && end_idx + 1 < rem && p[end_idx + 1] == '\n') {
            *consumed = pos + end_idx + 2;
        } else {
            *consumed = pos + end_idx + 1;
        }
        return HP_OK;
    }

    /* Pop all delimiters in chunk 0 */
    uint64_t delims = d0;
    while (delims) {
        uint32_t d_idx = (uint32_t)__builtin_ctzll(delims);
        out_row->fields[fc].ptr = (const char *)(p + field_start);
        out_row->fields[fc].len = d_idx - field_start;
        out_row->fields[fc].has_escapes = false;
        fc++;
        field_start = d_idx + 1;
        delims &= delims - 1;
    }

    /* Process chunk 1 (bytes 64..127) */
    if (rem >= 128) {
        __m512i chunk1 = _mm512_loadu_si512((const void *)(p + 64));
        uint64_t q1 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk1, v_quote);
        if (q1 == 0) {
            uint64_t d1 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk1, v_delim);
            uint64_t lf1 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk1, v_lf);
            uint64_t cr1 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk1, v_cr);
            uint64_t end1 = lf1 | cr1;

            if (end1 != 0) {
                uint32_t rel_end = (uint32_t)__builtin_ctzll(end1);
                uint32_t end_idx = 64 + rel_end;
                uint64_t delims1 = d1 & ((rel_end < 64) ? ((1ULL << rel_end) - 1) : ~0ULL);

                while (delims1) {
                    uint32_t d_idx = 64 + (uint32_t)__builtin_ctzll(delims1);
                    out_row->fields[fc].ptr = (const char *)(p + field_start);
                    out_row->fields[fc].len = d_idx - field_start;
                    out_row->fields[fc].has_escapes = false;
                    fc++;
                    field_start = d_idx + 1;
                    delims1 &= delims1 - 1;
                }

                out_row->fields[fc].ptr = (const char *)(p + field_start);
                out_row->fields[fc].len = end_idx - field_start;
                out_row->fields[fc].has_escapes = false;
                fc++;
                out_row->field_count = fc;

                if (p[end_idx] == '\r' && end_idx + 1 < rem && p[end_idx + 1] == '\n') {
                    *consumed = pos + end_idx + 2;
                } else {
                    *consumed = pos + end_idx + 1;
                }
                return HP_OK;
            }

            /* Pop all delimiters in chunk 1 and try chunk 2 (128..191) */
            uint64_t delims1 = d1;
            while (delims1) {
                uint32_t d_idx = 64 + (uint32_t)__builtin_ctzll(delims1);
                out_row->fields[fc].ptr = (const char *)(p + field_start);
                out_row->fields[fc].len = d_idx - field_start;
                out_row->fields[fc].has_escapes = false;
                fc++;
                field_start = d_idx + 1;
                delims1 &= delims1 - 1;
            }

            if (rem >= 192) {
                __m512i chunk2 = _mm512_loadu_si512((const void *)(p + 128));
                uint64_t q2 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk2, v_quote);
                if (q2 == 0) {
                    uint64_t d2 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk2, v_delim);
                    uint64_t lf2 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk2, v_lf);
                    uint64_t cr2 = (uint64_t)_mm512_cmpeq_epi8_mask(chunk2, v_cr);
                    uint64_t end2 = lf2 | cr2;

                    if (end2 != 0) {
                        uint32_t rel_end2 = (uint32_t)__builtin_ctzll(end2);
                        uint32_t end_idx2 = 128 + rel_end2;
                        uint64_t delims2 = d2 & ((rel_end2 < 64) ? ((1ULL << rel_end2) - 1) : ~0ULL);

                        while (delims2) {
                            uint32_t d_idx = 128 + (uint32_t)__builtin_ctzll(delims2);
                            out_row->fields[fc].ptr = (const char *)(p + field_start);
                            out_row->fields[fc].len = d_idx - field_start;
                            out_row->fields[fc].has_escapes = false;
                            fc++;
                            field_start = d_idx + 1;
                            delims2 &= delims2 - 1;
                        }

                        out_row->fields[fc].ptr = (const char *)(p + field_start);
                        out_row->fields[fc].len = end_idx2 - field_start;
                        out_row->fields[fc].has_escapes = false;
                        fc++;
                        out_row->field_count = fc;

                        if (p[end_idx2] == '\r' && end_idx2 + 1 < rem && p[end_idx2 + 1] == '\n') {
                            *consumed = pos + end_idx2 + 2;
                        } else {
                            *consumed = pos + end_idx2 + 1;
                        }
                        return HP_OK;
                    }
                }
            }
        }
    }

    out_row->field_count = fc;
    hp_error_t err = hp_csv_parse_row_scalar(parser, p, rem, consumed, out_row, is_eof);
    *consumed += pos;
    return err;
}
