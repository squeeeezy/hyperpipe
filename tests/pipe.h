/* Test helpers: run the transcoder over an in-memory input and capture its
 * output. Built on tmpfile() rather than fmemopen()/open_memstream(), so
 * the tests run on Windows as well as POSIX. */

#ifndef HYPERPIPE_TESTS_PIPE_H
#define HYPERPIPE_TESTS_PIPE_H

#include "hyperpipe/hp_transcoder.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pipe_result_s {
    hp_error_t err;
    char *out;                      /* NUL-terminated copy of the output */
    size_t len;
    hp_transcoder_stats_t stats;
} pipe_result_t;

/* A temporary file holding `len` bytes of `data`, positioned at the start */
static inline FILE *pipe_input(const void *data, size_t len) {
    FILE *f = tmpfile();
    CHECK(f);
    CHECK(fwrite(data, 1, len, f) == len);
    rewind(f);
    return f;
}

/* Reads everything written to `f` and closes it */
static inline char *pipe_slurp(FILE *f, size_t *len) {
    fflush(f);
    long end = ftell(f);
    CHECK(end >= 0);
    char *data = (char *)malloc((size_t)end + 1);
    CHECK(data);
    rewind(f);
    CHECK(fread(data, 1, (size_t)end, f) == (size_t)end);
    data[end] = '\0';
    fclose(f);
    *len = (size_t)end;
    return data;
}

static inline pipe_result_t pipe_run(const hp_transcoder_config_t *cfg,
                                     const void *input, size_t len) {
    pipe_result_t r;
    memset(&r, 0, sizeof(r));
    FILE *in = pipe_input(input, len);
    FILE *out = tmpfile();
    CHECK(out);

    hp_transcoder_t tc;
    r.err = hp_transcoder_init(&tc, cfg);
    if (r.err == HP_OK) {
        r.err = hp_transcoder_run(&tc, in, out);
        r.stats = tc.stats;
        hp_transcoder_free(&tc);
    }
    fclose(in);
    r.out = pipe_slurp(out, &r.len);
    return r;
}

static inline pipe_result_t pipe_run_str(const hp_transcoder_config_t *cfg, const char *input) {
    return pipe_run(cfg, input, strlen(input));
}

static inline void pipe_free(pipe_result_t *r) {
    free(r->out);
    r->out = NULL;
    r->len = 0;
}

#endif /* HYPERPIPE_TESTS_PIPE_H */
