/* --add and paths into JSON: what expressions compute, where computed and
 * hidden columns go, and the same output from the parallel pipeline. */

#include "hyperpipe/hp_compute.h"
#include "hyperpipe/hp_parallel.h"
#include "pipe.h"

#ifndef HP_TEST_TMPDIR
#define HP_TEST_TMPDIR "build/"
#endif

typedef struct run_s {
    hp_format_t from, to;
    const char *add, *select, *where, *group_by, *agg, *sort;
} run_t;

static bool output_is(const char *input, run_t run, const char *want) {
    hp_transcoder_config_t cfg = {
        .in_format = run.from,
        .out_format = run.to ? run.to : HP_FORMAT_CSV,
        .compute_spec = run.add,
        .select_spec = run.select,
        .where_spec = run.where,
        .group_by_spec = run.group_by,
        .agg_spec = run.agg,
        .sort_spec = run.sort,
    };
    pipe_result_t r = pipe_run_str(&cfg, input);
    bool ok = r.err == HP_OK && strcmp(r.out, want) == 0;
    if (!ok) fprintf(stderr, "# error %d, got:\n%s# want:\n%s", (int)r.err, r.out, want);
    pipe_free(&r);
    return ok;
}

/* One computed column over the one-column CSV input */
static bool computes(const char *input, const char *expr, const char *want) {
    char add[256];
    snprintf(add, sizeof(add), "y = %s", expr);
    return output_is(input, (run_t){ .from = HP_FORMAT_CSV, .add = add, .select = "y" }, want);
}

static void test_arithmetic(void) {
    const char *in = "x\n9.99\n0.1\n-3\nn/a\n";
    /* Exact decimals, as sums are; division in double */
    CHECK(computes(in, "x * 3", "y\n29.97\n0.3\n-9\n\n"));
    CHECK(computes(in, "x + 0.2", "y\n10.19\n0.3\n-2.8\n\n"));
    CHECK(computes(in, "x / 4", "y\n2.4975\n0.025\n-0.75\n\n"));
    CHECK(computes(in, "x % 2", "y\n1.99\n0.1\n-1\n\n"));
    CHECK(computes(in, "-x - -1", "y\n-8.99\n0.9\n4\n\n"));
    CHECK(computes(in, "(x + 1) * 2 - x / 0.5", "y\n2\n2\n2\n\n"));
    CHECK(computes(in, "x / 0", "y\n\n\n\n\n"));
    CHECK(computes("x\n1\n", "1 / 3", "y\n0.3333333333333333\n"));
    CHECK(computes("x\n1\n", "1e3 * 2.5", "y\n2500\n"));
    /* Past 18 digits, double */
    CHECK(computes("x\n9000000000000000000\n", "x * 10", "y\n9e+19\n"));
    CHECK(computes("x\n0.000000001\n", "x * x * x", "y\n1e-27\n"));
}

static void test_number_functions(void) {
    const char *in = "x\n2.5\n-2.5\n1.234\n-7\n";
    CHECK(computes(in, "round(x)", "y\n3\n-3\n1\n-7\n"));
    CHECK(computes(in, "round(x, 2)", "y\n2.5\n-2.5\n1.23\n-7\n"));
    CHECK(computes(in, "round(x * 100, -2)", "y\n300\n-300\n100\n-700\n"));
    CHECK(computes(in, "floor(x)", "y\n2\n-3\n1\n-7\n"));
    CHECK(computes(in, "ceil(x)", "y\n3\n-2\n2\n-7\n"));
    CHECK(computes(in, "abs(x)", "y\n2.5\n2.5\n1.234\n7\n"));
    CHECK(computes("x\n1e300\n", "round(x * 1e10)", "y\n\n"));
}

static void test_text_functions(void) {
    const char *in = "s\n\"  Hi, Ünï  \"\nabc\n";
    CHECK(computes(in, "lower(s)", "y\n\"  hi, ünï  \"\nabc\n"));
    CHECK(computes(in, "upper(trim(s))", "y\n\"HI, ÜNÏ\"\nABC\n"));
    CHECK(computes("s\nМосква Ёж Ωμέγα Łódź\n", "upper(s)", "y\nМОСКВА ЁЖ ΩΜέΓΑ ŁÓDŹ\n"));
    CHECK(computes("s\nМосква Ёж Ωμέγα Łódź\n", "lower(s)", "y\nмосква ёж ωμέγα łódź\n"));
    CHECK(computes(in, "len(s)", "y\n11\n3\n"));
    CHECK(computes(in, "substr(trim(s), 2, 3)", "y\n\"i, \"\nbc\n"));
    CHECK(computes(in, "substr(s, -3)", "y\nï  \nabc\n"));
    CHECK(computes(in, "substr(s, 0, 2)", "y\n \na\n"));
    CHECK(computes(in, "replace(s, ' ', '_')", "y\n\"__Hi,_Ünï__\"\nabc\n"));
    CHECK(computes(in, "split_part(trim(s), ', ', 2)", "y\nÜnï\n\"\"\n"));
    CHECK(computes(in, "concat(len(s), ':', s, 1.50)", "y\n\"11:  Hi, Ünï  1.50\"\n3:abc1.50\n"));
    CHECK(computes("a,b\n,x\n1,y\n", "coalesce(a, b)", "y\nx\n1\n"));
    CHECK(computes("t\n01/Jul/1995:00:00:01 -0400\n2024-03-05T23:59Z\nnope\n", "hour(t)",
                   "y\n1995-07-01 04:00\n2024-03-05 23:00\n\n"));
    CHECK(computes("t\n1700000000\n", "day(t)", "y\n2023-11-14\n"));
}

