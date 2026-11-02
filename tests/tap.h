/* Minimal TAP producer for the test binaries (https://testanything.org).
 *
 * Each test function is one TAP test point. CHECK() aborts on failure
 * instead of returning, so a failing binary prints the failed condition on
 * stderr and never reaches its plan line; TAP harnesses report that as a
 * failure. Unlike assert(), CHECK() is not compiled out by NDEBUG. */

#ifndef HYPERPIPE_TESTS_TAP_H
#define HYPERPIPE_TESTS_TAP_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n",                    \
                    __FILE__, __LINE__, #cond);                             \
            abort();                                                        \
        }                                                                   \
    } while (0)

static int tap_points;

static inline void tap_run(const char *name, void (*test)(void)) {
    test();
    printf("ok %d - %s\n", ++tap_points, name);
    fflush(stdout);
}

#define TAP_RUN(test) tap_run(#test, test)

static inline int tap_done(void) {
    printf("1..%d\n", tap_points);
    return 0;
}

static inline int tap_skip_all(const char *reason) {
    printf("1..0 # SKIP %s\n", reason);
    return 0;
}

#endif /* HYPERPIPE_TESTS_TAP_H */
