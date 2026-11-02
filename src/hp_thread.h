/* Minimal threads for the parallel pipeline: one portable spelling over
 * Win32 and POSIX threads. Internal to src/; only what hp_parallel.c needs.
 * Win32 uses SRW locks and condition variables (Vista+), which need no
 * destruction, and _beginthreadex so worker threads get CRT state. */

#ifndef HYPERPIPE_SRC_HP_THREAD_H
#define HYPERPIPE_SRC_HP_THREAD_H

#include <stdbool.h>
#include <stdlib.h>

typedef void *(*hp_thread_fn)(void *arg);

#if defined(_WIN32)

#include <windows.h>
#include <process.h>

typedef HANDLE hp_thread_t;
typedef SRWLOCK hp_mutex_t;
typedef CONDITION_VARIABLE hp_cond_t;

typedef struct hp_thread_start_s {
    hp_thread_fn fn;
    void *arg;
} hp_thread_start_t;

static unsigned __stdcall hp_thread_trampoline(void *p) {
    hp_thread_start_t start = *(hp_thread_start_t *)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

static inline bool hp_thread_create(hp_thread_t *t, hp_thread_fn fn, void *arg) {
    hp_thread_start_t *start = (hp_thread_start_t *)malloc(sizeof(*start));
    if (!start) return false;
    start->fn = fn;
    start->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, hp_thread_trampoline, start, 0, NULL);
    if (h == 0) {
        free(start);
        return false;
    }
    *t = (HANDLE)h;
    return true;
}

static inline void hp_thread_join(hp_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void hp_mutex_init(hp_mutex_t *m) { InitializeSRWLock(m); }
static inline void hp_mutex_destroy(hp_mutex_t *m) { (void)m; }
static inline void hp_mutex_lock(hp_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void hp_mutex_unlock(hp_mutex_t *m) { ReleaseSRWLockExclusive(m); }

static inline void hp_cond_init(hp_cond_t *c) { InitializeConditionVariable(c); }
static inline void hp_cond_destroy(hp_cond_t *c) { (void)c; }
static inline void hp_cond_signal(hp_cond_t *c) { WakeConditionVariable(c); }
static inline void hp_cond_wait(hp_cond_t *c, hp_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

static inline int hp_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else

#include <pthread.h>
#include <unistd.h>

typedef pthread_t hp_thread_t;
typedef pthread_mutex_t hp_mutex_t;
typedef pthread_cond_t hp_cond_t;

static inline bool hp_thread_create(hp_thread_t *t, hp_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}

static inline void hp_thread_join(hp_thread_t t) { pthread_join(t, NULL); }

static inline void hp_mutex_init(hp_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void hp_mutex_destroy(hp_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void hp_mutex_lock(hp_mutex_t *m) { pthread_mutex_lock(m); }
static inline void hp_mutex_unlock(hp_mutex_t *m) { pthread_mutex_unlock(m); }

static inline void hp_cond_init(hp_cond_t *c) { pthread_cond_init(c, NULL); }
static inline void hp_cond_destroy(hp_cond_t *c) { pthread_cond_destroy(c); }
static inline void hp_cond_signal(hp_cond_t *c) { pthread_cond_signal(c); }
static inline void hp_cond_wait(hp_cond_t *c, hp_mutex_t *m) { pthread_cond_wait(c, m); }

static inline int hp_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

#endif

#endif /* HYPERPIPE_SRC_HP_THREAD_H */