static const char *const events =
    "{\"id\":1,\"meta\":{\"user\":{\"id\":7,\"name\":\"Ann \\\"A\\\"\"},\"tags\":[\"x\",\"y\"]}}\n"
    "{\"id\":2,\"meta\":{\"user\":{\"id\":8,\"name\":\"bob\"},\"tags\":[]},\"new\":true}\n"
    "{\"id\":3,\"meta\":{\"caf\\u00e9\":[[1,2],{\"k\":null}]}}\n"
    "{\"id\":4,\"meta\":\"{\\\"user\\\":{\\\"id\\\":9}}\"}\n";

static void test_paths(void) {
    const run_t select = { .from = HP_FORMAT_NDJSON, .select =
        "id, meta.user.name, meta.tags[0], meta.tags.1, meta.tags[-1], meta.café[0][-1], meta.café.1, meta.user" };
    CHECK(output_is(events, select,
                    "id,meta.user.name,meta.tags[0],meta.tags.1,meta.tags[-1],meta.café[0][-1],"
                    "meta.café.1,meta.user\n"
                    "1,\"Ann \"\"A\"\"\",x,y,y,,,\"{\"\"id\"\":7,\"\"name\"\":\"\"Ann \\\"\"A\\\"\"\"\"}\"\n"
                    "2,bob,,,,,,\"{\"\"id\"\":8,\"\"name\"\":\"\"bob\"\"}\"\n"
                    "3,,,,,2,\"{\"\"k\"\":null}\",\n"
                    "4,,,,,,,\"{\"\"id\"\":9}\"\n"));

    /* Named in a filter, a path is not written out; keys that appear later
     * still are */
    CHECK(output_is(events, (run_t){ .from = HP_FORMAT_NDJSON, .to = HP_FORMAT_NDJSON,
                                     .where = "meta.user.id >= 8" },
                    "{\"id\":2,\"meta\":{\"user\":{\"id\":8,\"name\":\"bob\"},\"tags\":[]},\"new\":true}\n"
                    "{\"id\":4,\"meta\":\"{\\\"user\\\":{\\\"id\\\":9}}\"}\n"));
    CHECK(output_is(events, (run_t){ .from = HP_FORMAT_NDJSON, .where = "meta.tags[0] == x",
                                     .select = "id" }, "id\n1\n"));
    CHECK(output_is(events, (run_t){ .from = HP_FORMAT_NDJSON, .group_by = "meta.user.id",
                                     .agg = "count, sum(meta.user.id)" },
                    "meta.user.id,count,sum(meta.user.id)\n7,1,7\n8,1,8\n,1,\n9,1,9\n"));
    CHECK(output_is(events, (run_t){ .from = HP_FORMAT_NDJSON, .sort = "-meta.user.id",
                                     .select = "id" }, "id\n4\n2\n1\n3\n"));
    /* A path from a column that is not JSON finds nothing; null is no
     * value either, so count(col) skips it as SQL does */
    CHECK(output_is("a\nx\n", (run_t){ .from = HP_FORMAT_CSV, .select = "a.b" }, "a.b\n\n"));
    CHECK(output_is("{\"m\":{\"v\":null}}\n{\"m\":{\"v\":0}}\n",
                    (run_t){ .from = HP_FORMAT_NDJSON, .agg = "count, count(m.v)" },
                    "count,count(m.v)\n2,1\n"));
}

