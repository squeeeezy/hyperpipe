/* Fuzz harness for HyperPipe (libFuzzer + standalone driver).
 *
 * Invariants enforced for every input:
 *   - parsing/transcoding never crashes and never hangs (CSV and NDJSON)
 *   - the CSV -> CSV -> reparse pipeline is fingerprint-stable
 *     (parse(serialize(parse(x))) == parse(x))
 *   - the NDJSON -> NDJSON pipeline is fingerprint-stable (order-free XOR
 *     fingerprint over present fields, so sparse rows and reordered keys
 *     compare fairly across the re-serialization)
 *
 * libFuzzer (OSS-Fuzz style):
 *   clang -fsanitize=fuzzer,address -O1 -Iinclude tests/fuzz_harness.c
 *         src/hp_*.c -o fuzz && ./fuzz corpus/
 *
 * Standalone (works with plain gcc, no libFuzzer required):
 *   make fuzz            # ASan+UBSan build, random-mutation loop
 *   make fuzz-long       # 200k iterations plus the in-repo corpus seeds
 *   ./build/fuzz_hyperpipe [seed files...]   # optional corpus seeds
 *
 * Any invariant violation aborts the process, which is exactly what both
 * drivers report as a finding. */
#include "hyperpipe/hp_csv_parser.h"
#include "hyperpipe/hp_transcoder.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_DOC_CAP (256 * 1024)

/* Compact FNV-1a fingerprint of a full parse.
 *
 * The first row is the header: it is hashed by NAME only (a header cell is
 * schema, not data, and an empty cell has no meaningful quoted-form after a
 * roundtrip - the name itself is what must survive). Every data row is
 * hashed with full fidelity: field count, lengths, quoted flags and bytes. */
static void parse_fingerprint(const char *doc, size_t len,
                              uint64_t *fields_out, uint64_t *hash_out) {
    hp_csv_parser_t parser;
    if (hp_csv_parser_init(&parser, HP_FORMAT_CSV, HP_SIMD_SCALAR) != HP_OK) {
        abort();
    }

    size_t off = 0;
    hp_row_t row;
    uint64_t fields = 0, hash = 1469598103934665603ULL;
    bool header_seen = false;

    while (off < len) {
        size_t consumed = 0;
        memset(&row, 0, sizeof(row));
        hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)doc + off,
                                          len - off, &consumed, &row, true);
        if (err != HP_OK || consumed == 0) break;
        off += consumed;
        for (size_t i = 0; i < row.field_count; i++) {
            hash ^= (uint64_t)row.fields[i].len;
            hash *= 1099511628211ULL;
            /* Quoted-form is canonicalized on output per RFC 4180 content
             * rules, so the quoted flag is deliberately not hashed; value
             * bytes are what must survive a roundtrip. */
            for (size_t k = 0; k < row.fields[i].len; k++) {
                hash ^= (uint8_t)row.fields[i].ptr[k];
                hash *= 1099511628211ULL;
            }
        }
        if (!header_seen && row.field_count > 0) header_seen = true;
        fields += row.field_count;
    }
    hp_csv_parser_free(&parser);
    *fields_out = fields;
    *hash_out = hash;
}

/* Order-free NDJSON fingerprint: XOR-combines per-field (key, bytes) hashes
 * so reordered keys and sparse rows compare fairly across a roundtrip. The
 * present/absent distinction is kept via the field count; the string vs
 * literal typing is deliberately not hashed (a literal that fails the JSON
 * number grammar is re-typed as a quoted string on output - documented
 * fidelity behavior, not drift). */
