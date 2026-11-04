/* Number text: hp_format_double() writes what the printf loop it replaces
 * wrote, for every kind of double. */

#include "../src/hp_number.h"
#include "tap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The definition: the first of %.15g, %.16g and %.17g that reads back */
static size_t reference(char *out, double v) {
    int n = 0;
    for (int prec = 15; prec <= 17; prec++) {
        n = snprintf(out, HP_NUMBER_TEXT_MAX, "%.*g", prec, v);
        if (strtod(out, NULL) == v) break;
    }
    return (size_t)n;
}

static bool same_text(double v) {
    char want[HP_NUMBER_TEXT_MAX], got[HP_NUMBER_TEXT_MAX];
    size_t a = reference(want, v), b = hp_format_double(got, v);
    bool ok = a == b && memcmp(want, got, a) == 0;
    if (!ok) fprintf(stderr, "# %.17g: want %.*s, got %.*s\n", v, (int)a, want, (int)b, got);
    return ok;
}

static uint64_t next(uint64_t *x) {
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    return *x;
}

static void test_known_values(void) {
    const double values[] = {
        0.0, -0.0, 0.1, 0.2, 0.3, 0.1 + 0.2, 1.0 / 3, 2.0 / 3, 6245.0 / 1024, 1e-3, 1e-4,
        0.00099999999999999999, 999999999999999.9, 1e15, 1e15 - 1, 123456789012345.6,
        0.5, 1.5, 2.5, 4503599627370496.0, 9007199254740993.0, 1e300, 5e-324, -1.0 / 7,
        1024, 3.0e-3, 0.001953125, 299792458, 1e22, 1e23, 4.35, 0.07, 1.0000000000000002,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) CHECK(same_text(values[i]));

    /* Powers of two, where the gap below is half the gap above */
    for (int e = -12; e <= 52; e++) {
        double p = ldexp(1.0, e);
        CHECK(same_text(p));
        CHECK(same_text(nextafter(p, 0)));
        CHECK(same_text(nextafter(p, INFINITY)));
    }
}

static void test_random_values(void) {
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 400000; i++) {
        /* Quotients, as division gives them, and arbitrary bits in and
         * around the range the integer path covers */
        double a = (double)(int64_t)(next(&x) % 2000001) - 1000000;
        double b = (double)(next(&x) % 4096 + 1);
        CHECK(same_text(a / b));
        double d = ldexp((double)(next(&x) >> 11), (int)(next(&x) % 80) - 70);
        CHECK(same_text(d));
        CHECK(same_text(-d));
    }
}

int main(void) {
    TAP_RUN(test_known_values);
    TAP_RUN(test_random_values);
    return tap_done();
}
