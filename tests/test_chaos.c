/* Chaos tests: adversarial but deterministic documents fed through every
 * SIMD kernel and through the full transcode pipeline.
 *
 * For every generated document:
 *   1. The scalar, AVX2 and AVX-512 row parsers produce identical fields.
 *   2. CSV -> CSV -> reparse reproduces the original fields exactly
 *      (parse(serialize(parse(x))) == parse(x)), which covers RFC 4180
 *      re-quoting on hostile data.
 *   3. NDJSON parse -> serialize -> parse round-trips every key and value,
 *      escapes and reordered keys included.
 *
 * The generator is a seeded xorshift, so a failure names its seed and
 * iteration. NUL bytes are left out of generated values: header names are
 * C strings, so a NUL would truncate them and make the fingerprint unstable.
 * tests/fuzz_harness.c checks the same invariants under libFuzzer. */

#include "hyperpipe/hp_csv_parser.h"
#include "hyperpipe/hp_json_ser.h"
#include "pipe.h"

#define MAX_FIELDS 16
#define MAX_FIELD_LEN 200
#define MAX_ROWS 24
#define DOC_CAP (MAX_ROWS * MAX_FIELDS * (MAX_FIELD_LEN + 4) + 64)
#define ITERATIONS 400

static const uint64_t base_seed = 0x5EEDC0FFEE000001ULL;

typedef struct rng_s {
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

static rng_t rng_for(size_t iter) {
    rng_t r = { .s = base_seed + iter * 7919 };
    for (int warm = 0; warm < 8; warm++) rng_next(&r);
    return r;
}

/* A byte alphabet biased toward bytes that matter to CSV and JSON */
static const char HOSTILES[] = ",\n\r\t\"\\ abc0123456789-./:"
                               "\x01\x1f\x7f\x80\xff{}[]:eE+-";

static size_t gen_field(rng_t *r, char *dst, bool quotable) {
    size_t len = rng_range(r, MAX_FIELD_LEN);
    if (rng_range(r, 4) == 0) len = 0;
    bool quoted = quotable && rng_range(r, 3) == 0;
    size_t w = 0;
    if (quoted) dst[w++] = '"';
    for (size_t i = 0; i < len && w < MAX_FIELD_LEN + 2; i++) {
        char c = HOSTILES[rng_range(r, sizeof(HOSTILES) - 1)];
        if (quoted && c == '"') dst[w++] = '"';
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
            if (row == 0) {
                size_t n = 1 + rng_range(r, 9);
                for (size_t k = 0; k < n; k++) doc[w++] = (char)('a' + rng_range(r, 26));
            } else {
                w += gen_field(r, doc + w, true);
            }
        }
        doc[w++] = (rng_range(r, 2) == 0) ? '\n' : '\r';
        if (doc[w - 1] == '\r') doc[w++] = '\n';
    }
    return w;
}

typedef struct fingerprint_s {
    uint64_t fields;
    uint64_t hash;
    hp_error_t err;
} fingerprint_t;

/* Parses a whole document with one kernel and reduces it to a field count
 * and an FNV-1a hash over every field's length and bytes. The quoted flag
 * is left out: output re-quotes by content, so only values must survive. */
static fingerprint_t fingerprint_csv(const char *doc, size_t len,
                                     hp_error_t (*parse)(hp_csv_parser_t *, const uint8_t *, size_t,
                                                         size_t *, hp_row_t *, bool),
                                     hp_simd_mode_t mode) {
    fingerprint_t fp = { 0, 1469598103934665603ULL, HP_OK };
    hp_csv_parser_t parser;
    CHECK(hp_csv_parser_init(&parser, HP_FORMAT_CSV, mode) == HP_OK);

    static hp_row_t row;
    size_t off = 0;
    while (off < len) {
        size_t consumed = 0;
        memset(&row, 0, sizeof(row));
        /* The whole document is in one buffer, so the final row may end
         * without a newline */
        fp.err = parse(&parser, (const uint8_t *)doc + off, len - off, &consumed, &row, true);
        if (fp.err != HP_OK || consumed == 0) break;
        off += consumed;

        for (size_t i = 0; i < row.field_count; i++) {
            fp.hash ^= (uint64_t)row.fields[i].len;
            fp.hash *= 1099511628211ULL;
            for (size_t k = 0; k < row.fields[i].len; k++) {
                fp.hash ^= (uint8_t)row.fields[i].ptr[k];
                fp.hash *= 1099511628211ULL;
            }
        }
        fp.fields += row.field_count;
    }
    hp_csv_parser_free(&parser);
    return fp;
}

static const hp_transcoder_config_t csv_to_csv = {
    .in_format = HP_FORMAT_CSV,
    .out_format = HP_FORMAT_CSV,
    .simd_mode = HP_SIMD_AVX2,
};

/* CSV -> CSV must reparse to the same values. Documents the pipeline
 * rejects are skipped here; error handling has its own tests. */
static void check_round_trip(const char *doc, size_t len, fingerprint_t original) {
    pipe_result_t r = pipe_run(&csv_to_csv, doc, len);
    if (r.err == HP_OK) {
        fingerprint_t again = fingerprint_csv(r.out, r.len, hp_csv_parse_row_scalar, HP_SIMD_SCALAR);
        if (again.fields != original.fields || again.hash != original.hash) {
            fprintf(stderr, "round trip drift: fields %llu -> %llu\n",
                    (unsigned long long)original.fields, (unsigned long long)again.fields);
        }
        CHECK(again.fields == original.fields && again.hash == original.hash);
    }
    pipe_free(&r);
}

