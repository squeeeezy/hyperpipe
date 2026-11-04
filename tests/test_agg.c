/* --group-by and --agg: what each function returns, exact decimals, and the
 * same result whether a table is built in one pass or merged from parts. */

#include "hyperpipe/hp_agg.h"
#include "hyperpipe/hp_parallel.h"
#include "pipe.h"

#ifndef HP_TEST_TMPDIR
#define HP_TEST_TMPDIR "build/"
#endif

static pipe_result_t group(const char *input, hp_format_t from, const char *by, const char *fns,
                           const char *where) {
    hp_transcoder_config_t cfg = {
        .in_format = from,
        .out_format = HP_FORMAT_CSV,
        .group_by_spec = by,
        .agg_spec = fns,
        .where_spec = where,
    };
    return pipe_run_str(&cfg, input);
}

static bool groups_are(const char *input, const char *by, const char *fns, const char *want) {
    pipe_result_t r = group(input, HP_FORMAT_CSV, by, fns, NULL);
    bool ok = r.err == HP_OK && strcmp(r.out, want) == 0;
    if (!ok) fprintf(stderr, "# got:\n%s# want:\n%s", r.out, want);
    pipe_free(&r);
    return ok;
}

static const char *const logs =
    "host,status,bytes,ms\n"
    "a,200,100,1.5\n"
    "b,404,,2\n"
    "a,200,50,0.1\n"
    "c,500,x,0.2\n"
    "b,200,7,\n";

static void test_functions(void) {
    CHECK(groups_are(logs, "host", "count, sum(bytes), avg(ms), min(ms), max(bytes), count(bytes)",
                     "host,count,sum(bytes),avg(ms),min(ms),max(bytes),count(bytes)\n"
                     "a,2,150,0.8,0.1,100,2\n"
                     "b,2,7,2,2,7,1\n"
                     "c,1,,0.2,0.2,,1\n"));
    /* --group-by alone counts; groups keep the order of their first row */
    CHECK(groups_are(logs, "status", NULL, "status,count\n200,3\n404,1\n500,1\n"));
    CHECK(groups_are(logs, "host, status", "count",
                     "host,status,count\na,200,2\nb,404,1\nc,500,1\nb,200,1\n"));
    /* Columns by position */
    CHECK(groups_are(logs, "0", "sum(2)", "host,sum(bytes)\na,150\nb,7\nc,\n"));
}

static void test_without_groups(void) {
    CHECK(groups_are(logs, NULL, "count, sum(ms), avg(ms)", "count,sum(ms),avg(ms)\n5,3.8,0.95\n"));

    /* No rows still give one row, as in SQL */
    pipe_result_t r = group(logs, HP_FORMAT_CSV, NULL, "count, sum(bytes)", "status == 999");
    CHECK(r.err == HP_OK && strcmp(r.out, "count,sum(bytes)\n0,\n") == 0);
    pipe_free(&r);
    r = group("", HP_FORMAT_CSV, NULL, "count", NULL);
    CHECK(r.err == HP_OK && strcmp(r.out, "count\n0\n") == 0);
    pipe_free(&r);

    /* ... but no groups give no rows */
    r = group(logs, HP_FORMAT_CSV, "host", "count", "status == 999");
    CHECK(r.err == HP_OK && strcmp(r.out, "host,count\n") == 0);
    pipe_free(&r);
}

