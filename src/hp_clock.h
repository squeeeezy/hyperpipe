/* Monotonic clock for throughput statistics. Internal to src/. */

#ifndef HYPERPIPE_SRC_HP_CLOCK_H
#define HYPERPIPE_SRC_HP_CLOCK_H

#if defined(_WIN32)

#include <windows.h>

static inline double hp_clock_seconds(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

#else

#include <time.h>

static inline double hp_clock_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif

#endif /* HYPERPIPE_SRC_HP_CLOCK_H */