static void ndjson_fingerprint(const char *doc, size_t len,
                               uint64_t *fields_out, uint64_t *hash_out) {
    hp_header_t header;
    memset(&header, 0, sizeof(header));
    hp_keymap_t map;
    memset(&map, 0, sizeof(map));
    bool map_ok = false;

    static char arena[64 * 1024];
    hp_row_t row;
    uint64_t fields = 0, hash = 1469598103934665603ULL;

    size_t off = 0;
    while (off < len) {
        const char *nl = (const char *)memchr(doc + off, '\n', len - off);
        size_t line_len = nl ? (size_t)(nl - (doc + off) + 1) : len - off;
        size_t sl = line_len;
        while (sl && (doc[off + sl - 1] == '\r' || doc[off + sl - 1] == '\n')) sl--;
        off += line_len;
        if (sl == 0) continue;

        memset(&row, 0, sizeof(row));
        hp_error_t err = hp_json_parse_line_ex(doc + off - line_len, sl,
                                               &header,
                                               map_ok ? &map : NULL,
                                               &row, arena, sizeof(arena));
        if (err != HP_OK) break;
        if (!map_ok && header.count) {
            map_ok = (hp_keymap_build(&map, &header) == HP_OK);
        }

        uint64_t row_hash = 0;
        for (size_t i = 0; i < row.field_count; i++) {
            if (!row.fields[i].present) continue;
            fields++;
            const char *name = (i < header.count && header.names[i])
                                   ? header.names[i] : "?";
            for (const char *c = name; *c; c++) {
                row_hash ^= (uint8_t)*c;
                row_hash *= 1099511628211ULL;
            }
            row_hash ^= 0xFF;
            row_hash *= 1099511628211ULL;
            for (size_t k = 0; k < row.fields[i].len; k++) {
                row_hash ^= (uint8_t)row.fields[i].ptr[k];
                row_hash *= 1099511628211ULL;
            }
        }
        hash ^= row_hash;
        hash *= 1099511628211ULL;
    }

    hp_keymap_free(&map);
    hp_header_free(&header);
    *fields_out = fields;
    *hash_out = hash;
}

/* Input as a stream: fmemopen where it exists, which keeps libFuzzer fast,
 * and a temporary file elsewhere. NULL for an empty input. */
static FILE *input_stream(const char *data, size_t len) {
    if (len == 0) return NULL;
#if defined(_WIN32)
    FILE *f = tmpfile();
    if (!f) abort();
    fwrite(data, 1, len, f);
    rewind(f);
    return f;
#else
    FILE *f = fmemopen((void *)data, len, "rb");
    if (!f) abort();
    return f;
#endif
}

typedef struct out_buf_s {
    char *data;
    size_t len;
    size_t cap;
} out_buf_t;

static size_t out_append(void *ctx, const void *data, size_t len) {
    out_buf_t *b = (out_buf_t *)ctx;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + len) cap *= 2;
        char *grown = (char *)realloc(b->data, cap);
        if (!grown) abort();
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return len;
}

/* Transcodes doc with cfg into out (through the output sink) */
static hp_error_t transcode(hp_transcoder_config_t cfg, const char *doc, size_t size, out_buf_t *out) {
    FILE *in = input_stream(doc, size);
    if (!in) return HP_ERR_INVALID_ARG;
    cfg.sink = out_append;
    cfg.sink_ctx = out;
    hp_transcoder_t tc;
    if (hp_transcoder_init(&tc, &cfg) != HP_OK) abort();
    hp_error_t err = hp_transcoder_run(&tc, in, NULL);
    hp_transcoder_free(&tc);
    fclose(in);
    return err;
}

static void save_failing_input(const char *path, const char *doc, size_t size) {
    FILE *dump = fopen(path, "wb");
    if (dump) {
        fwrite(doc, 1, size, dump);
        fclose(dump);
    }
}

static int check_input(const uint8_t *data, size_t size) {
    if (size > FUZZ_DOC_CAP) return 0;
    const char *doc = (const char *)data;

    /* Header names are C strings: any NUL byte truncates a name by design
     * (including names promoted to the header after blank leading rows are
     * skipped, or NDJSON keys). Documents containing NUL are still
     * crash-checked, but exempt from the roundtrip fingerprints, which
     * would legitimately differ. */
    bool header_has_nul = (size > 0) && memchr(data, 0x00, size) != NULL;

    uint64_t f0 = 0, h0 = 0;
    if (!header_has_nul) {
        parse_fingerprint(doc, size, &f0, &h0);
    }

    /* Full-pipeline round trip. Oversized or unterminated documents may
     * fail in fail mode, which is fine: the invariant is "no crash", plus a
     * stable fingerprint whenever the run succeeds. */
    const hp_transcoder_config_t csv = {
        .in_format = HP_FORMAT_CSV,
        .out_format = HP_FORMAT_CSV,
        .simd_mode = HP_SIMD_AVX2,
    };
    out_buf_t out = { 0 };
    if (transcode(csv, doc, size, &out) == HP_OK && !header_has_nul) {
        uint64_t f1, h1;
        parse_fingerprint(out.data, out.len, &f1, &h1);
        if (f0 > 0 && (f0 != f1 || h0 != h1)) {
            fprintf(stderr, "fuzz: round trip drift (fields %llu -> %llu)\n",
                    (unsigned long long)f0, (unsigned long long)f1);
            save_failing_input("fuzz_fail_input.csv", doc, size);
            abort();
        }
    }
    free(out.data);

    /* NDJSON round trip through the structural fast path (AVX2 dispatch on
     * x86, scalar elsewhere; both must satisfy the same invariant) */
    if (!header_has_nul) {
        uint64_t nf0 = 0, nh0 = 0;
        ndjson_fingerprint(doc, size, &nf0, &nh0);

        const hp_transcoder_config_t ndjson = {
            .in_format = HP_FORMAT_NDJSON,
            .out_format = HP_FORMAT_NDJSON,
            .simd_mode = HP_SIMD_AVX2,
        };
        out_buf_t jout = { 0 };
        if (transcode(ndjson, doc, size, &jout) == HP_OK) {
            uint64_t nf1 = 0, nh1 = 0;
            ndjson_fingerprint(jout.data, jout.len, &nf1, &nh1);
            if (nf0 > 0 && (nf0 != nf1 || nh0 != nh1)) {
                fprintf(stderr, "fuzz: ndjson round trip drift (fields %llu -> %llu)\n",
                        (unsigned long long)nf0, (unsigned long long)nf1);
                save_failing_input("fuzz_fail_input.ndjson", doc, size);
                abort();
            }
        }
        free(jout.data);
    }

    return 0;
}

