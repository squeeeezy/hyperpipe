/* test_parallel: --jobs pipeline must reproduce the single-threaded byte
 * stream exactly, across input formats (CSV/TSV/NDJSON), output formats,
 * filters, projections, quoted and multi-line records, ragged rows,
 * reordered/sparse NDJSON keys and fault-tolerance modes. */

#include "hyperpipe/hp_parallel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  [PASS] %s\n", msg); \
        } else { \
            printf("  [FAIL] %s\n", msg); \
            failures++; \
        } \
    } while (0)

/* Writes a deterministic test CSV with the adversarial shapes a parallel
 * split must handle: quoted commas, doubled quotes, embedded newlines,
 * ragged rows and a header whose width differs from some data rows. */
static void write_test_csv(const char *path, int rows, int variant) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs("id,name,payload,flag\n", f);
    for (int i = 0; i < rows; i++) {
        switch ((i + variant) % 5) {
            case 0:  /* embedded newline inside a quoted field */
                fprintf(f, "%d,\"Smith, John\",\"line1\nline2\",true\n", i);
                break;
            case 1:  /* doubled quotes */
                fprintf(f, "%d,plain,\"say \"\"hi\"\"\",false\n", i);
                break;
            case 2:  /* ragged row: one extra trailing column */
                fprintf(f, "%d,extra,\"a,b\",true,overhang\n", i);
                break;
            case 3:  /* quoted numeric-looking value keeps string type */
                fprintf(f, "%d,\"%d\",\"%d00\",false\n", i, i, i);
                break;
            default: /* clean row */
                fprintf(f, "%d,row%d,%d,true\n", i, i, i * 7);
                break;
        }
    }
    /* Last record intentionally without a trailing newline: the final
     * worker must still emit it at EOF. */
    fprintf(f, "%d,last,\"tail, quoted\",true", rows);
    fclose(f);
}

/* Runs one configuration once through the single-threaded path and once
 * through the parallel pipeline, capturing both outputs, and compares them
 * byte for byte. */
static void run_case(hp_format_t in_fmt, const char *path,
                     const char *select_spec, const char *where_spec,
                     hp_format_t out_fmt, hp_on_error_t on_error) {
    hp_transcoder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.in_format = in_fmt;
    cfg.out_format = out_fmt;
    cfg.select_spec = select_spec;
    cfg.where_spec = where_spec;
    cfg.on_error = on_error;

    /* single-threaded reference */
    char *ref = (char *)malloc(16u << 20);
    size_t ref_len = 0;
    FILE *in = fopen(path, "rb");
    FILE *out = fmemopen(ref, 16u << 20, "w");

    hp_transcoder_t tc;
    hp_error_t err = hp_transcoder_init(&tc, &cfg);
    if (err == HP_OK) err = hp_transcoder_run(&tc, in, out);
    hp_transcoder_free(&tc);
    ref_len = ftello(out);
    fclose(out);
    fclose(in);
    CHECK(err == HP_OK, "single-threaded run completed");

    /* parallel pipeline */
    char *par = (char *)malloc(16u << 20);
    size_t par_len = 0;
    in = fopen(path, "rb");
    out = fmemopen(par, 16u << 20, "w");

    hp_transcoder_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    err = hp_parallel_run(&cfg, path, out, 3, &stats);
    par_len = ftello(out);
    fclose(out);
    fclose(in);
    CHECK(err == HP_OK, "parallel run completed");

    bool same = (ref_len == par_len) && (memcmp(ref, par, ref_len) == 0);
    CHECK(same, "parallel output is byte-identical to single-threaded");
    if (!same) {
        printf("         ref_len=%zu par_len=%zu\n", ref_len, par_len);
        size_t k = 0;
        while (k < ref_len && k < par_len && ref[k] == par[k]) k++;
        printf("         first difference at byte %zu\n", k);
    }
    if (stats.rows_read == 0 && ref_len > 0) {
        CHECK(false, "parallel stats report rows");
    }

    free(ref);
    free(par);
}

/* Writes a deterministic NDJSON dataset with the shapes a parallel NDJSON
 * split must handle: escaped strings, sparse rows (absent keys), reordered
 * keys and string values that look like numbers. The schema stays stable:
 * parallel NDJSON locks the schema to the first line, mirroring the
 * documented CSV-output rule (see the growth case below). */
