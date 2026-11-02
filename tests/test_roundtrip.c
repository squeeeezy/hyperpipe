#include "hyperpipe/hp_transcoder.h"
#include "tap.h"
#include <stdio.h>
#include <string.h>

static void test_csv_to_ndjson_to_csv(void) {
    const char *orig_csv =
        "id,name,role,active\n"
        "1,Alice,Engineer,true\n"
        "2,Bob,Designer,false\n"
        "3,Charlie,Architect,true\n";

    FILE *f_in = fopen("rt_orig.csv", "wb");
    fwrite(orig_csv, 1, strlen(orig_csv), f_in);
    fclose(f_in);

    /* 1. Transcode CSV -> NDJSON */
    hp_transcoder_config_t cfg_to_json = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_AUTO
    };
    hp_transcoder_t tc_json;
    CHECK(hp_transcoder_init(&tc_json, &cfg_to_json) == HP_OK);
    FILE *in1 = fopen("rt_orig.csv", "rb");
    FILE *out1 = fopen("rt_stage1.ndjson", "wb");
    CHECK(hp_transcoder_run(&tc_json, in1, out1) == HP_OK);
    fclose(in1);
    fclose(out1);
    hp_transcoder_free(&tc_json);

    /* 2. Transcode NDJSON -> CSV */
    hp_transcoder_config_t cfg_to_csv = {
        .in_format = HP_FORMAT_NDJSON,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_AUTO
    };
    hp_transcoder_t tc_csv;
    CHECK(hp_transcoder_init(&tc_csv, &cfg_to_csv) == HP_OK);
    FILE *in2 = fopen("rt_stage1.ndjson", "rb");
    FILE *out2 = fopen("rt_final.csv", "wb");
    CHECK(hp_transcoder_run(&tc_csv, in2, out2) == HP_OK);
    fclose(in2);
    fclose(out2);
    hp_transcoder_free(&tc_csv);

    /* 3. Verify content */
    FILE *f_final = fopen("rt_final.csv", "rb");
    char final_buf[1024] = {0};
    size_t final_len = fread(final_buf, 1, sizeof(final_buf), f_final);
    fclose(f_final);

    remove("rt_orig.csv");
    remove("rt_stage1.ndjson");
    remove("rt_final.csv");

    CHECK(final_len == strlen(orig_csv));
    CHECK(memcmp(final_buf, orig_csv, final_len) == 0);
}

static void test_quoted_csv_roundtrip(void) {
    /* Regression: quoted fields with embedded delimiters/newlines MUST be
     * re-quoted on CSV output (v1.1 silently corrupted them). */
    const char *orig =
        "host,status,bytes\n"
        "\"x,1\",200,5\n"
        "\"multi\nline\",404,6\n"
        "\"say \"\"hi\"\"\",200,7\n";

    FILE *f_in = fopen("rt2_orig.csv", "wb");
    fwrite(orig, 1, strlen(orig), f_in);
    fclose(f_in);

    hp_transcoder_config_t cfg_pass = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_SCALAR
    };
    hp_transcoder_t tc;
    CHECK(hp_transcoder_init(&tc, &cfg_pass) == HP_OK);
    FILE *in = fopen("rt2_orig.csv", "rb");
    FILE *out = fopen("rt2_final.csv", "wb");
    CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
    fclose(in); fclose(out);
    hp_transcoder_free(&tc);

    FILE *f_final = fopen("rt2_final.csv", "rb");
    char buf[1024] = {0};
    size_t len = fread(buf, 1, sizeof(buf) - 1, f_final);
    fclose(f_final);
    remove("rt2_orig.csv");
    remove("rt2_final.csv");

    CHECK(len == strlen(orig));
    CHECK(memcmp(buf, orig, len) == 0 && "quoted CSV roundtrip must be byte-identical");
}

static void test_unterminated_quoted_eof(void) {
    /* Regression (found by the mutation fuzzer): the EOF tail of an
     * unterminated quoted field used to be emitted raw, leaking CR/LF/quote
     * bytes into the output as record structure. A quoted-origin field must
     * travel the re-quoting write path, and the CSV -> CSV pipeline must
     * reach a fingerprint-stable fixed point. */
    const char *orig = "a\n\"\runterminated\n\"\"";

    FILE *f_in = fopen("rt4_orig.csv", "wb");
    fwrite(orig, 1, strlen(orig), f_in);
    fclose(f_in);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_SCALAR
    };

    for (int pass = 0; pass < 2; pass++) {
        hp_transcoder_t tc;
        CHECK(hp_transcoder_init(&tc, &cfg) == HP_OK);
        FILE *in = fopen(pass == 0 ? "rt4_orig.csv" : "rt4_stage1.csv", "rb");
        FILE *out = fopen(pass == 0 ? "rt4_stage1.csv" : "rt4_stage2.csv", "wb");
        CHECK(in && out);
        CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
        fclose(in);
        fclose(out);
        hp_transcoder_free(&tc);
    }

    char buf1[1024] = {0}, buf2[1024] = {0};
    FILE *f1 = fopen("rt4_stage1.csv", "rb");
    size_t len1 = fread(buf1, 1, sizeof(buf1) - 1, f1);
    fclose(f1);
    FILE *f2 = fopen("rt4_stage2.csv", "rb");
    size_t len2 = fread(buf2, 1, sizeof(buf2) - 1, f2);
    fclose(f2);
    remove("rt4_orig.csv");
    remove("rt4_stage1.csv");
    remove("rt4_stage2.csv");

    CHECK(len1 > 0);
    CHECK(len1 == len2 && memcmp(buf1, buf2, len1) == 0 &&
           "unterminated-quoted EOF tail must reach a stable fixed point");
}

