/* Errors must say what is wrong and where: a malformed record by its number
 * and byte offset, a filter by the place where parsing stopped, a missing
 * column by its name. */

#include "hyperpipe/hp_filter.h"
#include "hyperpipe/hp_parallel.h"
#include "pipe.h"

#ifndef HP_TEST_TMPDIR
#define HP_TEST_TMPDIR "build/"
#endif

static pipe_result_t run_str(const char *input, hp_format_t from, const char *where,
                             const char *select) {
    hp_transcoder_config_t cfg = {
        .in_format = from,
        .where_spec = where,
        .select_spec = select,
    };
    return pipe_run_str(&cfg, input);
}

static void test_malformed_record_position(void) {
    /* Record 3 starts after "a,b\n1,2\n", at byte 8 */
    pipe_result_t r = run_str("a,b\n1,2\n3,\"x\"y\n4,5\n", HP_FORMAT_CSV, NULL, NULL);
    CHECK(r.err == HP_ERR_PARSE_QUOTE);
    CHECK(r.stats.error_record == 3 && r.stats.error_offset == 8);
    pipe_free(&r);

    /* NDJSON has no header: the blank line still counts as a record */
    r = run_str("{\"a\":1}\n\n{\"a\":}\n", HP_FORMAT_NDJSON, NULL, NULL);
    CHECK(r.err == HP_ERR_PARSE_JSON);
    CHECK(r.stats.error_record == 3 && r.stats.error_offset == 9);
    pipe_free(&r);

    r = run_str("a\n1\n2\n", HP_FORMAT_CSV, NULL, NULL);
    CHECK(r.err == HP_OK && r.stats.error_record == 0 && r.stats.records_in == 3);
    pipe_free(&r);
}

static void test_oversized_record_position(void) {
    static char doc[4096];
    size_t w = (size_t)snprintf(doc, sizeof(doc), "a\nshort\n");
    memset(doc + w, 'x', 2000);
    strcpy(doc + w + 2000, "\n");

    hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_CSV, .buffer_capacity = 1024 };
    pipe_result_t r = pipe_run_str(&cfg, doc);
    CHECK(r.err == HP_ERR_BUFFER_OVERFLOW);
    CHECK(r.stats.error_record == 3 && r.stats.error_offset == w);
    pipe_free(&r);
}

/* Chunk-relative positions must come out global */
static void test_parallel_position_matches_sequential(void) {
    const char *path = HP_TEST_TMPDIR "test_errors_parallel.csv";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs("id,name\n", f);
    for (int i = 0; i < 400000; i++) {
        fprintf(f, i == 333333 ? "%d,\"a\"b\n" : "%d,\"n,%d\"\n", i, i);
    }
    fclose(f);

    hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV };
    FILE *in = fopen(path, "rb");
    CHECK(in);
    hp_transcoder_t tc;
    CHECK(hp_transcoder_init(&tc, &cfg) == HP_OK);
    FILE *out = tmpfile();
    CHECK(hp_transcoder_run(&tc, in, out) == HP_ERR_PARSE_QUOTE);
    hp_transcoder_stats_t seq = tc.stats;
    hp_transcoder_free(&tc);
    fclose(in);
    fclose(out);

    hp_transcoder_stats_t par;
    out = tmpfile();
    CHECK(hp_parallel_run(&cfg, path, out, 8, &par) == HP_ERR_PARSE_QUOTE);
    fclose(out);
    remove(path);

    CHECK(seq.error_record == 333335);
    CHECK(par.error_record == seq.error_record);
    CHECK(par.error_offset == seq.error_offset);
}

static void test_missing_column_is_named(void) {
    pipe_result_t r = run_str("a,b\n1,2\n", HP_FORMAT_CSV, NULL, "a,zz");
    CHECK(r.err == HP_ERR_COLUMN_NOT_FOUND && strcmp(r.stats.error_column, "zz") == 0);
    pipe_free(&r);

    r = run_str("a,b\n1,2\n", HP_FORMAT_CSV, "b == 2 and qq > 1", NULL);
    CHECK(r.err == HP_ERR_COLUMN_NOT_FOUND && strcmp(r.stats.error_column, "qq") == 0);
    pipe_free(&r);
}

/* Each bad filter fails at a known offset instead of matching something */
static void test_filter_errors_have_a_position(void) {
    static const struct {
        const char *filter;
        size_t at;
    } bad[] = {
        { "a ==", 4 },
        { "a == 1 b == 2", 7 },
        { "a ~ 1", 2 },
        { "a == \"x", 5 },
        { "== 1", 0 },
        { "a == 1 and", 10 },
        { "a == 1 or or b == 2", 13 },
        { "a == ,", 5 },
    };
    static hp_filter_t filter;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        hp_error_t err = hp_filter_init(&filter, bad[i].filter);
        if (err != HP_ERR_FILTER || filter.error_at != bad[i].at) {
            fprintf(stderr, "# '%s': err %d at %zu\n", bad[i].filter, (int)err, filter.error_at);
        }
        CHECK(err == HP_ERR_FILTER && filter.error_at == bad[i].at && filter.error);
    }

    char long_value[400] = "a == ";
    memset(long_value + 5, 'x', 300);
    CHECK(hp_filter_init(&filter, long_value) == HP_ERR_FILTER);

    char many[512] = "";
    for (int i = 0; i < 17; i++) strcat(many, i ? " and a == 1" : "a == 1");
    CHECK(hp_filter_init(&filter, many) == HP_ERR_FILTER);

    CHECK(hp_filter_init(&filter, "a == 1, b != 2 && c > 3 || d contains x OR e = 5") == HP_OK);
    CHECK(filter.count == 5);
    CHECK(hp_filter_init(&filter, "  ") == HP_OK && filter.count == 0);
}

static void test_select_names_may_hold_spaces(void) {
    pipe_result_t r = run_str("first name,age\nAda,36\n", HP_FORMAT_CSV, NULL, " first name , age");
    CHECK(r.err == HP_OK && strcmp(r.out, "{\"first name\":\"Ada\",\"age\":36}\n") == 0);
    pipe_free(&r);
}

int main(void) {
    TAP_RUN(test_malformed_record_position);
    TAP_RUN(test_oversized_record_position);
    TAP_RUN(test_parallel_position_matches_sequential);
    TAP_RUN(test_missing_column_is_named);
    TAP_RUN(test_filter_errors_have_a_position);
    TAP_RUN(test_select_names_may_hold_spaces);
    return tap_done();
}