static void test_exact_decimals(void) {
    CHECK(groups_are("v\n0.1\n0.2\n", NULL, "sum(v)", "sum(v)\n0.3\n"));
    CHECK(groups_are("v\n1.5\n2.25\n-0.75\n", NULL, "sum(v), min(v), max(v)",
                     "sum(v),min(v),max(v)\n3.00,-0.75,2.25\n"));
    /* Past int64, still exact */
    CHECK(groups_are("v\n9000000000000000000\n9000000000000000000\n", NULL, "sum(v)",
                     "sum(v)\n18000000000000000000\n"));
    /* Exponents are exact too */
    CHECK(groups_are("v\n5e3\n2.5E-1\n", NULL, "sum(v), max(v)", "sum(v),max(v)\n5000.25,5000\n"));
    /* More than 18 significant digits falls back to double */
    CHECK(groups_are("v\n1.2345678901234567890\n1\n", NULL, "sum(v)", "sum(v)\n2.234567890123457\n"));
    /* Values that are not numbers in their whole length are skipped */
    CHECK(groups_are("v\n10\n10ms\n0x10\ninf\n\n", NULL, "sum(v), count(v)", "sum(v),count(v)\n10,4\n"));
    /* avg does not depend on how the sum was split */
    CHECK(groups_are("v\n1\n2\n2\n", NULL, "avg(v)", "avg(v)\n1.6666666666666667\n"));
}

static void test_keys(void) {
    /* Keys are text: a CSV key with a comma is quoted on output */
    CHECK(groups_are("k,v\n\"a,b\",1\n\"a,b\",2\nc,3\n", "k", "sum(v)", "k,sum(v)\n\"a,b\",3\nc,3\n"));

    /* NDJSON: 1 and "1" are the same text; an absent key is its own group */
    pipe_result_t r = group("{\"k\":1,\"v\":1}\n{\"k\":\"1\",\"v\":2}\n{\"v\":4}\n{\"k\":\"\",\"v\":8}\n",
                            HP_FORMAT_NDJSON, "k", "sum(v)", NULL);
    CHECK(r.err == HP_OK && strcmp(r.out, "k,sum(v)\n1,3\n,4\n\"\",8\n") == 0);
    pipe_free(&r);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_NDJSON,
        .group_by_spec = "host", .agg_spec = "count, avg(ms)",
    };
    r = pipe_run_str(&cfg, logs);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "{\"host\":\"a\",\"count\":2,\"avg(ms)\":0.8}\n"
                        "{\"host\":\"b\",\"count\":2,\"avg(ms)\":2}\n"
                        "{\"host\":\"c\",\"count\":1,\"avg(ms)\":0.2}\n") == 0);
    pipe_free(&r);
}

static void test_head_and_count(void) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .group_by_spec = "host", .limit_rows = 2,
    };
    pipe_result_t r = pipe_run_str(&cfg, logs);
    CHECK(r.err == HP_OK && strcmp(r.out, "host,count\na,2\nb,2\n") == 0);
    pipe_free(&r);

    /* --count counts the groups */
    cfg.limit_rows = 0;
    cfg.count_only = true;
    r = pipe_run_str(&cfg, logs);
    CHECK(r.err == HP_OK && r.len == 0 && r.stats.rows_emitted == 3);
    pipe_free(&r);
}

static void test_errors(void) {
    pipe_result_t r = group(logs, HP_FORMAT_CSV, "host", "sum(nope)", NULL);
    CHECK(r.err == HP_ERR_COLUMN_NOT_FOUND && strcmp(r.stats.error_column, "nope") == 0);
    pipe_free(&r);

    static const struct {
        const char *fns;
        size_t at;
    } bad[] = {
        { "mode(x)", 0 }, { "sum", 3 }, { "sum(x", 5 }, { "sum()", 4 },
        { "count sum(x)", 6 }, { "count,", 6 },
        { "p101(x)", 0 }, { "median", 6 }, { "count_distinct", 14 }, { "p(x)", 0 },
    };
    static hp_agg_t agg;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        hp_error_t err = hp_agg_init(&agg, NULL, bad[i].fns);
        if (err != HP_ERR_AGG || agg.error_at != bad[i].at) {
            fprintf(stderr, "# '%s': err %d at %zu\n", bad[i].fns, (int)err, agg.error_at);
        }
        CHECK(err == HP_ERR_AGG && agg.error_at == bad[i].at);
    }
}

