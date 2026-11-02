/* End-to-end regression tests: each case drives the full transcoder over an
 * in-memory input and pins one previously observed defect. */

#include "pipe.h"

/* The first 4 KiB of output by value, which keeps the cases short */
typedef struct run_result_s {
    hp_error_t err;
    char out[4096];
    uint64_t rows_emitted;
} run_result_t;

static run_result_t run_config(const char *input, const hp_transcoder_config_t *cfg) {
    pipe_result_t p = pipe_run_str(cfg, input);
    run_result_t r = { .err = p.err, .rows_emitted = p.stats.rows_emitted };
    size_t n = p.len < sizeof(r.out) - 1 ? p.len : sizeof(r.out) - 1;
    memcpy(r.out, p.out, n);
    r.out[n] = '\0';
    pipe_free(&p);
    return r;
}

static run_result_t run_pipeline(const char *input, const char *where, const char *select,
                                 hp_format_t to) {
    hp_transcoder_config_t cfg = {
        .out_format = to,
        .where_spec = where,
        .select_spec = select,
    };
    return run_config(input, &cfg);
}

static uint64_t count_matches(const char *input, const char *where) {
    run_result_t r = run_pipeline(input, where, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK);
    return r.rows_emitted;
}

/* A lone `==` predicate used to take a byte-compare shortcut, so
 * `a == 5000.0` missed "5000" while `a == 5000.0 and a == 5000.0` hit it. */
static void test_single_equality_is_numeric(void) {
    const char *csv = "a,b\n5000,x\n5000.0,y\n5e3,z\n6000,w\n";
    CHECK(count_matches(csv, "a == 5000.0") == 3);
    CHECK(count_matches(csv, "a == 5000.0 and a == 5000.0") == 3);
    CHECK(count_matches(csv, "a == 5000") == 3);
    CHECK(count_matches(csv, "b == \"x\"") == 1);

    /* Non-canonical spellings of the same integer are still equal */
    const char *spellings = "a\n500\n0500\n+500\n500.0\n501\n";
    CHECK(count_matches(spellings, "a == 500") == 4);
    CHECK(count_matches(spellings, "a != 500") == 1);
    CHECK(count_matches("a\n-0\n0\n00\n", "a == 0") == 3);

    /* Every spelling of 500 matches; near misses that differ in a
     * significant digit do not */
    const char *forms = "a\n500\n5e2\n50e1\n5.0e2\n500.0\n0500\n+500\n5000e-1\n"
                        "501\n499\n600\n5000\n50\n5e3\n";
    CHECK(count_matches(forms, "a == 500") == 8);
    CHECK(count_matches("a\n12e2\n13e2\n1200\n1.2e3\n", "a == 1200") == 3);
}

/* Numeric predicates must accept a decimal number spanning the whole field,
 * not a numeric prefix, and must not treat hex, inf or nan as numbers. */
static void test_numeric_compare_needs_whole_field(void) {
    const char *csv = "a\n200abc\n0x10\ninf\nnan\n 20\n20\n";
    CHECK(count_matches(csv, "a > 15.5") == 1);
    CHECK(count_matches(csv, "a > 15") == 1);
}

static void test_int64_min_is_an_integer(void) {
    const char *csv = "a\n-9223372036854775808\n9223372036854775807\n";
    CHECK(count_matches(csv, "a < 0") == 1);
    CHECK(count_matches(csv, "a > 0") == 1);
}

/* A header named "2019" is a column name first; only a number that is not
 * a column name falls back to a positional index. */
static void test_numeric_column_name_resolves_by_name(void) {
    run_result_t r = run_pipeline("x,2019\n7,8\n", NULL, "2019", HP_FORMAT_NDJSON);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "{\"2019\":8}\n") == 0);

    r = run_pipeline("x,y\n7,8\n", NULL, "1", HP_FORMAT_CSV);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "y\n8\n") == 0);

    CHECK(count_matches("x,2019\n7,8\n7,9\n", "2019 == 8") == 1);
}

/* Bytes after a closing quote used to be dropped silently ("abc"xyz -> abc).
 * They are malformed CSV, so the default fail mode must reject the row,
 * matching how a stray quote inside an unquoted field is treated. */
