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
    /* Parallel workers (hp_parallel.h) only: stop after span_end input bytes
     * and bind preset_header instead of parsing a header row. Both are
     * zero/NULL in ordinary single-stream runs. */
    uint64_t span_end;
    const hp_header_t *preset_header;
    /* Optional output sink. When set, every output page goes to it instead
     * of out_stream (hp_transcoder_run then accepts out_stream == NULL); it
     * returns how many bytes it took, and a short count is an I/O error. */
    size_t (*sink)(void *ctx, const void *data, size_t len);
    void *sink_ctx;
} hp_transcoder_config_t;

typedef struct hp_transcoder_stats_s {
    uint64_t rows_read;
    uint64_t rows_emitted;
    uint64_t rows_dropped;
    uint64_t bytes_in;
    uint64_t bytes_out;
    double elapsed_seconds;
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
    uint64_t span_consumed;       /* input bytes consumed toward config.span_end */
    hp_transcoder_stats_t stats;
    FILE *quarantine_file;
    uint8_t *page_buf_data;
    size_t page_buf_cap;
    hp_gzip_deflate_t *gz_out;    /* output side compression (when enabled) */
} hp_transcoder_t;

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg);
void hp_transcoder_free(hp_transcoder_t *tc);

/* Sniff format from sample bytes */
hp_format_t hp_sniff_format(const uint8_t *sample, size_t len);

/* Run streaming transcoding from in_stream to out_stream */
hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream);

#endif /* HYPERPIPE_HP_TRANSCODER_H */
