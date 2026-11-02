/* test_parallel: the --jobs pipeline must reproduce the single-threaded byte
 * stream exactly, across input formats (CSV/NDJSON), output formats,
 * filters, projections, quoted and multi-line records, ragged rows,
 * reordered/sparse NDJSON keys and fault-tolerance modes. */

#include "hyperpipe/hp_parallel.h"
#include "pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const csv_path = "build/test_parallel_input.csv";
static const char *const ndjson_path = "build/test_parallel_input.ndjson";
static const int job_counts[] = { 2, 3, 7 };

/* Large enough for several 1 MiB ranges, so chunk boundaries land on every
 * record shape the writers produce. */
static const int csv_rows = 250003;
static const int ndjson_rows = 150003;

/* Writes a deterministic CSV with the shapes a parallel split must handle:
 * quoted commas, doubled quotes, embedded newlines, ragged rows and a header
 * whose width differs from some data rows. */
static void write_test_csv(const char *path, int rows, int variant) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
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
    /* The last record has no trailing newline: the final chunk must still
     * emit it at end of file. */
    fprintf(f, "%d,last,\"tail, quoted\",true", rows);
    fclose(f);
}

/* Writes a deterministic NDJSON file with escaped strings, sparse rows,
 * reordered keys and number-like strings. The first line carries the full
 * key set, which parallel runs lock in as the schema. */
static void write_test_ndjson(const char *path, int rows, int variant) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
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
    fprintf(f, "{\"id\":%d,\"name\":\"last\",\"payload\":\"tail\",\"flag\":true}", rows);
    fclose(f);
}

static pipe_result_t run_single(const hp_transcoder_config_t *cfg, const char *path) {
    pipe_result_t c = { 0 };
    FILE *in = fopen(path, "rb");
    FILE *out = tmpfile();
    CHECK(in && out);
    hp_transcoder_t tc;
    c.err = hp_transcoder_init(&tc, cfg);
    if (c.err == HP_OK) {
        c.err = hp_transcoder_run(&tc, in, out);
        c.stats = tc.stats;
        hp_transcoder_free(&tc);
    }
    fclose(in);
    c.out = pipe_slurp(out, &c.len);
    return c;
}

static pipe_result_t run_parallel(const hp_transcoder_config_t *cfg, const char *path, int jobs) {
    pipe_result_t c = { 0 };
    FILE *out = tmpfile();
    CHECK(out);
    c.err = hp_parallel_run(cfg, path, out, jobs, &c.stats);
    c.out = pipe_slurp(out, &c.len);
    return c;
}

/* Every job count must produce the single-threaded bytes exactly. */
static void check_identical(const hp_transcoder_config_t *cfg, const char *path) {
    pipe_result_t ref = run_single(cfg, path);
    CHECK(ref.err == HP_OK);
    for (size_t j = 0; j < sizeof(job_counts) / sizeof(job_counts[0]); j++) {
        pipe_result_t par = run_parallel(cfg, path, job_counts[j]);
        CHECK(par.err == HP_OK);
        if (par.len != ref.len || memcmp(par.out, ref.out, ref.len) != 0) {
            size_t k = 0;
            while (k < ref.len && k < par.len && ref.out[k] == par.out[k]) k++;
            fprintf(stderr, "jobs=%d: %zu vs %zu bytes, first difference at %zu\n",
                    job_counts[j], par.len, ref.len, k);
        }
        CHECK(par.len == ref.len && memcmp(par.out, ref.out, ref.len) == 0);
        CHECK(par.stats.rows_emitted == ref.stats.rows_emitted);
        pipe_free(&par);
    }
    pipe_free(&ref);
}

static hp_transcoder_config_t config(hp_format_t in, hp_format_t out) {
    hp_transcoder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.in_format = in;
    cfg.out_format = out;
    return cfg;
}

static void test_csv_to_ndjson(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    check_identical(&cfg, csv_path);
}

