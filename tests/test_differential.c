/* The scalar, AVX2 and AVX-512 engines must produce byte-identical output
 * for the same input. Each case targets a shape the vector kernels handle
 * differently: quotes, fields across 64-byte chunk boundaries, CRLF. */

#include "pipe.h"

static void check_engines_agree(const char *csv) {
    static const hp_simd_mode_t engines[] = { HP_SIMD_SCALAR, HP_SIMD_AVX2, HP_SIMD_AVX512 };
    pipe_result_t ref;
    for (size_t i = 0; i < sizeof(engines) / sizeof(engines[0]); i++) {
        hp_transcoder_config_t cfg = {
            .in_format = HP_FORMAT_CSV,
            .out_format = HP_FORMAT_NDJSON,
            .simd_mode = engines[i],
        };
        pipe_result_t r = pipe_run_str(&cfg, csv);
        CHECK(r.err == HP_OK);
        if (i == 0) {
            ref = r;
            continue;
        }
        CHECK(r.len == ref.len && memcmp(r.out, ref.out, ref.len) == 0);
        pipe_free(&r);
    }
    pipe_free(&ref);
}

static void test_plain_table(void) {
    check_engines_agree(
        "id,user,score,city\n"
        "1001,alice,95.5,Seattle\n"
        "1002,bob,88.0,Austin\n"
        "1003,charlie,72.2,Boston\n");
}

static void test_quotes_and_doubled_quotes(void) {
    check_engines_agree(
        "title,notes,flag\n"
        "\"The \"\"Big\"\" Book\",\"Multi, comma, note\",true\n"
        "\"Simple title\",\"Normal notes\",false\n");
}

static void test_fields_across_64_byte_chunks(void) {
    check_engines_agree(
        "colA,colB,colC\n"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_,"
        "1234567890123456789012345678901234567890123456789012345678901234567890,end\n"
        "SecondRowAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA,"
        "SecondRowBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB,999\n");
}

static void test_mixed_line_endings_and_empty_fields(void) {
    check_engines_agree(
        "a,b,c,d\r\n"
        ",2,,4\n"
        "1,,,4\r\n"
        ",,,\n");
}

static void test_control_bytes_in_quoted_fields(void) {
    check_engines_agree(
        "name,details,code\n"
        "\"Alpha\",\"line1\\line2\twith\ttabs\",10\n"
        "\"Beta\nNewline\",\"quote: \"\"nested\"\" and slash: \\\\\",20\n");
}

static void test_single_column(void) {
    check_engines_agree(
        "single_column\n"
        "first_value\n"
        "\"quoted_value\"\n"
        "third_value\n");
}

int main(void) {
    TAP_RUN(test_plain_table);
    TAP_RUN(test_quotes_and_doubled_quotes);
    TAP_RUN(test_fields_across_64_byte_chunks);
    TAP_RUN(test_mixed_line_endings_and_empty_fields);
    TAP_RUN(test_control_bytes_in_quoted_fields);
    TAP_RUN(test_single_column);
    return tap_done();
}