static void test_text_after_closing_quote_is_an_error(void) {
    run_result_t r = run_pipeline("a,b\n\"abc\"xyz,2\n", NULL, NULL, HP_FORMAT_NDJSON);
    CHECK(r.err == HP_ERR_PARSE_QUOTE);

    r = run_pipeline("a,b\n\"abc\" ,2\n", NULL, NULL, HP_FORMAT_NDJSON);
    CHECK(r.err == HP_ERR_PARSE_QUOTE);
}

/* Projected columns that do not exist in a row must be marked absent, not
 * inherit whatever `present` value the destination row held before. */
static void test_projection_marks_missing_columns_absent(void) {
    hp_header_t hdr = { .count = 1 };
    hdr.names[0] = strdup("a");

    hp_projection_t proj;
    CHECK(hp_projection_init(&proj, "a,0") == HP_OK);
    CHECK(hp_projection_bind(&proj, &hdr) == HP_OK);

    static hp_row_t src, dst;
    src.field_count = 0;
    dst.fields[0].present = true;
    dst.fields[1].present = true;
    CHECK(hp_project_row(&proj, &src, &dst) == HP_OK);
    CHECK(dst.field_count == 2);
    CHECK(!dst.fields[0].present && !dst.fields[1].present);

    hp_header_free(&hdr);
}

/* CSV output used to memcpy fields into the 256 KiB page buffer after only
 * reserving 2 KiB of headroom, so one long field overran the heap. */
static void test_field_longer_than_output_page(void) {
    const size_t field_len = 300 * 1024;
    char *csv = malloc(field_len + 16);
    CHECK(csv);
    memcpy(csv, "a,b\n", 4);
    memset(csv + 4, 'x', field_len);
    strcpy(csv + 4 + field_len, ",1\n");

    run_result_t r = run_pipeline(csv, NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK && r.rows_emitted == 1);
    r = run_pipeline(csv, NULL, "a,b,a", HP_FORMAT_CSV);
    CHECK(r.err == HP_OK && r.rows_emitted == 1);
    free(csv);
}

/* A field made only of quotes doubles in size on output: the worst case of
 * the output reservation, on both sides of the one-page limit. */
static void test_quote_heavy_field_round_trips(void) {
    const size_t sizes[] = { 40 * 1024, 200 * 1024 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        const size_t quotes = sizes[s];
        char *csv = malloc(2 * quotes + 16);
        CHECK(csv);
        size_t w = 0;
        memcpy(csv, "a\n\"", 3);
        w = 3;
        for (size_t i = 0; i < quotes; i++) {
            csv[w++] = '"';
            csv[w++] = '"';
        }
        strcpy(csv + w, "\"\n");

        run_result_t r = run_pipeline(csv, NULL, NULL, HP_FORMAT_CSV);
        CHECK(r.err == HP_OK && r.rows_emitted == 1);
        CHECK(strncmp(r.out, csv, sizeof(r.out) - 1) == 0);
        free(csv);
    }
}

/* Escaped NDJSON strings were decoded into a fixed 64 KiB arena that
 * dropped bytes once full while still counting them, so long values read
 * past the arena. Both the CSV and the NDJSON input path must accept a
 * record as large as the input buffer. */
static void test_long_escaped_values(void) {
    const size_t body = 100 * 1024;
    char *json = malloc(body + 32);
    CHECK(json);
    strcpy(json, "{\"a\":\"\\n");
    size_t w = strlen(json);
    memset(json + w, 'x', body);
    strcpy(json + w + body, "\\t\"}\n");

    run_result_t r = run_pipeline(json, NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK && r.rows_emitted == 1);
    CHECK(strncmp(r.out, "a\n\"\nxxx", 7) == 0);
    free(json);
}

/* A TSV cell may contain commas. Copying it verbatim into CSV output split
 * one field into two ("x,y" -> x,y). */
static void test_tsv_to_csv_quotes_commas(void) {
    run_result_t r = run_pipeline("a\tb\nx,y\t1\n", NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "a,b\n\"x,y\",1\n") == 0);

    r = run_pipeline("a,b\n\"x\ty\",1\n", NULL, NULL, HP_FORMAT_TSV);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "a\tb\n\"x\ty\"\t1\n") == 0);
}

