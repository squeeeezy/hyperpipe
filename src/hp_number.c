#include "hp_number.h"

#if defined(__SIZEOF_INT128__)

static const uint64_t pow10_u64[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull,
    1000000000ull, 10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull,
    100000000000000ull, 1000000000000000ull, 10000000000000000ull, 100000000000000000ull,
    1000000000000000000ull, 10000000000000000000ull,
};

/* Whether m / 2^k >= 10^x, -3 <= x <= 15 */
static bool at_least_pow10(uint64_t m, int k, int x) {
    if (x >= 0) return (hp_uwide_t)m >= (hp_uwide_t)pow10_u64[x] << k;
    return (hp_uwide_t)m * pow10_u64[-x] >= (hp_uwide_t)1 << k;
}

/* Writes v, 1e-3 <= v < 1e15, as %.Pg does for the first P from 15 to 17
 * that reads back, without printf: v is m / 2^k exactly, so its P digits
 * round correctly in 128-bit integers, and whether they read back is a
 * comparison with half the gap to the next double. False when rounding
 * carries into a new digit, where %g may write an exponent. */
static bool format_fixed(char *out, double v, size_t *len) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    const uint64_t m = (bits & ((1ull << 52) - 1)) | (1ull << 52);
    const int k = 1075 - (int)(bits >> 52);     /* 3 to 62 in this range */

    /* 10^x <= v < 10^(x+1), from log10(2^(52-k)) */
    int x = (int)((52 - k) * 0.30102999566398) - 1;
    if (x < -3) x = -3;
    while (x < 14 && at_least_pow10(m, k, x + 1)) x++;

    for (int p = 15; p <= 17; p++) {
        const int s = p - 1 - x;                 /* digits after the point */
        const hp_uwide_t exact = (hp_uwide_t)m * pow10_u64[s];   /* v * 10^s * 2^k */
        uint64_t q = (uint64_t)(exact >> k);
        const hp_uwide_t r = exact - ((hp_uwide_t)q << k), half = (hp_uwide_t)1 << (k - 1);
        if (r > half || (r == half && (q & 1))) q++;
        if (q >= pow10_u64[p]) return false;

        /* Reads back when |q / 10^s - v| is within half the gap above v,
         * or a quarter of it below a power of two, where the gap below is
         * half as wide; a tie goes to the even m */
        const hp_uwide_t a = (hp_uwide_t)q << k;
        const hp_uwide_t diff = a > exact ? a - exact : exact - a;
        const hp_uwide_t twice = (m == (1ull << 52) && a < exact) ? 4 * diff : 2 * diff;
        if (twice < pow10_u64[s] || (twice == pow10_u64[s] && (m & 1) == 0)) {
            *len = hp_format_decimal(out, (hp_wide_t)q, s);
            return true;
        }
    }
    return false;
}

#endif

size_t hp_format_double(char *out, double v) {
#if defined(__SIZEOF_INT128__)
    const bool neg = v < 0;
    const double a = neg ? -v : v;
    size_t n = 0;
    if (a >= 1e-3 && a < 1e15 && format_fixed(out + neg, a, &n)) {
        if (neg) out[0] = '-';
        return n + neg;
    }
#endif
    int printed = 0;
    for (int prec = 15; prec <= 17; prec++) {
        printed = snprintf(out, HP_NUMBER_TEXT_MAX, "%.*g", prec, v);
        if (strtod(out, NULL) == v) break;
    }
    return (size_t)printed;
}