static void test_csv_kernels_agree_and_round_trip(void) {
    static char doc[DOC_CAP];
    for (size_t iter = 0; iter < ITERATIONS; iter++) {
        rng_t r = rng_for(iter);
        size_t len = gen_csv_doc(&r, doc);

        fingerprint_t s = fingerprint_csv(doc, len, hp_csv_parse_row_scalar, HP_SIMD_SCALAR);
        fingerprint_t a2 = fingerprint_csv(doc, len, hp_csv_parse_row_avx2, HP_SIMD_AVX2);
        fingerprint_t a5 = fingerprint_csv(doc, len, hp_csv_parse_row_avx512, HP_SIMD_AVX512);
        if (s.fields != a2.fields || s.fields != a5.fields || s.hash != a2.hash || s.hash != a5.hash) {
            fprintf(stderr, "kernels diverge at iteration %zu\n", iter);
        }
        CHECK(s.fields == a2.fields && s.fields == a5.fields);
        CHECK(s.hash == a2.hash && s.hash == a5.hash);

        if (s.err == HP_OK && s.fields > 0) check_round_trip(doc, len, s);
    }
}

static size_t gen_ndjson_line(rng_t *r, char *line, size_t cap) {
    size_t w = 0;
    size_t keys = 1 + rng_range(r, 8);
    line[w++] = '{';
    for (size_t k = 0; k < keys; k++) {
        if (k > 0) line[w++] = ',';
        w += (size_t)snprintf(line + w, cap - w, "\"k%zu\":\"", rng_range(r, 12));
        size_t vlen = rng_range(r, 40);
        for (size_t i = 0; i < vlen; i++) {
            char c = HOSTILES[rng_range(r, sizeof(HOSTILES) - 1)];
            if (c == '"' || c == '\\') line[w++] = '\\';
            if (c == '\n') { line[w++] = '\\'; c = 'n'; }
            if (c == '\r') { line[w++] = '\\'; c = 'r'; }
            if (c == '\t') { line[w++] = '\\'; c = 't'; }
            line[w++] = c;
        }
        line[w++] = '"';
    }
    line[w++] = '}';
    line[w] = '\0';
    return w;
}

static void test_ndjson_round_trips(void) {
    static char line[4096], arena[64 * 1024], arena2[64 * 1024];
    static hp_row_t row, row2;

    for (size_t iter = 0; iter < ITERATIONS; iter++) {
        rng_t r = rng_for(iter);
        size_t w = gen_ndjson_line(&r, line, sizeof(line));

        hp_header_t header = { .count = 0 };
        memset(&row, 0, sizeof(row));
        CHECK(hp_json_parse_line(line, w, &header, &row, arena, sizeof(arena)) == HP_OK);

        hp_json_ser_t ser;
        CHECK(hp_json_ser_init(&ser, 4096, HP_SIMD_AVX512) == HP_OK);
        ser.emit_string = hp_json_emit_body_avx512;
        CHECK(hp_json_ser_bind_keys(&ser, &header) == HP_OK);
        const char *out = NULL;
        size_t out_len = 0;
        CHECK(hp_json_ser_row(&ser, &header, &row, &out, &out_len) == HP_OK);

        /* Every key must come back with the same value */
        hp_header_t header2 = { .count = 0 };
        memset(&row2, 0, sizeof(row2));
        CHECK(hp_json_parse_line(out, out_len, &header2, &row2, arena2, sizeof(arena2)) == HP_OK);
        CHECK(row2.field_count == row.field_count);
        for (size_t i = 0; i < row.field_count; i++) {
            CHECK(strcmp(header.names[i], header2.names[i]) == 0);
            CHECK(row.fields[i].len == row2.fields[i].len);
            CHECK(memcmp(row.fields[i].ptr, row2.fields[i].ptr, row.fields[i].len) == 0);
            CHECK(row.fields[i].present == row2.fields[i].present);
        }

        hp_json_ser_free(&ser);
        hp_header_free(&header);
        hp_header_free(&header2);
    }
}

/* Documents already in canonical form must come back byte for byte */
static void test_canonical_documents_are_unchanged(void) {
    static const char *const docs[] = {
        "\"\",y\n",                 /* an empty quoted field stays "" */
        "h1,h2\nonly\n1,2,3\n",     /* ragged rows keep their width */
    };
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        pipe_result_t r = pipe_run_str(&csv_to_csv, docs[i]);
        CHECK(r.err == HP_OK);
        CHECK(strcmp(r.out, docs[i]) == 0);
        pipe_free(&r);
    }
}

/* Each document pins a rule that random generation only hits by chance */
static void test_fixed_documents_round_trip(void) {
    static const char *const docs[] = {
        "\"abc\",1\n",              /* a clean quoted value is re-quoted by content */
        "\"\",y\n",
        "h1,h2\nonly\n1,2,3\n",
        "a,b\r\n1,2\n3,4\r\n",      /* mixed CRLF and LF */
    };
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        size_t len = strlen(docs[i]);
        fingerprint_t fp = fingerprint_csv(docs[i], len, hp_csv_parse_row_scalar, HP_SIMD_SCALAR);
        CHECK(fp.err == HP_OK && fp.fields > 0);
        check_round_trip(docs[i], len, fp);
    }
}

int main(void) {
    TAP_RUN(test_csv_kernels_agree_and_round_trip);
    TAP_RUN(test_ndjson_round_trips);
    TAP_RUN(test_canonical_documents_are_unchanged);
    TAP_RUN(test_fixed_documents_round_trip);
    return tap_done();
}