static void write_test_ndjson(const char *path, int rows, int variant) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    /* First line carries the complete key set: it defines the schema that
     * every worker binds as its preset, so no key may appear later that
     * the first line did not have (see the growth case below for the
     * locked-schema policy). */
    fprintf(f, "{\"id\":0,\"name\":\"n0\",\"payload\":\"p0\",\"flag\":true}\n");
    for (int i = 1; i < rows; i++) {
        switch ((i + variant) % 4) {
            case 0:  /* escaped string value takes the general parser path */
                fprintf(f, "{\"id\":%d,\"name\":\"row %d\",\"payload\":\"line\\n2\",\"flag\":true}\n", i, i);
                break;
            case 1:  /* sparse row: flag absent */
                fprintf(f, "{\"id\":%d,\"payload\":\"p%d\"}\n", i, i);
                break;
            case 2:  /* reordered keys resolve through the keymap */
                fprintf(f, "{\"flag\":false,\"payload\":\"q\\\"%d\",\"name\":\"n%d\",\"id\":%d}\n", i, i, i);
                break;
            default: /* string that looks like a number keeps string type */
                fprintf(f, "{\"id\":\"%d\",\"name\":\"n%d\",\"payload\":\"%d00\",\"flag\":false}\n", i, i, i);
                break;
        }
    }
    /* Last record intentionally without a trailing newline. */
    fprintf(f, "{\"id\":%d,\"name\":\"last\",\"payload\":\"tail\",\"flag\":true}", rows);
    fclose(f);
}