static void test_csv_to_csv(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_CSV);
    check_identical(&cfg, csv_path);
}

static void test_csv_filter_and_projection(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    cfg.select_spec = "name,payload";
    cfg.where_spec = "id >= 100 and id < 140000";
    check_identical(&cfg, csv_path);
}

/* Skip and quarantine resynchronize at the next line after a stray quote,
 * which quote-parity cuts cannot reproduce: CSV keeps the sequential path. */
static void test_csv_skip_mode_runs_sequentially(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    cfg.on_error = HP_ON_ERROR_SKIP;
    pipe_result_t par = run_parallel(&cfg, csv_path, 4);
    CHECK(par.err == HP_ERR_UNSUPPORTED);
    pipe_free(&par);
}

/* TSV fields may be quoted too, newlines included */
static void test_tsv_with_quoted_newlines(void) {
    const char *tsv_path = "build/test_parallel_input.tsv";
    FILE *f = fopen(tsv_path, "wb");
    CHECK(f);
    fputs("id\tnote\n", f);
    for (int i = 0; i < 200000; i++) {
        if (i % 3 == 0) fprintf(f, "%d\t\"two\nlines\t%d\"\n", i, i);
        else            fprintf(f, "%d\tplain %d\n", i, i);
    }
    fclose(f);
    hp_transcoder_config_t cfg = config(HP_FORMAT_TSV, HP_FORMAT_NDJSON);
    check_identical(&cfg, tsv_path);
    remove(tsv_path);
}

/* Malformed NDJSON lines are quarantined in input order, as sequentially */
static void test_ndjson_quarantine(void) {
    const char *dirty_path = "build/test_parallel_dirty.ndjson";
    const char *q_single = "build/test_parallel_q1.txt";
    const char *q_parallel = "build/test_parallel_q2.txt";
    FILE *f = fopen(dirty_path, "wb");
    CHECK(f);
    for (int i = 0; i < 150000; i++) {
        if (i % 997 == 5) fprintf(f, "{\"id\":%d,\"v\"\n", i);
        else              fprintf(f, "{\"id\":%d,\"v\":\"x%d\"}\n", i, i);
    }
    fclose(f);

    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_CSV);
    cfg.on_error = HP_ON_ERROR_QUARANTINE;
    cfg.quarantine_path = q_single;
    pipe_result_t ref = run_single(&cfg, dirty_path);
    cfg.quarantine_path = q_parallel;
    pipe_result_t par = run_parallel(&cfg, dirty_path, 5);
    CHECK(ref.err == HP_OK && par.err == HP_OK);
    CHECK(ref.stats.rows_dropped > 100);
    CHECK(par.stats.rows_dropped == ref.stats.rows_dropped);
    CHECK(par.len == ref.len && memcmp(par.out, ref.out, ref.len) == 0);

    FILE *a = fopen(q_single, "rb"), *b = fopen(q_parallel, "rb");
    CHECK(a && b);
    int ca, cb;
    do {
        ca = fgetc(a);
        cb = fgetc(b);
        CHECK(ca == cb);
    } while (ca != EOF);
    fclose(a);
    fclose(b);

    pipe_free(&ref);
    pipe_free(&par);
    remove(dirty_path);
    remove(q_single);
    remove(q_parallel);
}

static void test_csv_count(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    cfg.where_spec = "id >= 500";
    cfg.count_only = true;
    pipe_result_t par = run_parallel(&cfg, csv_path, 4);
    CHECK(par.err == HP_OK);
    /* ids 500..csv_rows-1 from the loop plus the trailing id=csv_rows record */
    CHECK(par.stats.rows_emitted == (uint64_t)(csv_rows - 500) + 1);
    pipe_free(&par);
}

/* A different row offset moves the chunk boundaries onto other shapes. */
static void test_csv_other_boundaries(void) {
    write_test_csv(csv_path, csv_rows + 8, 3);
    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    check_identical(&cfg, csv_path);
}

