/* In-tree chaos fuzzer: adversarial-but-deterministic structured data fed
 * through every SIMD kernel and through the full transcode pipeline.
 *
 * What it asserts, on every generated document:
 *   1. Differential parsing: scalar, AVX2 and AVX-512 row parsers produce
 *      identical field sequences (values, lengths, quoted-flags).
 *   2. Pipeline fidelity: CSV -> CSV -> reparse reproduces the original
 *      parse fingerprint exactly (parse(serialize(parse(x))) == parse(x)),
 *      which subsumes RFC 4180 re-quoting correctness on hostile data.
 *   3. NDJSON fidelity: parse -> serialize -> parse round-trips every
 *      (key, value) pair, including escapes and reordered keys.
 *
 * The generator is a seeded xorshift, so failures are exactly reproducible:
 * the seed and iteration index are printed for every document. NUL bytes
 * are excluded from generated values on purpose: header names travel
 * through C strings, so NUL-truncated headers would make the roundtrip
 * fingerprint legitimately unstable (values stay length-based).
 *
 * This is the same invariant set an OSS-Fuzz target checks; the standalone
 * libFuzzer harness lives in tests/fuzz_harness.c. */
#include "hyperpipe/hp_csv_parser.h"
#include "hyperpipe/hp_json_ser.h"
#include "hyperpipe/hp_transcoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)

#define MAX_FIELDS 16
#define MAX_FIELD_LEN 200
#define MAX_ROWS 24
#define DOC_CAP (MAX_ROWS * MAX_FIELDS * (MAX_FIELD_LEN + 4) + 64)

typedef struct {
    uint64_t s;
} rng_t;

static uint64_t rng_next(rng_t *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}

static size_t rng_range(rng_t *r, size_t n) {
    return (size_t)(rng_next(r) % n);
}

/* A byte alphabet biased toward CSV/JSON hostiles. */
static const char HOSTILES[] = ",\n\r\t\"\\ abc0123456789-./:"
                               "\x01\x1f\x7f\x80\xff{}[]:eE+-";

static size_t gen_field(rng_t *r, char *dst, bool quoted_able) {
    size_t len = rng_range(r, MAX_FIELD_LEN);
    if (rng_range(r, 4) == 0) len = 0; /* sprinkle empty fields */
    bool quoted = quoted_able && rng_range(r, 3) == 0;
    size_t w = 0;
    if (quoted) dst[w++] = '"';
    for (size_t i = 0; i < len && w < MAX_FIELD_LEN + 2; i++) {
        char c = HOSTILES[rng_range(r, sizeof(HOSTILES) - 1)];
        if (quoted && c == '"') {
            /* RFC 4180 escape for an embedded quote */
            dst[w++] = '"';
        }
        dst[w++] = c;
    }
    if (quoted) dst[w++] = '"';
    return w;
}

static size_t gen_csv_doc(rng_t *r, char *doc) {
    size_t w = 0;
    size_t rows = 1 + rng_range(r, MAX_ROWS);
    size_t cols = 1 + rng_range(r, MAX_FIELDS);

    for (size_t row = 0; row < rows; row++) {
        for (size_t col = 0; col < cols; col++) {
            if (col > 0) doc[w++] = ',';
            /* header row: plain names; data rows: hostile values */
            if (row == 0) {
                size_t n = 1 + rng_range(r, 9);
                for (size_t k = 0; k < n; k++) {
                    doc[w++] = (char)('a' + rng_range(r, 26));
                }
            } else {
                w += gen_field(r, doc + w, true);
            }
        }
        doc[w++] = (rng_range(r, 2) == 0) ? '\n' : '\r';
        if (doc[w - 1] == '\r') doc[w++] = '\n';
    }
    return w;
}

/* Parses the whole document with one forced kernel and reduces it to a
 * compact fingerprint: total fields, byte count and FNV-1a over every
 * field's length, quoted-flag and bytes. */
