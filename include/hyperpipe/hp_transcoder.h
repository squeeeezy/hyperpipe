#ifndef HYPERPIPE_HP_TRANSCODER_H
#define HYPERPIPE_HP_TRANSCODER_H

#include "hp_common.h"
#include "hp_ring_buffer.h"
#include "hp_csv_parser.h"
#include "hp_json_ser.h"
#include "hp_filter.h"
#include "hp_cpuid.h"
#include "hp_keymap.h"
#include "hp_summary.h"
#include "hp_agg.h"
#include "hp_sort.h"
#include "hp_compute.h"

typedef struct hp_transcoder_config_s {
    hp_format_t in_format;
    hp_format_t out_format;
    hp_simd_mode_t simd_mode;
    hp_on_error_t on_error;
    const char *quarantine_path;
    const char *select_spec;
    const char *where_spec;
    size_t buffer_capacity;
    uint64_t limit_rows;   /* stop after N emitted rows (0 = unlimited) */
    bool count_only;       /* --count: suppress row output, report row count */
    bool gzip_out;         /* compress output with gzip (requires libz at runtime) */
    bool summary;          /* --summary: aggregate column statistics, no row output */
    const char *schema_path; /* --schema <file>: declared column types (int hints) */
    /* --group-by and --agg (hp_agg.h): rows are folded into groups and the
     * groups are written once the input ends */
    const char *group_by_spec;
    const char *agg_spec;
    /* --sort (hp_sort.h): rows, or groups, are written in sorted order;
     * with limit_rows only that many are kept. sort_memory bounds the rows
     * held in memory (0: the default). */
    const char *sort_spec;
    size_t sort_memory;
    int sort_threads;      /* threads for the final sort; 0: one per processor */
    /* Parallel workers only: keep the groups or the sorted rows in the
     * transcoder for the coordinator instead of writing them, and number
     * rows for sorting from row_seq_base. */
    bool partial;
    uint64_t row_seq_base;
    /* Parallel workers (hp_parallel.h) only: read exactly span_end input
     * bytes and bind preset_header instead of parsing a header row. Both are
     * zero/NULL in ordinary single-stream runs. */
    uint64_t span_end;
    const hp_header_t *preset_header;
    bool lock_schema;      /* NDJSON: keep the first line's keys, never grow */
    /* Optional output sink. When set, every output page goes to it instead
     * of out_stream (hp_transcoder_run then accepts out_stream == NULL); it
     * returns how many bytes it took, and a short count is an I/O error. */
    size_t (*sink)(void *ctx, const void *data, size_t len);
    void *sink_ctx;
    /* Optional destination for quarantined input, used instead of
     * quarantine_path (parallel workers keep chunk order this way). */
    size_t (*quarantine_sink)(void *ctx, const void *data, size_t len);
    void *quarantine_ctx;
    /* --add (hp_compute.h): computed columns, after the input's */
    const char *compute_spec;
} hp_transcoder_config_t;

typedef struct hp_transcoder_stats_s {
    uint64_t rows_read;
    uint64_t rows_emitted;
    uint64_t rows_dropped;
    uint64_t records_in;      /* input records, header and blank lines included */
    uint64_t bytes_in;
    uint64_t bytes_out;
    double elapsed_seconds;
    /* Where a failed run stopped: the 1-based number of the bad record and
     * the offset of its first byte in the decompressed input, or 0 when the
     * error has no position; the column name of HP_ERR_COLUMN_NOT_FOUND. */
    uint64_t error_record;
    uint64_t error_offset;
    char error_column[128];
} hp_transcoder_stats_t;

typedef struct hp_transcoder_s {
    hp_transcoder_config_t config;
    hp_sliding_buffer_t ring_buf;
    hp_reader_t in_reader;        /* input side: stdio + transparent gzip */
    hp_csv_parser_t csv_parser;
    char *value_arena;            /* decoded NDJSON strings, sized to the line */
    size_t value_arena_cap;
    hp_json_ser_t json_ser;
    hp_projection_t projection;
    hp_filter_t filter;
    hp_header_t in_header;
    hp_header_t out_header;
    hp_keymap_t keymap;           /* O(1) NDJSON key lookup, valid when keymap_active */
    bool keymap_active;
    hp_summary_t *summary;        /* --summary accumulator (heap, one per run) */
    hp_agg_t *agg;                /* --group-by/--agg table (heap) */
    hp_sorter_t *sorter;          /* --sort (heap) */
    hp_transcoder_stats_t stats;
    FILE *quarantine_file;
    uint8_t *page_buf_data;
    size_t page_buf_cap;
    hp_gzip_deflate_t *gz_out;    /* output side compression (when enabled) */
    hp_compute_t *compute;        /* --add and paths into JSON (heap) */
    size_t visible_seen;          /* input columns project_visible() has looked at */
} hp_transcoder_t;

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg);
void hp_transcoder_free(hp_transcoder_t *tc);

/* Sniff format from sample bytes */
hp_format_t hp_sniff_format(const uint8_t *sample, size_t len);

/* Human-readable description of an error code */
const char *hp_strerror(hp_error_t err);

/* Run streaming transcoding from in_stream to out_stream */
hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream);

/* Writes the rows of a finished aggregation in the configured output
 * format; parallel runs use it for the merged table of their workers.
 * in_format must be known, so the default output format can be chosen. */
hp_error_t hp_transcoder_write_groups(hp_transcoder_t *tc, const hp_agg_t *agg, FILE *out_stream);

/* Writes the lines a parallel run's workers sorted, merged in order */
hp_error_t hp_transcoder_write_sorted(hp_transcoder_t *tc, hp_sorter_t *sorter, FILE *out_stream);

#endif /* HYPERPIPE_HP_TRANSCODER_H */