int main(void) {
    const char *path = "build/test_parallel_input.csv";
    write_test_csv(path, 20003, 0);

    printf("[test_parallel] plain full conversion, NDJSON output\n");
    run_case(HP_FORMAT_CSV, path, NULL, NULL, HP_FORMAT_AUTO, HP_ON_ERROR_FAIL);

    printf("[test_parallel] full conversion, CSV output (header only from worker 0)\n");
    run_case(HP_FORMAT_CSV, path, NULL, NULL, HP_FORMAT_CSV, HP_ON_ERROR_FAIL);

    printf("[test_parallel] filter + projection\n");
    run_case(HP_FORMAT_CSV, path, "name,payload", "id >= 100 and id < 19000",
             HP_FORMAT_AUTO, HP_ON_ERROR_FAIL);

    printf("[test_parallel] skip fault tolerance over ragged rows\n");
    run_case(HP_FORMAT_CSV, path, NULL, NULL, HP_FORMAT_AUTO, HP_ON_ERROR_SKIP);

    printf("[test_parallel] count mode\n");
    {
        hp_transcoder_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.in_format = HP_FORMAT_CSV;
        cfg.out_format = HP_FORMAT_AUTO;
        cfg.where_spec = "id >= 500";
        cfg.count_only = true;

        char sink[64];
        FILE *out = fmemopen(sink, sizeof(sink), "w");
        hp_transcoder_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        hp_error_t err = hp_parallel_run(&cfg, path, out, 4, &stats);
        fclose(out);
        CHECK(err == HP_OK, "parallel count run completed");
        /* ids 500..20002 from the loop plus the trailing id=20003 record */
        CHECK(stats.rows_emitted == (20003 - 500) + 1,
              "parallel count matches expected matching rows");
    }

    /* A second dataset with a different row variant offset exercises chunk
     * boundaries that land in different record shapes. */
    printf("[test_parallel] second variant (different boundary shapes)\n");
    write_test_csv(path, 20011, 3);
    run_case(HP_FORMAT_CSV, path, NULL, NULL, HP_FORMAT_AUTO, HP_ON_ERROR_FAIL);

    /* ---- NDJSON input ---- */

    const char *ndjson_path = "build/test_parallel_input.ndjson";
    write_test_ndjson(ndjson_path, 20003, 1);

    printf("[test_parallel] ndjson full conversion, CSV output\n");
    run_case(HP_FORMAT_NDJSON, ndjson_path, NULL, NULL, HP_FORMAT_CSV,
             HP_ON_ERROR_FAIL);

    printf("[test_parallel] ndjson full conversion, NDJSON output\n");
    run_case(HP_FORMAT_NDJSON, ndjson_path, NULL, NULL, HP_FORMAT_NDJSON,
             HP_ON_ERROR_FAIL);

    printf("[test_parallel] ndjson filter + projection\n");
    run_case(HP_FORMAT_NDJSON, ndjson_path, "id,payload", "id >= 100 and id < 19000",
             HP_FORMAT_AUTO, HP_ON_ERROR_FAIL);

    printf("[test_parallel] ndjson count mode\n");
    {
        hp_transcoder_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.in_format = HP_FORMAT_NDJSON;
        cfg.out_format = HP_FORMAT_NDJSON;
        cfg.where_spec = "id >= 500";
        cfg.count_only = true;

        /* Reference count from the single-threaded path: the invariant
         * under test is parallel == single, not a re-derivation of the
         * filter's mixed-type semantics. */
        uint64_t ref_count = 0;
        char sink[64];
        FILE *out = fmemopen(sink, sizeof(sink), "w");
        hp_transcoder_t tc;
        hp_error_t err = hp_transcoder_init(&tc, &cfg);
        if (err == HP_OK) {
            FILE *in = fopen(ndjson_path, "rb");
            err = hp_transcoder_run(&tc, in, out);
            fclose(in);
            ref_count = tc.stats.rows_emitted;
            hp_transcoder_free(&tc);
        }
        fclose(out);
        CHECK(err == HP_OK && ref_count > 0, "single-threaded ndjson count completed");

        out = fmemopen(sink, sizeof(sink), "w");
        hp_transcoder_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        err = hp_parallel_run(&cfg, ndjson_path, out, 4, &stats);
        fclose(out);
        CHECK(err == HP_OK, "parallel ndjson count run completed");
        CHECK(stats.rows_emitted == ref_count,
              "parallel ndjson count matches single-threaded count");
    }

    printf("[test_parallel] ndjson schema growth follows the documented lock\n");
    {
        /* Rows 0..2999 carry {id,v}; rows 3000.. carry an extra key.
         * Single-threaded output follows the growth; the parallel pipeline
         * locks the schema to the first line (same rule as CSV output), so
         * the late key serializes under its column index. */
        const char *grow_path = "build/test_parallel_growth.ndjson";
        FILE *f = fopen(grow_path, "wb");
        for (int i = 0; i < 4000; i++) {
            if (i < 3000) fprintf(f, "{\"id\":%d,\"v\":%d}\n", i, i);
            else          fprintf(f, "{\"id\":%d,\"v\":%d,\"extra\":%d}\n", i, i, i);
        }
        fclose(f);

        hp_transcoder_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.in_format = HP_FORMAT_NDJSON;
        cfg.out_format = HP_FORMAT_NDJSON;

        char *par = (char *)malloc(1u << 20);
        FILE *out = fmemopen(par, 1u << 20, "w");
        hp_transcoder_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        hp_error_t err = hp_parallel_run(&cfg, grow_path, out, 4, &stats);
        fclose(out);
        CHECK(err == HP_OK, "growth run completed");
        CHECK(stats.rows_emitted == 4000, "growth run emitted every row");
        CHECK(strstr(par, "\"col_2\":3000") != NULL,
              "late key serialized under the locked schema");
        free(par);
        remove(grow_path);
    }

    printf("[test_parallel] compressed input reports unsupported (single-thread fallback)\n");
    {
        const char *gz_path = "build/test_parallel_fake.gz";
        FILE *f = fopen(gz_path, "wb");
        if (f) {
            const uint8_t magic[4] = {0x1F, 0x8B, 0x08, 0x00};
            fwrite(magic, 1, sizeof(magic), f);
            fputs("payload", f);
            fclose(f);
        }
        hp_transcoder_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.in_format = HP_FORMAT_CSV;
        cfg.out_format = HP_FORMAT_AUTO;
        FILE *out = fopen("/dev/null", "wb");
        hp_transcoder_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        hp_error_t err = hp_parallel_run(&cfg, gz_path, out, 4, &stats);
        fclose(out);
        remove(gz_path);
        CHECK(err == HP_ERR_UNSUPPORTED, "compressed input reports HP_ERR_UNSUPPORTED");
    }

    remove(ndjson_path);
    remove(path);

    if (failures == 0) {
        printf("test_parallel: ok\n");
        return 0;
    }
    printf("test_parallel: %d failure(s)\n", failures);
    return 1;
}