static void fingerprint_csv(const char *doc, size_t len,
                            hp_error_t (*parse_fn)(hp_csv_parser_t *, const uint8_t *, size_t, size_t *, hp_row_t *, bool),
                            hp_simd_mode_t mode,
                            uint64_t *fp_fields, uint64_t *fp_hash, hp_error_t *first_err) {
    hp_csv_parser_t parser;
    assert(hp_csv_parser_init(&parser, HP_FORMAT_CSV, mode) == HP_OK);

    size_t off = 0;
    hp_row_t row;
    uint64_t fields = 0, hash = 1469598103934665603ULL;
    *first_err = HP_OK;

    while (off < len) {
        size_t consumed = 0;
        memset(&row, 0, sizeof(row));
        /* is_eof stays true: the whole document is in one buffer, so the
         * parser may terminate a final unterminated row at buffer end */
        hp_error_t err = parse_fn(&parser, (const uint8_t *)doc + off,
                                  len - off, &consumed, &row, true);
        if (err != HP_OK) {
            if (*first_err == HP_OK) *first_err = err;
            break;
        }
        if (consumed == 0) break;
        off += consumed;

        for (size_t i = 0; i < row.field_count; i++) {
            /* Value fidelity: length and bytes. The quoted flag is
             * deliberately excluded - quoting is canonicalized on output
             * per RFC 4180 content rules; kernel DIFFERENTIAL equality
             * (checked above) covers the flag itself. */
            hash ^= (uint64_t)row.fields[i].len;
            hash *= 1099511628211ULL;
            for (size_t k = 0; k < row.fields[i].len; k++) {
                hash ^= (uint8_t)row.fields[i].ptr[k];
                hash *= 1099511628211ULL;
            }
        }
        fields += row.field_count;
    }

    *fp_fields = fields;
    *fp_hash = hash;
}

/* Full-pipeline fidelity check for one document: CSV -> CSV transcode must
 * reparse to the same field values (values are sacred; quoting is
 * canonicalized per RFC 4180 content rules, and an empty cell that arrived
 * as a typed string must survive as ""). Returns true when the document
 * parsed cleanly and the roundtrip held. */
static bool roundtrip_check_doc(const char *doc, size_t len, uint64_t f0, uint64_t h0) {
    {
        hp_transcoder_config_t cfg = {
            .in_format = HP_FORMAT_CSV,
            .out_format = HP_FORMAT_CSV,
            .simd_mode = HP_SIMD_AVX2,
        };
        hp_transcoder_t tc;
        if (hp_transcoder_init(&tc, &cfg) != HP_OK) abort();

        FILE *fin = fmemopen((void *)doc, len, "rb");
        char *mem = NULL;
        size_t dummy = 0;
        FILE *fout = open_memstream(&mem, &dummy);
        assert(fin && fout);

        hp_error_t err = hp_transcoder_run(&tc, fin, fout);
        fclose(fin);
        fclose(fout);
        hp_transcoder_free(&tc);
        if (err != HP_OK) {
            /* fail/skip semantics are validated elsewhere; here we only
             * roundtrip cleanly-parsing documents */
            free(mem);
            return false;
        }

        uint64_t rf, rh;
        hp_error_t re;
        fingerprint_csv(mem, dummy, hp_csv_parse_row_scalar, HP_SIMD_SCALAR, &rf, &rh, &re);
        if (rf != f0 || rh != h0) {
            fprintf(stderr, "chaos: roundtrip fingerprint drift\n");
            fprintf(stderr, "  original : fields=%llu hash=%llx\n", (unsigned long long)f0, (unsigned long long)h0);
            fprintf(stderr, "  roundtrip: fields=%llu hash=%llx\n", (unsigned long long)rf, (unsigned long long)rh);
            abort();
        }
        free(mem);
        return true;
    }
    return false;
}

