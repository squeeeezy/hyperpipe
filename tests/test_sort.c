/* --sort: the order of numbers, text and empty values, stability, top-N,
 * runs spilled to disk, grouped output, and the same bytes from every
 * job count. */

#include "hyperpipe/hp_parallel.h"
#include "hyperpipe/hp_sort.h"
#include "pipe.h"

#ifndef HP_TEST_TMPDIR
#define HP_TEST_TMPDIR "build/"
#endif

static bool sorted_is(const char *input, const char *spec, uint64_t head, const char *want) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .sort_spec = spec, .limit_rows = head,
    };
    pipe_result_t r = pipe_run_str(&cfg, input);
    bool ok = r.err == HP_OK && strcmp(r.out, want) == 0;
    if (!ok) fprintf(stderr, "# got:\n%s# want:\n%s", r.out, want);
    pipe_free(&r);
    return ok;
}

static void test_value_order(void) {
    /* Numbers by value, then text by bytes, then empty values; 10 and 1e1
     * are equal and keep their input order */
    const char *in = "v,i\n10,1\nb,2\n9.5,3\n,4\n-3,5\na,6\n1e1,7\n";
    CHECK(sorted_is(in, "v", 0, "v,i\n-3,5\n9.5,3\n10,1\n1e1,7\na,6\nb,2\n,4\n"));
    /* Descending reverses each kind; empty values stay last */
    CHECK(sorted_is(in, "-v", 0, "v,i\n10,1\n1e1,7\n9.5,3\n-3,5\nb,2\na,6\n,4\n"));
    /* Negative zero sorts with zero; a prefix before longer text */
    CHECK(sorted_is("v,i\n0,1\n-0,2\nab,3\na,4\n", "v", 0, "v,i\n0,1\n-0,2\na,4\nab,3\n"));
}

static void test_stable_and_multi_column(void) {
    const char *in = "k,v,id\nb,2,1\na,2,2\nb,1,3\na,2,4\n";
    CHECK(sorted_is(in, "v", 0, "k,v,id\nb,1,3\nb,2,1\na,2,2\na,2,4\n"));
    CHECK(sorted_is(in, "k,-v", 0, "k,v,id\na,2,2\na,2,4\nb,2,1\nb,1,3\n"));
    /* By position, and by a column that is not in the output */
    CHECK(sorted_is(in, "2", 0, "k,v,id\nb,2,1\na,2,2\nb,1,3\na,2,4\n"));
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .sort_spec = "-id", .select_spec = "k",
    };
    pipe_result_t r = pipe_run_str(&cfg, in);
    CHECK(r.err == HP_OK && strcmp(r.out, "k\na\nb\na\nb\n") == 0);
    pipe_free(&r);
}

static void test_top_n(void) {
    const char *in = "v\n5\n3\n9\n1\n9\n7\n";
    CHECK(sorted_is(in, "-v", 3, "v\n9\n9\n7\n"));
    CHECK(sorted_is(in, "v", 1, "v\n1\n"));
    CHECK(sorted_is(in, "v", 100, "v\n1\n3\n5\n7\n9\n9\n"));
}

static void test_ndjson_and_quoting(void) {
    hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_NDJSON, .sort_spec = "-n" };
    pipe_result_t r = pipe_run_str(&cfg, "{\"n\":1,\"s\":\"a,b\"}\n{\"n\":3,\"s\":\"x\\\"y\"}\n{\"s\":\"none\"}\n");
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "n,s\n3,\"x\"\"y\"\n1,\"a,b\"\n,none\n") == 0);
    pipe_free(&r);

    cfg.out_format = HP_FORMAT_NDJSON;
    cfg.sort_spec = "s";
    r = pipe_run_str(&cfg, "{\"s\":\"b\",\"t\":[1]}\n{\"s\":\"a\"}\n");
    CHECK(r.err == HP_OK && strcmp(r.out, "{\"s\":\"a\"}\n{\"s\":\"b\",\"t\":[1]}\n") == 0);
    pipe_free(&r);
}

