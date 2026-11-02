/* test_summary: --summary column statistics.
 *
 * Unit part feeds hand-built rows directly into hp_summary_observe.
 * Integration part runs the full transcoder with config.summary over an
 * NDJSON stream that exercises sparse rows, schema growth and mixed column
 * types, capturing the printed table through fmemopen. */

#include "hyperpipe/hp_summary.h"
#include "hyperpipe/hp_transcoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  [PASS] %s\n", msg); \
        } else { \
            printf("  [FAIL] %s\n", msg); \
            failures++; \
        } \
    } while (0)

static void set_field(hp_row_t *row, size_t idx, const char *val, size_t len,
                      bool escapes, bool present) {
    row->fields[idx].ptr = val;
    row->fields[idx].len = len;
    row->fields[idx].has_escapes = escapes;
    row->fields[idx].present = present;
    if (idx + 1 > row->field_count) row->field_count = idx + 1;
}

static void test_unit_accumulator(void) {
    printf("[test_summary] unit: accumulator over hand-built rows\n");

    hp_summary_t s;
    hp_summary_init(&s);

    hp_header_t header;
    memset(&header, 0, sizeof(header));
    header.names[0] = (char *)"status";
    header.names[1] = (char *)"host";
    header.names[2] = (char *)"bytes";
    header.count = 3;

    hp_row_t r1, r2, r3;
    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    memset(&r3, 0, sizeof(r3));
    r1.field_count = r2.field_count = r3.field_count = 3;

    /* row 1: status=200, host="web-1", bytes=1024 */
    set_field(&r1, 0, "200", 3, false, true);
    set_field(&r1, 1, "web-1", 5, true, true);
    set_field(&r1, 2, "1024", 4, false, true);
    /* row 2: status=404, host absent, bytes=512 */
    set_field(&r2, 0, "404", 3, false, true);
    set_field(&r2, 1, "", 0, false, false);
    set_field(&r2, 2, "512", 3, false, true);
    /* row 3: status absent, host="", bytes=not-a-number */
    set_field(&r3, 0, "", 0, false, false);
    set_field(&r3, 1, "", 0, true, true);
    set_field(&r3, 2, "abc", 3, true, true);

    hp_summary_observe(&s, &header, &r1);
    hp_summary_observe(&s, &header, &r2);
    hp_summary_observe(&s, &header, &r3);

    char buf[4096];
    FILE *cap = fmemopen(buf, sizeof(buf), "w");
    hp_summary_print(&s, cap);
    fclose(cap);

    CHECK(s.rows == 3, "observed row count is 3");
    CHECK(s.cols[0].num_count == 2 && s.cols[0].int_count == 2,
          "status column: 2 integer values");
    CHECK(s.cols[0].absent == 1, "status column: 1 absent row");
    CHECK(strstr(buf, "int") != NULL, "status column typed int");
    CHECK(strstr(buf, "200") != NULL && strstr(buf, "404") != NULL,
          "status min/max present in table");
    CHECK(s.cols[2].num_count == 2 && s.cols[2].has_string,
          "bytes column mixes numbers and strings");
    CHECK(strstr(buf, "mixed") != NULL, "bytes column typed mixed");
    CHECK(s.cols[1].absent == 1 && s.cols[1].empty == 1,
          "host column: one absent, one empty");
}

static void test_integration_transcoder(void) {
    printf("[test_summary] integration: transcoder over NDJSON input\n");

    const char *input =
        "{\"a\":1,\"b\":\"x\"}\n"
        "{\"a\":2,\"b\":\"y\"}\n"
        "{\"a\":3,\"c\":\"late\"}\n";

    FILE *in = fmemopen((void *)input, strlen(input), "r");
    char out_buf[8192];
    memset(out_buf, 0, sizeof(out_buf));
    FILE *out = fmemopen(out_buf, sizeof(out_buf), "w");

    hp_transcoder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.in_format = HP_FORMAT_NDJSON;
    cfg.out_format = HP_FORMAT_CSV;   /* ignored by summary mode */
    cfg.summary = true;

    hp_transcoder_t tc;
    hp_error_t err = hp_transcoder_init(&tc, &cfg);
    if (err != HP_OK) {
        printf("  [FAIL] transcoder init (code %d)\n", err);
        failures++;
        fclose(in);
        fclose(out);
        return;
    }
    err = hp_transcoder_run(&tc, in, out);
    hp_transcoder_free(&tc);
    fclose(in);
    fclose(out);

    CHECK(err == HP_OK, "run completed");
    CHECK(strstr(out_buf, "column") != NULL, "table header printed");
    CHECK(strstr(out_buf, "rows: 3") != NULL, "row count footer");
    CHECK(strstr(out_buf, "int") != NULL, "column a typed int");
    CHECK(strstr(out_buf, "late") != NULL, "column c value captured");
    CHECK(strstr(out_buf, "3") != NULL && strstr(out_buf, "1") != NULL,
          "column a min/max values");
}

int main(void) {
    test_unit_accumulator();
    test_integration_transcoder();

    if (failures == 0) {
        printf("[test_summary] All summary tests passed.\n");
        return 0;
    }
    printf("[test_summary] %d FAILURE(S)\n", failures);
    return 1;
}