static void test_distinct_and_quantiles(void) {
    /* Exact quantiles interpolate as numpy and pandas do, to the bit */
    CHECK(groups_are(logs, "host", "count_distinct(status), median(ms), p25(ms)",
                     "host,count_distinct(status),median(ms),p25(ms)\n"
                     "a,1,0.8,0.44999999999999996\n"
                     "b,2,2,2\n"
                     "c,1,0.2,0.2\n"));
    CHECK(groups_are("v\n1\n2\n3\n4\n", NULL, "median(v), p0(v), p100(v), p99.9(v)",
                     "median(v),p0(v),p100(v),p99.9(v)\n2.5,1,4,3.997\n"));
    /* Empty and missing values are not distinct values; text is */
    CHECK(groups_are("k,v\na,x\na,\na,x\na,y\nb,1\nb,1.0\n", "k", "count_distinct(v)",
                     "k,count_distinct(v)\na,2\nb,2\n"));

    /* Past the exact limit the sketch stays within 1% */
    static char doc[1 << 20];
    size_t w = (size_t)snprintf(doc, sizeof(doc), "v\n");
    for (int i = 1; i <= 100000; i++) w += (size_t)snprintf(doc + w, sizeof(doc) - w, "%d\n", i);
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV, .agg_spec = "p50(v), p99(v), p1(v)",
    };
    pipe_result_t r = pipe_run(&cfg, doc, w);
    double p50 = 0, p99 = 0, p1 = 0;
    CHECK(r.err == HP_OK && sscanf(r.out, "p50(v),p99(v),p1(v)\n%lf,%lf,%lf", &p50, &p99, &p1) == 3);
    CHECK(p50 > 50000 * 0.99 && p50 < 50001 * 1.01);
    CHECK(p99 > 99000 * 0.99 && p99 < 99001 * 1.01);
    CHECK(p1 > 1000 * 0.99 && p1 < 1001 * 1.01);
    pipe_free(&r);
}

/* One pass, merged halves and the parallel pipeline give the same bytes */
static void test_merge_matches_one_pass(void) {
    const char *path = HP_TEST_TMPDIR "test_agg_parallel.csv";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs("k,v,w\n", f);
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 300000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        fprintf(f, "k%u,%d.%02u,%s\n", (unsigned)(x % 1000), (int)(x >> 40) % 2000 - 1000,
                (unsigned)(x >> 20) % 100, (x & 7) ? "1e2" : "n/a");
    }
    fclose(f);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .group_by_spec = "k",
        .agg_spec = "count, sum(v), avg(v), min(v), max(v), sum(w), count_distinct(v), median(v), p99(v)",
    };
    FILE *in = fopen(path, "rb");
    FILE *out = tmpfile();
    hp_transcoder_t tc;
    CHECK(in && out && hp_transcoder_init(&tc, &cfg) == HP_OK);
    CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
    hp_transcoder_free(&tc);
    fclose(in);
    size_t one_len = 0;
    char *one = pipe_slurp(out, &one_len);

    hp_transcoder_stats_t stats;
    out = tmpfile();
    CHECK(hp_parallel_run(&cfg, path, out, 8, &stats) == HP_OK);
    size_t par_len = 0;
    char *par = pipe_slurp(out, &par_len);
    remove(path);

    CHECK(stats.rows_emitted == 1000);
    CHECK(one_len == par_len && memcmp(one, par, one_len) == 0);
    free(one);
    free(par);
}

int main(void) {
    TAP_RUN(test_functions);
    TAP_RUN(test_without_groups);
    TAP_RUN(test_exact_decimals);
    TAP_RUN(test_keys);
    TAP_RUN(test_head_and_count);
    TAP_RUN(test_errors);
    TAP_RUN(test_distinct_and_quantiles);
    TAP_RUN(test_merge_matches_one_pass);
    return tap_done();
}