static void test_groups(void) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .group_by_spec = "k", .agg_spec = "count, sum(v)", .sort_spec = "-sum(v),k", .limit_rows = 2,
    };
    pipe_result_t r = pipe_run_str(&cfg, "k,v\na,1\nb,5\nc,5\na,1\nd,2\n");
    CHECK(r.err == HP_OK && strcmp(r.out, "k,count,sum(v)\nb,1,5\nc,1,5\n") == 0);
    pipe_free(&r);
}

static void test_errors(void) {
    hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_CSV, .sort_spec = "zz" };
    pipe_result_t r = pipe_run_str(&cfg, "a\n1\n");
    CHECK(r.err == HP_ERR_COLUMN_NOT_FOUND && strcmp(r.stats.error_column, "zz") == 0);
    pipe_free(&r);

    static hp_sorter_t s;
    CHECK(hp_sorter_init(&s, "", 0, 0) == HP_ERR_INVALID_ARG);
    CHECK(hp_sorter_init(&s, "a,,b", 0, 0) == HP_ERR_INVALID_ARG);
    CHECK(hp_sorter_init(&s, " -a , b ", 0, 0) == HP_OK && s.key_count == 2 && s.keys[0].desc);
    hp_sorter_free(&s);
}

/* One pass, runs spilled to disk and every job count give the same bytes */
static void test_spill_and_parallel_agree(void) {
    const char *path = HP_TEST_TMPDIR "test_sort_input.csv";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs("id,k,v\n", f);
    uint64_t x = 0x2545F4914F6CDD1Dull;
    for (int i = 0; i < 200000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        fprintf(f, "%d,k%u,%s\n", i, (unsigned)(x % 500), (x & 15) ? "" : "-");
        if (x & 1) fprintf(f, "%d,\"q,%u\",%d\n", i, (unsigned)(x % 7), (int)(x >> 50) - 8000);
    }
    fclose(f);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV, .sort_spec = "-v,k",
    };
    char *want = NULL;
    size_t want_len = 0;
    const size_t budgets[] = { 0, 64 << 10 };
    for (size_t b = 0; b < 2; b++) {
        cfg.sort_memory = budgets[b];
        FILE *in = fopen(path, "rb");
        FILE *out = tmpfile();
        hp_transcoder_t tc;
        CHECK(in && out && hp_transcoder_init(&tc, &cfg) == HP_OK);
        CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
        hp_transcoder_free(&tc);
        fclose(in);
        size_t len = 0;
        char *got = pipe_slurp(out, &len);
        if (!want) {
            want = got;
            want_len = len;
        } else {
            CHECK(len == want_len && memcmp(got, want, len) == 0);
            free(got);
        }
    }
    for (int jobs = 2; jobs <= 8; jobs += 3) {
        for (size_t b = 0; b < 2; b++) {
            cfg.sort_memory = budgets[b];
            hp_transcoder_stats_t stats;
            FILE *out = tmpfile();
            CHECK(hp_parallel_run(&cfg, path, out, jobs, &stats) == HP_OK);
            size_t len = 0;
            char *got = pipe_slurp(out, &len);
            if (len != want_len || memcmp(got, want, len) != 0) {
                fprintf(stderr, "# jobs %d, budget %zu differ\n", jobs, budgets[b]);
            }
            CHECK(len == want_len && memcmp(got, want, len) == 0);
            free(got);
        }
    }
    free(want);
    remove(path);
}

int main(void) {
    TAP_RUN(test_value_order);
    TAP_RUN(test_stable_and_multi_column);
    TAP_RUN(test_top_n);
    TAP_RUN(test_ndjson_and_quoting);
    TAP_RUN(test_groups);
    TAP_RUN(test_errors);
    TAP_RUN(test_spill_and_parallel_agree);
    return tap_done();
}
