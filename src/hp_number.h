/* Number parsing shared by filters and aggregates, so both agree on what a
 * number is: optional sign, digits with an optional fraction, optional
 * exponent, spanning the whole field. Internal to src/. */

#ifndef HYPERPIPE_SRC_HP_NUMBER_H
#define HYPERPIPE_SRC_HP_NUMBER_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Exact sums are scaled integers. 128 bits hold any 18-digit value at any
 * of the 19 scales with room for 10^20 more additions; without a 128-bit
 * type, sums stay exact while they fit in 64. */
#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 hp_wide_t;
__extension__ typedef unsigned __int128 hp_uwide_t;
#else
typedef int64_t hp_wide_t;
typedef uint64_t hp_uwide_t;
#endif

/* The most bytes hp_format_decimal() and hp_format_double() write */
#define HP_NUMBER_TEXT_MAX 48

/* Parses a signed decimal integer that spans the whole field. Values outside
 * int64 are rejected rather than wrapped, so they simply never match. */
static inline bool hp_parse_int_span(const char *ptr, size_t len, int64_t *out) {
    size_t k = 0;
    bool neg = false;
    if (len > 0 && (ptr[0] == '-' || ptr[0] == '+')) {
        neg = (ptr[0] == '-');
        k = 1;
    }
    const size_t digits = len - k;
    if (digits == 0 || digits > 19) return false;

    uint64_t v = 0;
    for (; k < len; k++) {
        uint8_t d = (uint8_t)(ptr[k] - '0');
        if (d > 9) return false;
        v = v * 10 + (uint64_t)d;
    }
    /* Up to 18 digits always fit; only a 19-digit value can exceed int64.
     * Below 10^19 the uint64 accumulator itself never wraps. */
    const uint64_t limit = neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    if (digits == 19 && v > limit) return false;
    *out = neg ? (int64_t)(0ull - v) : (int64_t)v;
    return true;
}

/* Parses a plain decimal number that spans the whole field: optional sign,
 * digits with an optional fraction, optional exponent. Unlike bare strtod it
 * rejects numeric prefixes ("200abc"), whitespace, hex, inf and nan. */
static inline bool hp_parse_decimal_span(const char *ptr, size_t len, double *out) {
    char buf[64];
    if (len == 0 || len >= sizeof(buf)) return false;

    size_t k = 0;
    size_t digits = 0;
    if (ptr[k] == '-' || ptr[k] == '+') k++;
    while (k < len && isdigit((unsigned char)ptr[k])) { k++; digits++; }
    if (k < len && ptr[k] == '.') {
        k++;
        while (k < len && isdigit((unsigned char)ptr[k])) { k++; digits++; }
    }
    if (digits == 0) return false;

    if (k < len && (ptr[k] == 'e' || ptr[k] == 'E')) {
        k++;
        if (k < len && (ptr[k] == '-' || ptr[k] == '+')) k++;
        size_t exp_digits = 0;
        while (k < len && isdigit((unsigned char)ptr[k])) { k++; exp_digits++; }
        if (exp_digits == 0) return false;
    }
    if (k != len) return false;

    memcpy(buf, ptr, len);
    buf[len] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

#define HP_DEC_MAX_DIGITS 18

/* A number in the grammar above as exactly mant * 10^-scale, 0 <= scale <=
 * 18, when mant fits in int64 ("12.50" is 1250 and 2, "5e3" is 5000 and 0).
 * Returns false for anything else, numbers included: the caller then falls
 * back to hp_parse_decimal_span(). */
static inline bool hp_parse_decimal_exact(const char *ptr, size_t len, int64_t *mant, int *scale) {
    size_t k = 0;
    bool neg = false;
    if (len > 0 && (ptr[0] == '-' || ptr[0] == '+')) {
        neg = (ptr[0] == '-');
        k = 1;
    }

    uint64_t m = 0;
    int sig = 0, frac = 0;
    size_t digits = 0;
    for (; k < len && (uint8_t)(ptr[k] - '0') <= 9; k++, digits++) {
        if (m == 0 && ptr[k] == '0') continue;
        if (++sig > 19) return false;   /* below 10^19: no uint64 wrap */
        m = m * 10 + (uint64_t)(ptr[k] - '0');
    }
    if (k < len && ptr[k] == '.') {
        for (k++; k < len && (uint8_t)(ptr[k] - '0') <= 9; k++, digits++, frac++) {
            if (m == 0 && ptr[k] == '0') continue;
            if (++sig > 19) return false;
            m = m * 10 + (uint64_t)(ptr[k] - '0');
        }
    }
    if (digits == 0) return false;

    int exp = 0;
    if (k < len && (ptr[k] == 'e' || ptr[k] == 'E')) {
        bool exp_neg = false;
        k++;
        if (k < len && (ptr[k] == '-' || ptr[k] == '+')) exp_neg = (ptr[k++] == '-');
        size_t exp_digits = 0;
        for (; k < len && (uint8_t)(ptr[k] - '0') <= 9; k++, exp_digits++) {
            if (exp < 1000) exp = exp * 10 + (ptr[k] - '0');
        }
        if (exp_digits == 0) return false;
        if (exp_neg) exp = -exp;
    }
    if (k != len) return false;

    int s = frac - exp;
    if (m == 0) s = 0;
    while (s < 0) {
        if (m > UINT64_MAX / 10) return false;
        m *= 10;
        s++;
    }
    if (s > HP_DEC_MAX_DIGITS || m > (neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull)) return false;

    *mant = neg ? -(int64_t)m : (int64_t)m;
    *scale = s;
    return true;
}

/* v * 10^-scale as decimal text without trailing zeros: "-12.5", "0.05" */
static inline size_t hp_format_decimal(char *out, hp_wide_t v, int scale) {
    char buf[48];
    char *d = buf;             /* the digits, least significant first */
    size_t n = 0, w = 0;
    hp_uwide_t u = v < 0 ? (hp_uwide_t)0 - (hp_uwide_t)v : (hp_uwide_t)v;
    if (u == 0) scale = 0;
    while (u > UINT64_MAX) {   /* wider than 64 bits: rare, and slow */
        d[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    for (uint64_t low = (uint64_t)u; n == 0 || low; low /= 10) {
        d[n++] = (char)('0' + (int)(low % 10));
    }

    for (; scale > 0 && d[0] == '0'; scale--) {
        d++;
        n--;
    }
    while (n <= (size_t)scale) d[n++] = '0';

    if (v < 0) out[w++] = '-';
    for (size_t i = n; i-- > 0;) {
        out[w++] = d[i];
        if (i == (size_t)scale && scale > 0) out[w++] = '.';
    }
    return w;
}

/* The shortest text that reads back as the same double: what %.15g, %.16g
 * or %.17g writes, the first that reads back (src/hp_number.c) */
size_t hp_format_double(char *out, double v);

#endif /* HYPERPIPE_SRC_HP_NUMBER_H */
