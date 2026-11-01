#include "hyperpipe/hp_json_ser.h"
#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_json_scan_escapes_avx512(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m512i v_space = _mm512_set1_epi8(0x20);
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_slash = _mm512_set1_epi8('\\');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        __mmask64 m_ctrl  = _mm512_cmplt_epu8_mask(chunk, v_space);
        __mmask64 m_quote = _mm512_cmpeq_epi8_mask(chunk, v_quote);
        __mmask64 m_slash = _mm512_cmpeq_epi8_mask(chunk, v_slash);
        uint64_t mask = (uint64_t)(m_ctrl | m_quote | m_slash);

        if (mask != 0) {
            return i + (size_t)__builtin_ctzll(mask);
        }
        i += 64;
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