static void test_ndjson_fidelity(void) {
    /* Regression bundle (v1.1 bugs): unicode escapes, key reordering,
     * heterogeneous schema, type fidelity of quoted values. */
    const char *in_lines =
        "{\"host\":\"caf\\u00e9 \\ud83d\\ude00\",\"status\":200,\"a\":1}\n"
        "{\"a\":2,\"status\":404,\"b\":\"x\"}\n"
        "{\"status\":200,\"host\":\"null\",\"a\":3}\n";

    FILE *f_in = fopen("rt3_orig.ndjson", "wb");
    fwrite(in_lines, 1, strlen(in_lines), f_in);
    fclose(f_in);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_NDJSON,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_SCALAR
    };
    hp_transcoder_t tc;
    CHECK(hp_transcoder_init(&tc, &cfg) == HP_OK);
    FILE *in = fopen("rt3_orig.ndjson", "rb");
    FILE *out = fopen("rt3_final.ndjson", "wb");
    CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
    fclose(in); fclose(out);
    hp_transcoder_free(&tc);

    FILE *f_final = fopen("rt3_final.ndjson", "rb");
    char buf[2048] = {0};
    size_t len = fread(buf, 1, sizeof(buf) - 1, f_final);
    fclose(f_final);
    remove("rt3_orig.ndjson");
    remove("rt3_final.ndjson");
    buf[len] = '\0';

    /* 1. Unicode escapes decoded (accented char + surrogate-pair emoji) */
    CHECK(strstr(buf, "caf\xc3\xa9 \xf0\x9f\x98\x80") != NULL && "\\u decoding failed");
    /* 2. Reordered keys land in schema order (host,status,a,b) with correct values */
    CHECK(strstr(buf, "{\"status\":404,\"a\":2,\"b\":\"x\"}") != NULL && "key reorder failed");
    /* 3. Heterogeneous key 'b' keeps its name (was col_2) */
    CHECK(strstr(buf, "\"b\":\"x\"") != NULL && "heterogeneous key lost");
    /* 4. Sparse rows omit absent keys (host absent here) instead of emitting empties */
    CHECK(strstr(buf, "{\"host\":\"null\",\"status\":200,\"a\":3}") != NULL && "sparse row failed");
    /* 5. Type fidelity: the STRING "null" stays a string (was JSON null) */
    CHECK(strstr(buf, "\"host\":\"null\"") != NULL && "string 'null' mangled to null literal");

}

static void test_or_and_string_ops(void) {
    /* v1.2 features: or-groups, contains/startswith/endswith, --head limit. */
    const char *csv =
        "url,st\n"
        "/api/v1,200\n"
        "/static/x.js,200\n"
        "/api/v2,404\n";

    FILE *f_in = fopen("rt4.csv", "wb");
    fwrite(csv, 1, strlen(csv), f_in);
    fclose(f_in);

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_SCALAR,
        .where_spec = "url startswith /api and st == 200 or url contains .js"
    };
    hp_transcoder_t tc;
    CHECK(hp_transcoder_init(&tc, &cfg) == HP_OK);
    FILE *in = fopen("rt4.csv", "rb");
    FILE *out = fopen("rt4.out", "wb");
    CHECK(hp_transcoder_run(&tc, in, out) == HP_OK);
    fclose(in); fclose(out);
    uint64_t emitted = tc.stats.rows_emitted;
    hp_transcoder_free(&tc);

    FILE *f = fopen("rt4.out", "rb");
    char buf[1024] = {0};
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove("rt4.csv");
    remove("rt4.out");
    buf[len] = '\0';

    /* Group 1 (startswith /api AND st == 200): /api/v1
     * Group 2 (contains .js): /static/x.js
     * /api/v2 (404) must NOT match either group. */
    CHECK(emitted == 2 && "or-group filter emitted wrong row count");
    CHECK(strstr(buf, "/api/v1") != NULL && strstr(buf, "/static/x.js") != NULL);
    CHECK(strstr(buf, "/api/v2") == NULL);
}

int main(void) {
    TAP_RUN(test_csv_to_ndjson_to_csv);
    TAP_RUN(test_quoted_csv_roundtrip);
    TAP_RUN(test_unterminated_quoted_eof);
    TAP_RUN(test_ndjson_fidelity);
    TAP_RUN(test_or_and_string_ops);
    return tap_done();
}
