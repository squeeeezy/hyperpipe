#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_format_sniffing(void) {
    const char *csv_sample = "id,name,role\n1,alice,admin\n";
    assert(hp_sniff_format((const uint8_t *)csv_sample, strlen(csv_sample)) == HP_FORMAT_CSV);

    const char *tsv_sample = "id\tname\trole\n1\talice\tadmin\n";
    assert(hp_sniff_format((const uint8_t *)tsv_sample, strlen(tsv_sample)) == HP_FORMAT_TSV);

    const char *ndjson_sample = "{\"id\":1,\"name\":\"alice\"}\n{\"id\":2,\"name\":\"bob\"}\n";
    assert(hp_sniff_format((const uint8_t *)ndjson_sample, strlen(ndjson_sample)) == HP_FORMAT_NDJSON);

    const char *bom_tsv = "\xEF\xBB\xBFid\tname\tage\n";
    assert(hp_sniff_format((const uint8_t *)bom_tsv, strlen(bom_tsv)) == HP_FORMAT_TSV);
}

static void test_fault_tolerant_skip(void) {
    /* CSV where second row is corrupted (unclosed quote without terminating quote) */
    const char *dirty_csv =
        "id,user,status\n"
        "1,alice,active\n"
        "2,\"corrupted_unclosed_quote,active\n"
        "3,charlie,active\n";

    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_NDJSON,
        .simd_mode = HP_SIMD_SCALAR,
        .on_error = HP_ON_ERROR_SKIP,
        .buffer_capacity = 4096,
    };

    hp_transcoder_t tc;
    assert(hp_transcoder_init(&tc, &cfg) == HP_OK);

    FILE *in_mem = tmpfile();
    FILE *out_mem = tmpfile();
    fputs(dirty_csv, in_mem);
    rewind(in_mem);

    hp_error_t err = hp_transcoder_run(&tc, in_mem, out_mem);
    assert(err == HP_OK);

    /* Verify row 1 and row 3 were emitted, row 2 dropped */
    assert(tc.stats.rows_read == 2);
    assert(tc.stats.rows_emitted == 2);
    assert(tc.stats.rows_dropped == 1);

    hp_transcoder_free(&tc);
    fclose(in_mem);
    fclose(out_mem);
}

int main(void) {
    test_format_sniffing();
    test_fault_tolerant_skip();
    printf("test_fault_tolerance passed successfully.\n");
    return 0;
}