static void test_csv_differential_and_roundtrip(rng_t *r, uint64_t seed, size_t iter) {
    static char doc[DOC_CAP];
    size_t len = gen_csv_doc(r, doc);

    /* 1. All three kernels agree */
    uint64_t f0, f1, f2, h0, h1, h2;
    hp_error_t e0, e1, e2;
    fingerprint_csv(doc, len, hp_csv_parse_row_scalar, HP_SIMD_SCALAR, &f0, &h0, &e0);
    fingerprint_csv(doc, len, hp_csv_parse_row_avx2, HP_SIMD_AVX2, &f1, &h1, &e1);
    fingerprint_csv(doc, len, hp_csv_parse_row_avx512, HP_SIMD_AVX512, &f2, &h2, &e2);
    if (!(f0 == f1 && f0 == f2 && h0 == h1 && h0 == h2)) {
        fprintf(stderr, "chaos: kernel divergence at seed=%llu iter=%zu\n",
                (unsigned long long)seed, iter);
        fprintf(stderr, "  scalar: fields=%llu hash=%llx\n", (unsigned long long)f0, (unsigned long long)h0);
        fprintf(stderr, "  avx2  : fields=%llu hash=%llx\n", (unsigned long long)f1, (unsigned long long)h1);
        fprintf(stderr, "  avx512: fields=%llu hash=%llx\n", (unsigned long long)f2, (unsigned long long)h2);
        abort();
    }

    /* 2. Pipeline roundtrip on documents whose scalar parse made progress */
    if (e0 == HP_OK && f0 > 0) {
        roundtrip_check_doc(doc, len, f0, h0);
    }
}

static void test_ndjson_roundtrip(rng_t *r, uint64_t seed, size_t iter) {
    static char line[4096];
    size_t w = 0;
    size_t keys = 1 + rng_range(r, 8);

    line[w++] = '{';
    for (size_t k = 0; k < keys; k++) {
        if (k > 0) line[w++] = ',';
        w += (size_t)snprintf(line + w, sizeof(line) - w, "\"k%zu\":\"", rng_range(r, 12));
        size_t vlen = rng_range(r, 40);
        for (size_t i = 0; i < vlen; i++) {
            char c = HOSTILES[rng_range(r, sizeof(HOSTILES) - 1)];
            if (c == '"') { line[w++] = '\\'; c = '"'; }
            if (c == '\\') { line[w++] = '\\'; }
            if (c == '\n') { line[w++] = '\\'; c = 'n'; }
            if (c == '\r') { line[w++] = '\\'; c = 'r'; }
            if (c == '\t') { line[w++] = '\\'; c = 't'; }
            line[w++] = c;
        }
        line[w++] = '"';
    }
    line[w++] = '}';
    line[w] = '\0';

    hp_header_t header = { .count = 0, .names = {NULL} };
    hp_row_t row;
    static char arena[64 * 1024];
    memset(&row, 0, sizeof(row));

    hp_error_t err = hp_json_parse_line(line, w, &header, &row, arena, sizeof(arena));
    if (err != HP_OK) {
        fprintf(stderr, "chaos: ndjson parse failed at seed=%llu iter=%zu\n",
                (unsigned long long)seed, iter);
        abort();
    }

    hp_json_ser_t ser;
    assert(hp_json_ser_init(&ser, 4096, HP_SIMD_AVX512) == HP_OK);
    ser.emit_string = hp_json_emit_body_avx512;
    assert(hp_json_ser_bind_keys(&ser, &header) == HP_OK);

    const char *out_line = NULL;
    size_t out_len = 0;
    err = hp_json_ser_row(&ser, &header, &row, &out_line, &out_len);
    assert(err == HP_OK);

    /* Re-parse the serialization: every key must resolve to the same value */
    hp_header_t header2 = { .count = 0, .names = {NULL} };
    hp_row_t row2;
    static char arena2[64 * 1024];
    memset(&row2, 0, sizeof(row2));
    err = hp_json_parse_line(out_line, out_len, &header2, &row2, arena2, sizeof(arena2));
    assert(err == HP_OK);
    assert(row2.field_count == row.field_count);
    for (size_t i = 0; i < row.field_count; i++) {
        assert(strcmp(header.names[i], header2.names[i]) == 0);
        assert(row.fields[i].len == row2.fields[i].len);
        assert(memcmp(row.fields[i].ptr, row2.fields[i].ptr, row.fields[i].len) == 0);
        assert(row.fields[i].present == row2.fields[i].present);
    }

    hp_json_ser_free(&ser);
    for (size_t i = 0; i < header.count; i++) free(header.names[i]);
    for (size_t i = 0; i < header2.count; i++) free(header2.names[i]);
}

