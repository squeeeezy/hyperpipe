/* gzip tests: transparent input decompression, output compression and
 * multi-member streams.
 *
 * The compressed fixture is embedded as hex so the test is deterministic:
 * it checks the exact decompression of a known gzip stream (zlib with
 * wbits=31, a real gzip container), the writer against the reader, and
 * gzip(1)-style concatenation of two members. */

#include "pipe.h"

static const char *const fixture_csv =
    "host,path,status,bytes\n"
    "alpha.example.com,/index.html,200,5120\n"
    "beta.example.net,/api/v1,404,0\n"
    "\"gamma, inc\",/log?a=1&b=2,200,321\n";

static const char *const fixture_gz_hex =
    "1f8b080000000000020345cc3d0a02311006d03ea7902dac3e92495ccbc5b34c"
    "d66113c81f6494f5f6828dfde3a53e158335612aeb6b227e54a6e132125b39b9"
    "8e2276ef152eb7a79c36692d0844b8fb40268afe551385e391dddb63a5156496"
    "836b655c72db17b8d28f076ffe1ab7f01b6ec19b2f686bdf277f000000";

static size_t unhex(const char *hex, unsigned char *out, size_t cap) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < cap) {
        unsigned hi, lo;
        if (sscanf(hex, "%1x%1x", &hi, &lo) != 2) break;
        out[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static pipe_result_t transcode(const void *in, size_t len, hp_format_t from, bool gzip_out) {
    hp_transcoder_config_t cfg = {
        .in_format = from,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_SCALAR,
        .gzip_out = gzip_out,
    };
    return pipe_run(&cfg, in, len);
}

/* The format sniff must see through the gzip wrapper, so the input format
 * stays on auto. CSV output makes the result comparable to the fixture. */
static void test_input_is_decompressed(void) {
    unsigned char gz[512];
    size_t gz_len = unhex(fixture_gz_hex, gz, sizeof(gz));
    CHECK(gz_len > 4 && gz[0] == 0x1F && gz[1] == 0x8B);

    pipe_result_t r = transcode(gz, gz_len, HP_FORMAT_AUTO, false);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, fixture_csv) == 0);
    pipe_free(&r);
}

static void test_output_reads_back(void) {
    pipe_result_t packed = transcode(fixture_csv, strlen(fixture_csv), HP_FORMAT_CSV, true);
    CHECK(packed.err == HP_OK);
    CHECK(packed.len > 2 && (uint8_t)packed.out[0] == 0x1F && (uint8_t)packed.out[1] == 0x8B);

    pipe_result_t back = transcode(packed.out, packed.len, HP_FORMAT_AUTO, false);
    CHECK(back.err == HP_OK);
    CHECK(strcmp(back.out, fixture_csv) == 0);
    pipe_free(&packed);
    pipe_free(&back);
}

static void test_concatenated_members_decode_fully(void) {
    unsigned char gz[1024];
    size_t gz_len = unhex(fixture_gz_hex, gz, sizeof(gz) / 2);
    memcpy(gz + gz_len, gz, gz_len);

    pipe_result_t r = transcode(gz, 2 * gz_len, HP_FORMAT_AUTO, false);
    CHECK(r.err == HP_OK);
    size_t once = strlen(fixture_csv);
    CHECK(r.len == 2 * once);
    CHECK(memcmp(r.out, fixture_csv, once) == 0 && memcmp(r.out + once, fixture_csv, once) == 0);
    pipe_free(&r);
}

int main(void) {
    if (!hp_gzip_available()) return tap_skip_all("zlib not found");
    TAP_RUN(test_input_is_decompressed);
    TAP_RUN(test_output_reads_back);
    TAP_RUN(test_concatenated_members_decode_fully);
    return tap_done();
}
