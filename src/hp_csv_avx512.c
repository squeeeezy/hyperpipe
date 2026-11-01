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

    /* Trailing elements with 32-byte or scalar processing */
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
