/* --summary column statistics: the accumulator over hand-built rows, and
 * the full transcoder over an NDJSON stream with sparse rows, schema growth
 * and mixed column types. */

#include "hyperpipe/hp_summary.h"
#include "pipe.h"

static void set_field(hp_row_t *row, size_t idx, const char *val, bool quoted, bool present) {
    row->fields[idx].ptr = val;
    row->fields[idx].len = strlen(val);
    row->fields[idx].has_escapes = quoted;
    row->fields[idx].present = present;
    if (idx + 1 > row->field_count) row->field_count = idx + 1;
}

static void test_accumulator(void) {
    static hp_summary_t s;
    hp_summary_init(&s);

    hp_header_t header = { .count = 3 };
    header.names[0] = (char *)"status";
    header.names[1] = (char *)"host";
    header.names[2] = (char *)"bytes";

    static hp_row_t r1, r2, r3;
    set_field(&r1, 0, "200", false, true);
    set_field(&r1, 1, "web-1", true, true);
    set_field(&r1, 2, "1024", false, true);
    set_field(&r2, 0, "404", false, true);
    set_field(&r2, 1, "", false, false);      /* host absent */
    set_field(&r2, 2, "512", false, true);
    set_field(&r3, 0, "", false, false);      /* status absent */
    set_field(&r3, 1, "", true, true);        /* host empty */
    set_field(&r3, 2, "abc", true, true);     /* bytes not a number */

    hp_summary_observe(&s, &header, &r1);
    hp_summary_observe(&s, &header, &r2);
    hp_summary_observe(&s, &header, &r3);

    FILE *f = tmpfile();
    CHECK(f);
    hp_summary_print(&s, f);
    size_t len;
    char *table = pipe_slurp(f, &len);

    CHECK(s.rows == 3);
    CHECK(s.cols[0].num_count == 2 && s.cols[0].int_count == 2 && s.cols[0].absent == 1);
    CHECK(s.cols[1].absent == 1 && s.cols[1].empty == 1);
    CHECK(s.cols[2].num_count == 2 && s.cols[2].has_string);
    CHECK(strstr(table, "int") && strstr(table, "200") && strstr(table, "404"));
    CHECK(strstr(table, "mixed"));
    free(table);
}

static void test_transcoder_summary(void) {
    const hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_NDJSON,
        .out_format = HP_FORMAT_CSV,     /* ignored by summary mode */
        .summary = true,
    };
    pipe_result_t r = pipe_run_str(&cfg,
        "{\"a\":1,\"b\":\"x\"}\n"
        "{\"a\":2,\"b\":\"y\"}\n"
        "{\"a\":3,\"c\":\"late\"}\n");
    CHECK(r.err == HP_OK);
    CHECK(strstr(r.out, "column") && strstr(r.out, "rows: 3"));
    CHECK(strstr(r.out, "int"));
    CHECK(strstr(r.out, "late"));    /* the column that appears on row 3 */
    pipe_free(&r);
}

int main(void) {
    TAP_RUN(test_accumulator);
    TAP_RUN(test_transcoder_summary);
    return tap_done();
}