/* The quoting decision scans values a word at a time; place the special
 * byte at every position of every length around the word boundaries. */
static void test_quoting_detects_special_at_any_offset(void) {
    /* A one-column file would sniff as CSV, so the TSV reading is forced */
    const hp_transcoder_config_t cfg = { .in_format = HP_FORMAT_TSV, .out_format = HP_FORMAT_CSV };
    for (size_t len = 1; len <= 40; len++) {
        for (size_t at = 0; at < len; at++) {
            char cell[64], tsv[128], expect[128];
            memset(cell, 'x', len);
            cell[at] = ',';
            cell[len] = '\0';
            snprintf(tsv, sizeof(tsv), "a\n%s\n", cell);
            snprintf(expect, sizeof(expect), "a\n\"%s\"\n", cell);

            run_result_t r = run_config(tsv, &cfg);
            CHECK(r.err == HP_OK);
            CHECK(strcmp(r.out, expect) == 0);
        }
    }
}

/* A record larger than the output page bypassed the compressor and went
 * into a --gzip stream raw. Compress one such record, read it back through
 * the transparent gunzip input and compare every byte. */
static void test_gzip_output_covers_oversized_records(void) {
    const size_t field_len = 300 * 1024;
    char *csv = malloc(field_len + 16);
    CHECK(csv);
    memcpy(csv, "a,b\n", 4);
    memset(csv + 4, 'x', field_len);
    strcpy(csv + 4 + field_len, ",1\n");

    const hp_transcoder_config_t pack = { .out_format = HP_FORMAT_CSV, .gzip_out = true };
    const hp_transcoder_config_t unpack = { .out_format = HP_FORMAT_CSV };
    pipe_result_t packed = pipe_run_str(&pack, csv);
    if (packed.err == HP_ERR_GZIP_UNAVAILABLE) {
        printf("# zlib not found, gzip round trip not exercised\n");
    } else {
        CHECK(packed.err == HP_OK);
        pipe_result_t plain = pipe_run(&unpack, packed.out, packed.len);
        CHECK(plain.err == HP_OK);
        CHECK(strcmp(plain.out, csv) == 0);
        pipe_free(&plain);
    }
    pipe_free(&packed);
    free(csv);
}

/* Each malformed NDJSON line must be rejected: as an error in the default
 * fail mode, as one dropped row in skip mode. The long form puts a known
 * key first so the line also takes the SIMD structural path. */
static void test_malformed_ndjson_is_rejected(void) {
    static const char *const bad[] = {
        "{\"id\":5,\"v\":",          /* truncated inside the value */
        "{\"id\":5",                 /* no closing brace */
        "{\"id\":5,\"v\":\"x\"}}",   /* text after the object */
        "{\"id\":5,\"v\":\"x\"} x",
        "{\"id\":5 \"v\":1}",        /* missing comma */
        "{\"id\":5,}",               /* trailing comma */
        "{,\"id\":5}",
        "{\"id\":}",                 /* missing value */
        "{\"id\"5}",                 /* missing colon */
        "{\"id\":abc}",              /* not a literal */
        "{\"id\":tru}",
        "{\"id\":01}",               /* leading zero */
        "{\"id\":1.}",
        "{\"id\":\"x}",              /* unterminated strings */
        "{\"id\":\"x\\",
        "{\"id\":\"a\\qb\"}",        /* invalid escapes */
        "{\"id\":\"\\u12\"}",
    };
    const char *pad = "\"p\":\"pppppppppppppppppppppppppppppppppppppppp\",";
    const char *first = "{\"p\":\"pppppppppppppppppppppppppppppppppppppppp\",\"id\":0,\"v\":\"a\"}\n";
    const char *last = "{\"id\":9,\"v\":\"z\"}\n";

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (int longer = 0; longer < 2; longer++) {
            char line[256], doc[512];
            if (longer) snprintf(line, sizeof(line), "{%s%s", pad, bad[i] + 1);
            else        snprintf(line, sizeof(line), "%s", bad[i]);
            snprintf(doc, sizeof(doc), "%s%s\n%s", first, line, last);

            run_result_t r = run_pipeline(doc, NULL, NULL, HP_FORMAT_CSV);
            if (r.err == HP_OK) fprintf(stderr, "accepted: %s\n", line);
            CHECK(r.err == HP_ERR_PARSE_JSON);

            hp_transcoder_config_t cfg = { .out_format = HP_FORMAT_CSV,
                                           .on_error = HP_ON_ERROR_SKIP };
            r = run_config(doc, &cfg);
            CHECK(r.err == HP_OK && r.rows_emitted == 2);
        }
    }
}

