#ifndef HYPERPIPE_HP_TRANSCODER_H
#define HYPERPIPE_HP_TRANSCODER_H

#include "hp_common.h"
#include "hp_ring_buffer.h"
#include "hp_csv_parser.h"
#include "hp_json_ser.h"
#include "hp_filter.h"
#include "hp_cpuid.h"

typedef struct hp_transcoder_config_s {
    hp_format_t in_format;
    hp_format_t out_format;
    hp_simd_mode_t simd_mode;
    const char *select_spec;
    const char *where_spec;
    size_t buffer_capacity;
} hp_transcoder_config_t;

typedef struct hp_transcoder_stats_s {
    uint64_t rows_read;
    uint64_t rows_emitted;
    uint64_t bytes_in;
    uint64_t bytes_out;
    double elapsed_seconds;
} hp_transcoder_stats_t;

typedef struct hp_transcoder_s {
    hp_transcoder_config_t config;
    hp_sliding_buffer_t ring_buf;
    hp_csv_parser_t csv_parser;
    hp_json_ser_t json_ser;
    hp_projection_t projection;
    hp_filter_t filter;
    hp_header_t in_header;
    hp_header_t out_header;
    hp_transcoder_stats_t stats;
} hp_transcoder_t;

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg);
void hp_transcoder_free(hp_transcoder_t *tc);

/* Run streaming transcoding from in_stream to out_stream */
hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream);

#endif /* HYPERPIPE_HP_TRANSCODER_H */
