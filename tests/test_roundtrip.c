#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
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
    assert(hp_transcoder_init(&tc_json, &cfg_to_json) == HP_OK);
    FILE *in1 = fopen("rt_orig.csv", "rb");
    FILE *out1 = fopen("rt_stage1.ndjson", "wb");
    assert(hp_transcoder_run(&tc_json, in1, out1) == HP_OK);
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
    assert(hp_transcoder_init(&tc_csv, &cfg_to_csv) == HP_OK);
    FILE *in2 = fopen("rt_stage1.ndjson", "rb");
    FILE *out2 = fopen("rt_final.csv", "wb");
    assert(hp_transcoder_run(&tc_csv, in2, out2) == HP_OK);
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

    assert(final_len == strlen(orig_csv));
    assert(memcmp(final_buf, orig_csv, final_len) == 0);
    printf("  [PASS] CSV -> NDJSON -> CSV roundtrip verified.\n");
}

int main(void) {
    printf("[test_roundtrip] Running golden file roundtrip tests...\n");
    test_csv_to_ndjson_to_csv();
    printf("[test_roundtrip] All roundtrip tests passed successfully.\n");
    return 0;
}
