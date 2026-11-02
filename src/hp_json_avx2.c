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

/* ---- NDJSON input span scanners (AVX2 twins of the scalar references) ----
 * Each returns the index of the first byte with the given property, or len.
 * The results are bit-identical to the scalar versions; only the byte
 * classification is vectorized. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_json_scan_str_avx2(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m256i v_quote = _mm256_set1_epi8('"');
    const __m256i v_slash = _mm256_set1_epi8('\\');

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_quote),
                            _mm256_cmpeq_epi8(chunk, v_slash)));
        if (mask != 0) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }

    while (i < len && p[i] != '"' && p[i] != '\\') i++;
    return i;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_json_scan_lit_avx2(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m256i v_comma = _mm256_set1_epi8(',');
    const __m256i v_brace = _mm256_set1_epi8('}');
    const __m256i v_space = _mm256_set1_epi8(' ');
    const __m256i v_tab   = _mm256_set1_epi8('\t');
    const __m256i v_lf    = _mm256_set1_epi8('\n');
    const __m256i v_cr    = _mm256_set1_epi8('\r');

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        __m256i m = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_comma),
                            _mm256_cmpeq_epi8(chunk, v_brace)),
            _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_space),
                                _mm256_cmpeq_epi8(chunk, v_tab)),
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_lf),
                                _mm256_cmpeq_epi8(chunk, v_cr))));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
        if (mask != 0) {
            return i + (size_t)__builtin_ctz(mask);
        }
        i += 32;
    }

    while (i < len) {
        uint8_t c = p[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        i++;
    }
    return i;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_json_scan_ws_avx2(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m256i v_space = _mm256_set1_epi8(' ');
    const __m256i v_tab   = _mm256_set1_epi8('\t');
    const __m256i v_lf    = _mm256_set1_epi8('\n');
    const __m256i v_cr    = _mm256_set1_epi8('\r');

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(p + i));
        /* mask bits are SET where the byte IS whitespace; the first clear
         * bit marks the first non-whitespace byte */
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(
            _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_space),
                                _mm256_cmpeq_epi8(chunk, v_tab)),
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, v_lf),
                                _mm256_cmpeq_epi8(chunk, v_cr))));
        if (mask != 0xFFFFFFFFu) {
            return i + (size_t)__builtin_ctz(~mask);
        }
        i += 32;
    }

    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    return i;
}

/* AVX2 twin of hp_json_emit_body_avx512: 32-byte chunks, movemask-based
 * detection, scalar copy of the clean prefix (AVX2 has no per-byte masked
 * load/store), shared scalar escape expansion. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
size_t hp_json_emit_body_avx2(char *dst, const char *str, size_t len) {
    size_t i = 0;
    size_t w = 0;
    const __m256i v_quote = _mm256_set1_epi8('"');
    const __m256i v_slash = _mm256_set1_epi8('\\');
    const __m256i v_31    = _mm256_set1_epi8(31);
    const __m256i v_zero  = _mm256_setzero_si256();

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(str + i));
        __m256i escapes = _mm256_or_si256(
            _mm256_or_si256(
                _mm256_cmpeq_epi8(chunk, v_quote),
                _mm256_cmpeq_epi8(chunk, v_slash)),
            _mm256_cmpeq_epi8(_mm256_subs_epu8(chunk, v_31), v_zero));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(escapes);

        if (mask == 0) {
            _mm256_storeu_si256((__m256i *)(dst + w), chunk);
            i += 32;
            w += 32;
            continue;
        }

        uint32_t pos = (uint32_t)__builtin_ctz(mask);
        for (uint32_t k = 0; k < pos; k++) {
            dst[w + k] = str[i + k];
        }
        w += pos;
        i += pos;

        w += hp_json_escape_one(dst + w, (uint8_t)str[i]);
        i += 1;
    }

    /* Tail (< 32 bytes): bulk-copy when clean (the common case), expand
     * byte-wise through the shared expander otherwise. */
    const uint8_t *q = (const uint8_t *)(str + i);
    size_t tail = len - i;
    size_t k = 0;
    while (k < tail && q[k] >= 0x20 && q[k] != '"' && q[k] != '\\') k++;
    if (k == tail) {
        memcpy(dst + w, q, tail);
        return w + tail;
    }
    for (size_t r = 0; r < tail; r++) {
        w += hp_json_escape_one(dst + w, q[r]);
    }
    return w;
}

/* Structural fast path: quote and backslash bitmaps for one chunk of up to
 * 64 bytes. Full 32-byte windows load directly; a short tail falls back to
 * the byte loop, which keeps every read inside the caller's buffer without
 * needing padding guarantees (unlike AVX-512, AVX2 has no masked load). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2,bmi2")))
#endif
void hp_json_struct_masks_avx2(const uint8_t *p, size_t chunk_len,
                               uint64_t *qm, uint64_t *bm) {
    uint64_t q = 0, b = 0;
    size_t i = 0;

    if (chunk_len >= 32) {
        __m256i c0 = _mm256_loadu_si256((const __m256i *)p);
        q = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c0, _mm256_set1_epi8('"')));
        b = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c0, _mm256_set1_epi8('\\')));
        i = 32;
    }
    if (chunk_len >= 64) {
        __m256i c1 = _mm256_loadu_si256((const __m256i *)(p + 32));
        q |= (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c1, _mm256_set1_epi8('"'))) << 32;
        b |= (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(c1, _mm256_set1_epi8('\\'))) << 32;
        i = 64;
    }

    for (; i < chunk_len; i++) {
        uint64_t bit = 1ull << i;
        q |= (p[i] == '"') ? bit : 0;
        b |= (p[i] == '\\') ? bit : 0;
    }

    *qm = q;
    *bm = b;
}
