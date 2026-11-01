#include "hyperpipe/hp_csv_parser.h"
#include <immintrin.h>

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
