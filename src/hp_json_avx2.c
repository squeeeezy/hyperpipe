#include "hyperpipe/hp_json_ser.h"
#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_json_scan_escapes_avx2(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m256i v_quote = _mm256_set1_epi8('"');
    const __m256i v_slash = _mm256_set1_epi8('\\');
    const __m256i v_31    = _mm256_set1_epi8(31);
    const __m256i v_zero  = _mm256_setzero_si256();

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        __m256i is_quote = _mm256_cmpeq_epi8(chunk, v_quote);
        __m256i is_slash = _mm256_cmpeq_epi8(chunk, v_slash);
        __m256i under_32 = _mm256_cmpeq_epi8(_mm256_subs_epu8(chunk, v_31), v_zero);
        __m256i escapes  = _mm256_or_si256(_mm256_or_si256(is_quote, is_slash), under_32);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(escapes);

        if (mask != 0) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }

    while (i < len) {
        uint8_t c = p[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            return i;
        }
        i++;
    }
    return len;
}
