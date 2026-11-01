#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void run_differential_test(const char *test_name, const char *csv_input) {
    size_t in_len = strlen(csv_input);

    char out_scalar[128 * 1024] = {0};
    char out_avx2[128 * 1024] = {0};
    char out_avx512[128 * 1024] = {0};

    FILE *tf_in = fopen("diff_in.tmp", "wb");
    assert(tf_in != NULL);
    fwrite(csv_input, 1, in_len, tf_in);
    fclose(tf_in);

    /* 1. Run Scalar */
    hp_transcoder_config_t cfg_scalar = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_SCALAR
    };
    hp_transcoder_t tc_scalar;
    assert(hp_transcoder_init(&tc_scalar, &cfg_scalar) == HP_OK);
    FILE *f_in1 = fopen("diff_in.tmp", "rb");
    FILE *f_out1 = fopen("diff_scalar.tmp", "wb");
    assert(hp_transcoder_run(&tc_scalar, f_in1, f_out1) == HP_OK);
    fclose(f_in1);
    fclose(f_out1);
    hp_transcoder_free(&tc_scalar);

    /* 2. Run AVX2 */
    hp_transcoder_config_t cfg_avx2 = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_AVX2
    };
    hp_transcoder_t tc_avx2;
    assert(hp_transcoder_init(&tc_avx2, &cfg_avx2) == HP_OK);
    FILE *f_in2 = fopen("diff_in.tmp", "rb");
    FILE *f_out2 = fopen("diff_avx2.tmp", "wb");
    assert(hp_transcoder_run(&tc_avx2, f_in2, f_out2) == HP_OK);
    fclose(f_in2);
    fclose(f_out2);
    hp_transcoder_free(&tc_avx2);

    /* 3. Run AVX-512 */
    hp_transcoder_config_t cfg_avx512 = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_AVX512
    };
    hp_transcoder_t tc_avx512;
    assert(hp_transcoder_init(&tc_avx512, &cfg_avx512) == HP_OK);
    FILE *f_in3 = fopen("diff_in.tmp", "rb");
    FILE *f_out3 = fopen("diff_avx512.tmp", "wb");
    assert(hp_transcoder_run(&tc_avx512, f_in3, f_out3) == HP_OK);
    fclose(f_in3);
    fclose(f_out3);
    hp_transcoder_free(&tc_avx512);

    /* Read outputs and compare */
    FILE *r1 = fopen("diff_scalar.tmp", "rb");
    size_t n1 = fread(out_scalar, 1, sizeof(out_scalar), r1);
    fclose(r1);

    FILE *r2 = fopen("diff_avx2.tmp", "rb");
    size_t n2 = fread(out_avx2, 1, sizeof(out_avx2), r2);
    fclose(r2);

    FILE *r3 = fopen("diff_avx512.tmp", "rb");
    size_t n3 = fread(out_avx512, 1, sizeof(out_avx512), r3);
    fclose(r3);

    remove("diff_in.tmp");
    remove("diff_scalar.tmp");
    remove("diff_avx2.tmp");
    remove("diff_avx512.tmp");

    assert(n1 == n2);
    assert(memcmp(out_scalar, out_avx2, n1) == 0);
    assert(n1 == n3);
    assert(memcmp(out_scalar, out_avx512, n1) == 0);

    printf("  [PASS] %s (bytes: %zu, scalar == avx2 == avx512)\n", test_name, n1);
}

int main(void) {
    printf("[test_differential] Running differential SIMD validation tests...\n");

    /* Test 1: Simple plain ASCII tabular data */
    run_differential_test("Plain ASCII table",
        "id,user,score,city\n"
        "1001,alice,95.5,Seattle\n"
        "1002,bob,88.0,Austin\n"
        "1003,charlie,72.2,Boston\n");

    /* Test 2: Quotes and escaped quotes */
    run_differential_test("Quotes and escaped quotes",
        "title,notes,flag\n"
        "\"The \"\"Big\"\" Book\",\"Multi, comma, note\",true\n"
        "\"Simple title\",\"Normal notes\",false\n");

    /* Test 3: Large row with 64+ bytes per field to span vector register boundaries */
    run_differential_test("Wide fields spanning 64-byte AVX-512 boundaries",
        "colA,colB,colC\n"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_,1234567890123456789012345678901234567890123456789012345678901234567890,end\n"
        "SecondRowAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA,SecondRowBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB,999\n");

    /* Test 4: Mixed CRLF and LF with empty fields */
    run_differential_test("Mixed CRLF/LF and empty fields",
        "a,b,c,d\r\n"
        ",2,,4\n"
        "1,,,4\r\n"
        ",,,\n");

    /* Test 5: Escape characters (tabs, newlines, backslashes in quoted strings) */
    run_differential_test("Special escape characters in quoted fields",
        "name,details,code\n"
        "\"Alpha\",\"line1\\line2\twith\ttabs\",10\n"
        "\"Beta\nNewline\",\"quote: \"\"nested\"\" and slash: \\\\\",20\n");

    /* Test 6: Single column table */
    run_differential_test("Single column table",
        "single_column\n"
        "first_value\n"
        "\"quoted_value\"\n"
        "third_value\n");

    printf("[test_differential] All differential tests PASSED: Scalar == AVX2 == AVX-512.\n");
    return 0;
}