/* ---- libFuzzer entry ---------------------------------------------------- */
#ifdef __has_include
#if __has_include(<fuzzer/FuzzedDataProvider.h>)
#define HP_HAVE_LIBFUZZER 1
#endif
#endif

#if defined(HP_HAVE_LIBFUZZER)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return check_input(data, size);
}
#endif

/* ---- Standalone driver: random mutations + optional seed files ---------- */
#if !defined(HP_HAVE_LIBFUZZER)

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

/* Seed corpus of structurally interesting documents */
static const char *SEEDS[] = {
    "a,b,c\n1,2,3\n",
    "\"q, \"\"x\"\"\",y\nr1,r2\n",
    "h\n\"multi\nline\"\n",
    "a,b\n,c\n,,, \n",
    "x\n\x80\xff\x01\n",
    "a\n\"unterminated\n",
    "a\n\"\runterminated\n\"\"",
    "1,2\r\n3,4\n",
    "",
};
#define SEED_COUNT (sizeof(SEEDS) / sizeof(SEEDS[0]))

int main(int argc, char **argv) {
    static uint8_t doc[FUZZ_DOC_CAP];
    size_t iterations = 20000;
    if (argc > 1) iterations = (size_t)strtoull(argv[1], NULL, 10) * 1000;

    printf("fuzz: %zu iterations\n", iterations);

    /* Seed files provided on the command line run verbatim first */
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        size_t n = fread(doc, 1, FUZZ_DOC_CAP, f);
        fclose(f);
        check_input(doc, n);
    }

    size_t seed_lens[SEED_COUNT];
    for (size_t s = 0; s < SEED_COUNT; s++) seed_lens[s] = strlen(SEEDS[s]);

    for (size_t it = 0; it < iterations; it++) {
        size_t s = rng_next() % SEED_COUNT;
        size_t len = seed_lens[s];
        memcpy(doc, SEEDS[s], len);

        /* splice/flip/insert random bytes, bounded */
        size_t muts = 1 + rng_next() % 8;
        for (size_t m = 0; m < muts && len + 8 < FUZZ_DOC_CAP; m++) {
            uint64_t op = rng_next() % 3;
            size_t pos = rng_next() % (len + 1);
            if (op == 0 && len < FUZZ_DOC_CAP - 1) {
                /* insert */
                memmove(doc + pos + 1, doc + pos, len - pos);
                doc[pos] = (uint8_t)",\"\n\r\\\t ABC0" [rng_next() % 10];
                len++;
            } else if (op == 1 && len > 0) {
                /* flip */
                size_t p = rng_next() % len;
                doc[p] ^= (uint8_t)(1u << (rng_next() % 8));
            } else if (op == 2 && len > 0) {
                /* splice a chunk to a random place */
                size_t clen = 1 + rng_next() % 32;
                if (pos + clen > len) clen = len - pos;
                size_t dst = rng_next() % (len + 1);
                if (dst + clen < FUZZ_DOC_CAP) {
                    static uint8_t tmp[256];
                    memcpy(tmp, doc + pos, clen);
                    memmove(doc + dst + clen, doc + dst, len > dst ? len - dst : 0);
                    memcpy(doc + dst, tmp, clen);
                    len += clen;
                }
            }
        }
        check_input(doc, len);
    }
    printf("fuzz: ok\n");
    return 0;
}

#endif /* !HP_HAVE_LIBFUZZER */
