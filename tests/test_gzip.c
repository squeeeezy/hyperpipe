/* Gzip integration tests: transparent input decompression, output
 * compression, and multi-member streams.
 *
 * The compressed fixture is embedded as hex so the suite is deterministic
 * and self-contained: it verifies the exact byte-for-byte decompression of
 * a known gzip stream (produced with zlib/wbits=31, i.e. a real gzip
 * container), our own gzip writer against a system-independent expectation,
 * and gzip(1)-style concatenation of two members.
 *
 * POSIX-only (fmemopen/open_memstream); skipped on other platforms. */
#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)

static const char *FIXTURE_CSV =
    "host,path,status,bytes\n"
    "alpha.example.com,/index.html,200,5120\n"
    "beta.example.net,/api/v1,404,0\n"
    "\"gamma, inc\",/log?a=1&b=2,200,321\n";

static const char *FIXTURE_GZ_HEX =
    "1f8b080000000000020345cc3d0a02311006d03ea7902dac3e92495ccbc5b34c"
    "d66113c81f6494f5f6828dfde3a53e158335612aeb6b227e54a6e132125b39b9"
    "8e2276ef152eb7a79c36692d0844b8fb40268afe551385e391dddb63a5156496"
    "836b655c72db17b8d28f076ffe1ab7f01b6ec19b2f686bdf277f000000";

static size_t unhex(const char *hex, unsigned char *out, size_t cap) {
    size_t n = 0;
    while (hex[0] && hex[1] && n + 2 <= cap) {
        unsigned hi, lo;
        if (sscanf(hex, "%1x%1x", &hi, &lo) != 2) break;
        out[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

/* Runs the full transcoder over an in-memory input and collects the output. */
static hp_error_t transcode_mem(const char *in, size_t in_len,
                                hp_format_t in_fmt, hp_format_t out_fmt,
                                bool gzip_out,
                                char **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;

    hp_transcoder_config_t cfg = {
        .in_format = in_fmt,
        .out_format = out_fmt,
        .simd_mode = HP_SIMD_SCALAR,
        .gzip_out = gzip_out,
    };
    hp_transcoder_t tc;
    hp_error_t err = hp_transcoder_init(&tc, &cfg);
    if (err != HP_OK) return err;

    FILE *fin = fmemopen((void *)in, in_len, "rb");
    if (!fin) { hp_transcoder_free(&tc); return HP_ERR_IO; }

    size_t dummy = 0;
    char *mem = NULL;
    FILE *fout = open_memstream(&mem, &dummy);
    if (!fout) { fclose(fin); hp_transcoder_free(&tc); return HP_ERR_IO; }

    err = hp_transcoder_run(&tc, fin, fout);

    fclose(fin);
    fclose(fout); /* flushes memstream */
    hp_transcoder_free(&tc);

    if (err != HP_OK) {
        free(mem);
        return err;
    }
    *out_buf = mem;
    *out_len = dummy;
    return HP_OK;
}

static void test_gzip_input(void) {
    unsigned char gz[512];
    size_t gz_len = unhex(FIXTURE_GZ_HEX, gz, sizeof(gz));
    assert(gz_len > 4);
    assert(gz[0] == 0x1F && gz[1] == 0x8B); /* fixture sanity: gzip magic */

    if (!hp_gzip_available()) {
        printf("  [SKIP] libz not present on this system\n");
        return;
    }

    char *out = NULL;
    size_t out_len = 0;
    /* Input format left on AUTO on purpose: the format sniff must see past
     * the gzip wrapper to the CSV payload inside. Output is pinned to CSV
     * so the result must equal the fixture byte for byte (AUTO would pick
     * the documented NDJSON default here). */
    hp_error_t err = transcode_mem((const char *)gz, gz_len,
                                   HP_FORMAT_AUTO, HP_FORMAT_CSV,
                                   false, &out, &out_len);
    assert(err == HP_OK);
    assert(out_len == strlen(FIXTURE_CSV));
    assert(memcmp(out, FIXTURE_CSV, out_len) == 0);
    free(out);
    printf("  [PASS] gzip input: transparent decompression is byte-exact\n");
}

static void test_gzip_output(void) {
    if (!hp_gzip_available()) {
        return; /* already reported by the input test */
    }

    char *gz_out = NULL;
    size_t gz_len = 0;
    hp_error_t err = transcode_mem(FIXTURE_CSV, strlen(FIXTURE_CSV),
                                   HP_FORMAT_CSV, HP_FORMAT_CSV,
                                   true, &gz_out, &gz_len);
    assert(err == HP_OK);
    assert(gz_len > 2);
    assert((unsigned char)gz_out[0] == 0x1F && (unsigned char)gz_out[1] == 0x8B);

    /* Roundtrip our own writer through our own reader */
    char *back = NULL;
    size_t back_len = 0;
    err = transcode_mem(gz_out, gz_len, HP_FORMAT_AUTO, HP_FORMAT_CSV,
                        false, &back, &back_len);
    assert(err == HP_OK);
    assert(back_len == strlen(FIXTURE_CSV));
    assert(memcmp(back, FIXTURE_CSV, back_len) == 0);
    free(gz_out);
    free(back);
    printf("  [PASS] gzip output: writer->reader roundtrip is byte-exact\n");
}

static void test_gzip_multi_member(void) {
    if (!hp_gzip_available()) {
        return;
    }

    unsigned char gz[512];
    size_t gz_len = unhex(FIXTURE_GZ_HEX, gz, sizeof(gz));

    /* Two concatenated members: gzip(1) semantics require decoding both */
    size_t dbl_len = gz_len * 2;
    char *dbl = (char *)malloc(dbl_len);
    assert(dbl);
    memcpy(dbl, gz, gz_len);
    memcpy(dbl + gz_len, gz, gz_len);

    char *out = NULL;
    size_t out_len = 0;
    hp_error_t err = transcode_mem(dbl, dbl_len, HP_FORMAT_AUTO, HP_FORMAT_CSV,
                                   false, &out, &out_len);
    assert(err == HP_OK);

    const char *expected = FIXTURE_CSV;
    size_t once = strlen(expected);
    assert(out_len == once * 2);
    assert(memcmp(out, expected, once) == 0);
    assert(memcmp(out + once, expected, once) == 0);
    free(dbl);
    free(out);
    printf("  [PASS] multi-member gzip: concatenated members decode fully\n");
}

int main(void) {
    printf("[test_gzip] Running gzip integration tests...\n");
    test_gzip_input();
    test_gzip_output();
    test_gzip_multi_member();
    printf("[test_gzip] All gzip tests passed successfully.\n");
    return 0;
}

#else /* _WIN32 */

int main(void) {
    printf("[test_gzip] Skipped: POSIX memstream APIs unavailable.\n");
    return 0;
}

#endif /* _WIN32 */