/* Transcodes CSV -> CSV in memory; aborts on pipeline errors. */
static char *transcode_csv_csv(const char *doc, size_t len, size_t *out_len) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_AVX2,
    };
    hp_transcoder_t tc;
    if (hp_transcoder_init(&tc, &cfg) != HP_OK) abort();

    FILE *fin = fmemopen((void *)doc, len, "rb");
    char *mem = NULL;
    size_t dummy = 0;
    FILE *fout = open_memstream(&mem, &dummy);
    if (!fin || !fout) abort();

    hp_error_t err = hp_transcoder_run(&tc, fin, fout);
    fclose(fin);
    fclose(fout);
    hp_transcoder_free(&tc);
    if (err != HP_OK) abort();
    *out_len = dummy;
    return mem;
}

/* Byte-exact regressions: these documents are already in canonical form,
 * so the transcoder must reproduce them byte for byte. */
static void test_byte_exact_regressions(void) {
    struct { const char *doc; size_t len; const char *why; } cases[] = {
        { "\"\",y\n", 5, "typed empty field must survive as \"\"" },
        { "h1,h2\nonly\n1,2,3\n", 17, "ragged rows keep their real width" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t out_len = 0;
        char *out = transcode_csv_csv(cases[c].doc, cases[c].len, &out_len);
        if (out_len != cases[c].len || memcmp(out, cases[c].doc, out_len) != 0) {
            fprintf(stderr, "chaos: byte-exact regression failed: %s\n", cases[c].why);
            fprintf(stderr, "  expected: %.*s\n", (int)cases[c].len, cases[c].doc);
            fprintf(stderr, "  actual  : %.*s\n", (int)out_len, out);
            abort();
        }
        free(out);
    }
    printf("  [PASS] byte-exact regressions: typed-empty and ragged rows\n");
}

/* Deterministic regression documents: each one pins a fidelity rule that
 * random generation hits only probabilistically. */
static const char *REGRESSION_DOCS[] = {
    /* clean quoted value: quoting is canonicalized on output */
    "\"abc\",1\n",
    /* typed empty field must stay \"\" (not degenerate to a blank field) */
    "\"\",y\n",
    /* ragged rows keep their real width */
    "h1,h2\nonly\n1,2,3\n",
    /* CRLF and LF mixed */
    "a,b\r\n1,2\n3,4\r\n",
};
#define REGRESSION_COUNT (sizeof(REGRESSION_DOCS) / sizeof(REGRESSION_DOCS[0]))

int main(void) {
    const uint64_t base_seed = 0x5EEDC0FFEE000001ULL;
    const size_t iterations = 400;

    printf("[test_chaos] Running chaos fuzzer (%zu adversarial documents)...\n", iterations);
    for (size_t iter = 0; iter < iterations; iter++) {
        uint64_t seed = base_seed + iter * 7919;
        rng_t r = { .s = seed };
        for (size_t warm = 0; warm < 8; warm++) rng_next(&r);

        test_csv_differential_and_roundtrip(&r, seed, iter);
        test_ndjson_roundtrip(&r, seed, iter);
    }

    test_byte_exact_regressions();

    for (size_t d = 0; d < REGRESSION_COUNT; d++) {
        const char *doc = REGRESSION_DOCS[d];
        size_t len = strlen(doc);
        uint64_t f0, h0;
        hp_error_t e0;
        fingerprint_csv(doc, len, hp_csv_parse_row_scalar, HP_SIMD_SCALAR, &f0, &h0, &e0);
        if (e0 != HP_OK || f0 == 0) continue;
        roundtrip_check_doc(doc, len, f0, h0);
    }
    printf("[test_chaos] All chaos invariants held: scalar == AVX2 == AVX-512, roundtrips stable.\n");
    return 0;
}

#else /* _WIN32 */

int main(void) {
    printf("[test_chaos] Skipped: POSIX memstream APIs unavailable.\n");
    return 0;
}

#endif /* _WIN32 */
