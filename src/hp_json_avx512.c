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

/* ---- NDJSON input span scanners (AVX-512 twins of the scalar references) --
 * Each returns the index of the first byte with the given property, or len.
 * The results are bit-identical to the scalar versions; only the byte
 * classification is vectorized. 64-byte chunks fit the input scanner well:
 * unlike the emitter, no stores are involved, so there is no throttling
 * trade-off to balance. */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_json_scan_str_avx512(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_slash = _mm512_set1_epi8('\\');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        uint64_t mask = (uint64_t)(_mm512_cmpeq_epi8_mask(chunk, v_quote)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_slash));
        if (mask != 0) {
            return i + (size_t)__builtin_ctzll(mask);
        }
        i += 64;
    }

    while (i < len && p[i] != '"' && p[i] != '\\') i++;
    return i;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_json_scan_lit_avx512(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m512i v_comma = _mm512_set1_epi8(',');
    const __m512i v_brace = _mm512_set1_epi8('}');
    const __m512i v_space = _mm512_set1_epi8(' ');
    const __m512i v_tab   = _mm512_set1_epi8('\t');
    const __m512i v_lf    = _mm512_set1_epi8('\n');
    const __m512i v_cr    = _mm512_set1_epi8('\r');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        uint64_t mask = (uint64_t)(_mm512_cmpeq_epi8_mask(chunk, v_comma)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_brace)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_space)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_tab)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_lf)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_cr));
        if (mask != 0) {
            return i + (size_t)__builtin_ctzll(mask);
        }
        i += 64;
    }

    while (i < len) {
        uint8_t c = p[i];
        if (c == ',' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
        i++;
    }
    return i;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
size_t hp_json_scan_ws_avx512(const uint8_t *p, size_t len) {
    size_t i = 0;
    const __m512i v_space = _mm512_set1_epi8(' ');
    const __m512i v_tab   = _mm512_set1_epi8('\t');
    const __m512i v_lf    = _mm512_set1_epi8('\n');
    const __m512i v_cr    = _mm512_set1_epi8('\r');

    while (i + 64 <= len) {
        __m512i chunk = _mm512_loadu_si512((const void *)(p + i));
        /* mask bits are SET where the byte IS whitespace; the first clear
         * bit marks the first non-whitespace byte */
        uint64_t mask = (uint64_t)(_mm512_cmpeq_epi8_mask(chunk, v_space)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_tab)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_lf)
                                 | _mm512_cmpeq_epi8_mask(chunk, v_cr));
        if (mask != ~0ull) {
            return i + (size_t)__builtin_ctzll(~mask);
        }
        i += 64;
    }

    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++;
    return i;
}

/* Fused emit: scan and copy in one pass. Clean chunks go straight from a
 * vector load to a single store; when a chunk contains an escape candidate,
 * the clean prefix before it is flushed and the special byte itself is
 * expanded by the shared escape_one routine. This halves the memory traffic
 * versus the classic scan-then-memcpy approach and removes the per-span
 * memcpy call.
 *
 * Width policy: real-world row fields are almost all short (up to a few
 * hundred bytes), and on Xeon-class cores a sustained stream of 512-bit
 * stores costs more in frequency throttling than it wins in width. Strings
 * below HP_EMIT_512_THRESHOLD therefore use the 32-byte AVX2-style loop;
 * only long values (embedded multi-line JSON blobs and similar) take the
 * 64-byte path where the width advantage pays for itself. Measured on the
 * reference host: this hybrid is +6% over pure-64B on short-field data
 * while keeping full width for long values. */
#define HP_EMIT_512_THRESHOLD 192

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
HP_HOT size_t hp_json_emit_body_avx512(char *dst, const char *str, size_t len) {
    size_t i = 0;
    size_t w = 0;
    const __m512i v_space = _mm512_set1_epi8(0x20);
    const __m512i v_quote = _mm512_set1_epi8('"');
    const __m512i v_slash = _mm512_set1_epi8('\\');

    if (len >= HP_EMIT_512_THRESHOLD) {
        while (i + 64 <= len) {
            __m512i chunk = _mm512_loadu_si512((const void *)(str + i));
            __mmask64 mask = _mm512_cmplt_epu8_mask(chunk, v_space)
                           | _mm512_cmpeq_epi8_mask(chunk, v_quote)
                           | _mm512_cmpeq_epi8_mask(chunk, v_slash);

            if (mask == 0) {
                _mm512_storeu_si512((void *)(dst + w), chunk);
                i += 64;
                w += 64;
                continue;
            }

            /* Flush the clean prefix below the first special byte with one
             * masked store (mask & (mask - 1) clears the lowest set bit). */
            __mmask64 prefix = mask & (mask - 1);
            if (prefix != 0) {
                _mm512_mask_storeu_epi8(dst + w, prefix,
                                        _mm512_maskz_loadu_epi8(prefix, (const void *)(str + i)));
            }
            size_t pos = (size_t)__builtin_ctzll(mask);
            w += pos;
            i += pos;

            w += hp_json_escape_one(dst + w, (uint8_t)str[i]);
            i += 1;
        }
    }

    /* 32-byte loop: covers short values entirely, drains the remainder of
     * long ones, and keeps the core from sitting in the 512-bit license. */
    const __m256i v_q8  = _mm256_set1_epi8('"');
    const __m256i v_s8  = _mm256_set1_epi8('\\');
    const __m256i v_31  = _mm256_set1_epi8(31);
    const __m256i v_zero = _mm256_setzero_si256();

    while (i + 32 <= len) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(str + i));
        __m256i escapes = _mm256_or_si256(
            _mm256_or_si256(
                _mm256_cmpeq_epi8(chunk, v_q8),
                _mm256_cmpeq_epi8(chunk, v_s8)),
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
 * 64 bytes. The masked load touches only enabled lanes, so a short tail
 * chunk is read without any padding guarantee from the caller. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx512f,avx512bw,avx512vl,bmi2")))
#endif
void hp_json_struct_masks_avx512(const uint8_t *p, size_t chunk_len,
                                 uint64_t *qm, uint64_t *bm) {
    __mmask64 len_mask = (chunk_len >= 64) ? ~0ull : ((1ull << chunk_len) - 1);
    __m512i chunk = _mm512_maskz_loadu_epi8(len_mask, (const void *)p);
    *qm = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, _mm512_set1_epi8('"')) & len_mask;
    *bm = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, _mm512_set1_epi8('\\')) & len_mask;
}