static void test_ndjson_to_csv(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_CSV);
    check_identical(&cfg, ndjson_path);
}

static void test_ndjson_to_ndjson(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_NDJSON);
    check_identical(&cfg, ndjson_path);
}

static void test_ndjson_filter_and_projection(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_AUTO);
    cfg.select_spec = "id,payload";
    cfg.where_spec = "id >= 100 and id < 140000";
    check_identical(&cfg, ndjson_path);
}

static void test_ndjson_count(void) {
    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_NDJSON);
    cfg.where_spec = "id >= 500";
    cfg.count_only = true;
    pipe_result_t ref = run_single(&cfg, ndjson_path);
    pipe_result_t par = run_parallel(&cfg, ndjson_path, 4);
    CHECK(ref.err == HP_OK && par.err == HP_OK);
    CHECK(ref.stats.rows_emitted > 0);
    CHECK(par.stats.rows_emitted == ref.stats.rows_emitted);
    pipe_free(&ref);
    pipe_free(&par);
}

/* Rows 0..2999 carry {id,v}; later rows add a key. The single-threaded run
 * follows the growth; the parallel run locks the schema to the first line,
 * so the late key serializes under its column index. */
static void test_ndjson_schema_lock(void) {
    const char *grow_path = "build/test_parallel_growth.ndjson";
    FILE *f = fopen(grow_path, "wb");
    CHECK(f);
    for (int i = 0; i < 4000; i++) {
        if (i < 3000) fprintf(f, "{\"id\":%d,\"v\":%d}\n", i, i);
        else          fprintf(f, "{\"id\":%d,\"v\":%d,\"extra\":%d}\n", i, i, i);
    }
    fclose(f);

    hp_transcoder_config_t cfg = config(HP_FORMAT_NDJSON, HP_FORMAT_NDJSON);
    pipe_result_t par = run_parallel(&cfg, grow_path, 4);
    CHECK(par.err == HP_OK);
    CHECK(par.stats.rows_emitted == 4000);
    CHECK(strstr(par.out, "\"col_2\":3000") != NULL);
    pipe_free(&par);
    remove(grow_path);
}

static void test_compressed_input_is_unsupported(void) {
    const char *gz_path = "build/test_parallel_fake.gz";
    FILE *f = fopen(gz_path, "wb");
    CHECK(f);
    const uint8_t magic[4] = { 0x1F, 0x8B, 0x08, 0x00 };
    fwrite(magic, 1, sizeof(magic), f);
    fputs("payload", f);
    fclose(f);

    hp_transcoder_config_t cfg = config(HP_FORMAT_CSV, HP_FORMAT_AUTO);
    pipe_result_t par = run_parallel(&cfg, gz_path, 4);
    CHECK(par.err == HP_ERR_UNSUPPORTED);
    pipe_free(&par);
    remove(gz_path);
}

int main(void) {
    write_test_csv(csv_path, csv_rows, 0);
    write_test_ndjson(ndjson_path, ndjson_rows, 1);

    TAP_RUN(test_csv_to_ndjson);
    TAP_RUN(test_csv_to_csv);
    TAP_RUN(test_csv_filter_and_projection);
    TAP_RUN(test_csv_skip_mode_runs_sequentially);
    TAP_RUN(test_tsv_with_quoted_newlines);
    TAP_RUN(test_csv_count);
    TAP_RUN(test_csv_other_boundaries);
    TAP_RUN(test_ndjson_to_csv);
    TAP_RUN(test_ndjson_to_ndjson);
    TAP_RUN(test_ndjson_filter_and_projection);
    TAP_RUN(test_ndjson_count);
    TAP_RUN(test_ndjson_schema_lock);
    TAP_RUN(test_ndjson_quarantine);
    TAP_RUN(test_compressed_input_is_unsupported);

    remove(csv_path);
    remove(ndjson_path);
    return tap_done();
}