/* Valid JSON spellings that a strict parser must keep accepting */
static void test_valid_ndjson_edge_cases(void) {
    const char *doc =
        "{\"a\":1,\"b\":\"x\"}\n"
        "  { \"a\" : -0.5e10 , \"b\" : \"y\" }  \n"
        "{}\n"
        "{\"b\":null,\"a\":true}\n"
        "{\"a\":false,\"b\":\"\\u00e9\\n\\/\"}\n"
        "{\"a\":0,\"b\":\"\"}\r\n";
    run_result_t r = run_pipeline(doc, NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK && r.rows_emitted == 6);
    CHECK(strcmp(r.out, "a,b\n1,x\n-0.5e10,y\n,\ntrue,null\nfalse,\"\xc3\xa9\n/\"\n0,\"\"\n") == 0);
}

/* Keys and header names were cut to 255 bytes, so two long keys sharing
 * that prefix collapsed into one column */
static void test_long_names_are_kept_whole(void) {
    char prefix[301], doc[1024];
    memset(prefix, 'k', 300);
    prefix[300] = '\0';
    snprintf(doc, sizeof(doc), "{\"%sA\":1,\"%sB\":2}\n", prefix, prefix);

    run_result_t r = run_pipeline(doc, NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK);
    char expect[1024];
    snprintf(expect, sizeof(expect), "%sA,%sB\n1,2\n", prefix, prefix);
    CHECK(strcmp(r.out, expect) == 0);

    snprintf(doc, sizeof(doc), "%sA,%sB\n1,2\n", prefix, prefix);
    r = run_pipeline(doc, NULL, NULL, HP_FORMAT_CSV);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, doc) == 0);
}

/* Switching to the scalar engine left the NDJSON structural kernel on the
 * vector version, so "scalar" runs still used AVX code */
static void test_scalar_engine_replaces_every_kernel(void) {
    hp_dispatch_init(HP_SIMD_AVX512);
    hp_dispatch_init(HP_SIMD_SCALAR);
    CHECK(g_hp_dispatch.active_mode == HP_SIMD_SCALAR);
    CHECK(g_hp_dispatch.json_struct_masks == hp_json_struct_masks_scalar);
    CHECK(g_hp_dispatch.csv_parse_row == hp_csv_parse_row_scalar);
    CHECK(g_hp_dispatch.json_emit_string == hp_json_emit_body_scalar);
    hp_dispatch_init(HP_SIMD_AUTO);
}

int main(void) {
    TAP_RUN(test_single_equality_is_numeric);
    TAP_RUN(test_numeric_compare_needs_whole_field);
    TAP_RUN(test_int64_min_is_an_integer);
    TAP_RUN(test_numeric_column_name_resolves_by_name);
    TAP_RUN(test_text_after_closing_quote_is_an_error);
    TAP_RUN(test_projection_marks_missing_columns_absent);
    TAP_RUN(test_field_longer_than_output_page);
    TAP_RUN(test_quote_heavy_field_round_trips);
    TAP_RUN(test_long_escaped_values);
    TAP_RUN(test_tsv_to_csv_quotes_commas);
    TAP_RUN(test_quoting_detects_special_at_any_offset);
    TAP_RUN(test_gzip_output_covers_oversized_records);
    TAP_RUN(test_valid_ndjson_edge_cases);
    TAP_RUN(test_malformed_ndjson_is_rejected);
    TAP_RUN(test_long_names_are_kept_whole);
    TAP_RUN(test_scalar_engine_replaces_every_kernel);
    return tap_done();
}