static void test_columns_in_order(void) {
    const char *in = "host,bytes\nA.org,2048\nb.org,\n";
    /* Each column sees the ones before it; one named like an input column
     * replaces it in place */
    CHECK(output_is(in, (run_t){ .from = HP_FORMAT_CSV,
                                 .add = "kb = bytes / 1024, host = lower(host), mb = kb / 1024" },
                    "host,bytes,kb,mb\na.org,2048,2,0.001953125\nb.org,,,\n"));
    /* Filters, groups and sorting read computed columns */
    CHECK(output_is(in, (run_t){ .from = HP_FORMAT_CSV, .add = "kb = bytes / 1024",
                                 .where = "kb == 2", .select = "host" }, "host\nA.org\n"));
    CHECK(output_is(in, (run_t){ .from = HP_FORMAT_CSV, .add = "tld = split_part(host, '.', 2)",
                                 .group_by = "tld", .agg = "count" }, "tld,count\norg,2\n"));
    CHECK(output_is(in, (run_t){ .from = HP_FORMAT_CSV, .add = "n = len(host)", .sort = "-bytes, n",
                                 .select = "host" }, "host\nA.org\nb.org\n"));
    /* NDJSON output: numbers stay numbers, text is quoted */
    CHECK(output_is("x\n12\n", (run_t){ .from = HP_FORMAT_CSV, .to = HP_FORMAT_NDJSON,
                                        .add = "a = x * 2, b = concat(x), c = x / 0" },
                    "{\"x\":12,\"a\":24,\"b\":\"12\"}\n"));
}

static void test_csv_rows(void) {
    /* Short rows are padded, long ones lose their extra fields to the new
     * columns; text with the delimiter is quoted */
    CHECK(output_is("a,b\n1\n2,x,extra\n", (run_t){ .from = HP_FORMAT_CSV,
                                                    .add = "c = concat(a, ',', b)" },
                    "a,b,c\n1,,\"1,\"\n2,x,\"2,x\"\n"));
    /* JSON text in a CSV cell */
    CHECK(output_is("j\n\"{\"\"a\"\":{\"\"b\"\":[1,2]}}\"\n", (run_t){ .from = HP_FORMAT_CSV,
                                                                    .select = "j.a, j.a.b[1]" },
                    "j.a,j.a.b[1]\n\"{\"\"b\"\":[1,2]}\",2\n"));
}

static bool add_fails(const char *spec, const char *why, size_t at) {
    static hp_compute_t c;
    hp_error_t err = hp_compute_init(&c, spec);
    bool ok = err == HP_ERR_COMPUTE && strcmp(c.error, why) == 0 && c.error_at == at;
    if (!ok) fprintf(stderr, "# %s: %s at %zu\n", spec, c.error ? c.error : "no error", c.error_at);
    hp_compute_free(&c);
    return ok;
}

static void test_errors(void) {
    CHECK(add_fails("= 1", "expected a column name", 0));
    CHECK(add_fails("a 1", "expected '=' after the column name", 3));
    CHECK(add_fails("a == 1", "expected '=' after the column name", 2));
    CHECK(add_fails("a = 1 +", "expected a column, a number, quoted text or a function", 7));
    CHECK(add_fails("a = (1", "expected ')'", 6));
    CHECK(add_fails("a = 1 2", "expected an operator, or ',' and the next column", 6));
    CHECK(add_fails("a = nope(1)", "unknown function", 4));
    CHECK(add_fails("a = lower(1, 2)", "too many arguments", 13));
    CHECK(add_fails("a = substr(x)", "too few arguments", 4));
    CHECK(add_fails("a = replace(x, y, 'z')", "replace takes quoted text to find and to put in its place", 15));
    CHECK(add_fails("a = 'x", "unterminated quote", 4));

    /* A reference to nothing names the column */
    hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
                                   .compute_spec = "a = b.c + 1" };
    pipe_result_t r = pipe_run_str(&cfg, "x\n1\n");
    CHECK(r.err == HP_ERR_COLUMN_NOT_FOUND && strcmp(r.stats.error_column, "b.c") == 0);
    pipe_free(&r);
}

/* The parallel pipeline computes the same columns */
static void test_parallel_matches_one_pass(void) {
    const char *path = HP_TEST_TMPDIR "test_compute_parallel.ndjson";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 200000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        fprintf(f, "{\"id\":%d,\"m\":{\"u\":{\"k\":\"k%u\"},\"v\":[%d.%02u,\"\\u00e9\"]}}\n", i,
                (unsigned)(x % 50), (int)(x >> 40) % 2000 - 1000, (unsigned)(x >> 20) % 100);
    }
    fclose(f);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_NDJSON, .out_format = HP_FORMAT_CSV,
        .compute_spec = "v = m.v[0] * 3, k = upper(m.u.k), e = m.v[-1]",
        .where_spec = "m.v[0] > -500",
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

    CHECK(stats.rows_emitted > 100000);
    CHECK(one_len == par_len && memcmp(one, par, one_len) == 0);
    free(one);
    free(par);
}

int main(void) {
    TAP_RUN(test_arithmetic);
    TAP_RUN(test_number_functions);
    TAP_RUN(test_text_functions);
    TAP_RUN(test_paths);
    TAP_RUN(test_columns_in_order);
    TAP_RUN(test_csv_rows);
    TAP_RUN(test_errors);
    TAP_RUN(test_parallel_matches_one_pass);
    return tap_done();
}
