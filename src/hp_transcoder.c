#include "hyperpipe/hp_transcoder.h"
#include <time.h>

#if defined(_WIN32)
    #include <windows.h>
    static double get_time_sec(void) {
        LARGE_INTEGER freq, counter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&counter);
        return (double)counter.QuadPart / (double)freq.QuadPart;
    }
#else
    static double get_time_sec(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    }
#endif

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg) {
    if (!tc || !cfg) return HP_ERR_INVALID_ARG;
    memset(tc, 0, sizeof(*tc));
    tc->config = *cfg;

    size_t cap = cfg->buffer_capacity ? cfg->buffer_capacity : HP_DEFAULT_BUFFER_CAPACITY;
    hp_error_t err = hp_sliding_buffer_init(&tc->ring_buf, cap);
    if (err != HP_OK) return err;

    /* Initialize SIMD runtime dispatch */
    hp_dispatch_init(cfg->simd_mode);

    err = hp_csv_parser_init(&tc->csv_parser, cfg->in_format, cfg->simd_mode);
    if (err != HP_OK) {
        hp_sliding_buffer_free(&tc->ring_buf);
        return err;
    }

    err = hp_json_ser_init(&tc->json_ser, 4096, cfg->simd_mode);
    if (err != HP_OK) {
        hp_sliding_buffer_free(&tc->ring_buf);
        return err;
    }

    /* Configure vectorized escape scanner according to dispatch */
    tc->json_ser.scan_escapes = g_hp_dispatch.json_scan_escapes;

    if (cfg->select_spec) {
        hp_projection_init(&tc->projection, cfg->select_spec);
    }
    if (cfg->where_spec) {
        hp_filter_init(&tc->filter, cfg->where_spec);
    }

    return HP_OK;
}

void hp_transcoder_free(hp_transcoder_t *tc) {
    if (!tc) return;
    hp_sliding_buffer_free(&tc->ring_buf);
    hp_json_ser_free(&tc->json_ser);
    hp_header_free(&tc->in_header);
    hp_header_free(&tc->out_header);
}

static void write_csv_field(FILE *out, const char *p, size_t len, uint8_t delim) {
    bool needs_quotes = false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == delim || p[i] == '"' || p[i] == '\n' || p[i] == '\r') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        fwrite(p, 1, len, out);
    } else {
        fputc('"', out);
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '"') fputc('"', out);
            fputc(p[i], out);
        }
        fputc('"', out);
    }
}

hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream) {
    if (!tc || !in_stream || !out_stream) return HP_ERR_INVALID_ARG;

    double t0 = get_time_sec();
    bool header_processed = false;

    if (tc->config.in_format == HP_FORMAT_CSV || tc->config.in_format == HP_FORMAT_TSV) {
        /* CSV/TSV input stream */
        while (1) {
            size_t bytes_read = 0;
            hp_error_t err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
            if (err != HP_OK) return err;
            tc->stats.bytes_in += bytes_read;

            size_t available = 0;
            const uint8_t *src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            if (available == 0 && tc->ring_buf.eof_reached) break;

            bool is_eof = tc->ring_buf.eof_reached;
            bool made_progress = false;

            while (available > 0) {
                size_t consumed = 0;
                hp_row_t raw_row;

                err = hp_csv_parse_row(&tc->csv_parser, src, available, &consumed, &raw_row, is_eof);
                if (err != HP_OK) return err;

                if (consumed == 0) {
                    /* Incomplete row at buffer tail -> refill buffer */
                    break;
                }

                hp_sliding_buffer_consume(&tc->ring_buf, consumed);
                made_progress = true;

                if (raw_row.field_count == 0) {
                    src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
                    continue;
                }

                if (!header_processed) {
                    /* Store header */
                    tc->in_header.count = 0;
                    for (size_t i = 0; i < raw_row.field_count && i < HP_MAX_COLUMNS; i++) {
                        char buf[256];
                        size_t flen = raw_row.fields[i].len < sizeof(buf) - 1 ? raw_row.fields[i].len : sizeof(buf) - 1;
                        memcpy(buf, raw_row.fields[i].ptr, flen);
                        buf[flen] = '\0';
                        tc->in_header.names[tc->in_header.count++] = strdup(buf);
                    }

                    if (tc->projection.active) {
                        hp_projection_bind(&tc->projection, &tc->in_header);
                        hp_project_header(&tc->projection, &tc->in_header, &tc->out_header);
                    } else {
                        tc->out_header.count = 0;
                        for (size_t i = 0; i < tc->in_header.count; i++) {
                            tc->out_header.names[tc->out_header.count++] = strdup(tc->in_header.names[i]);
                        }
                    }

                    if (tc->filter.count > 0) {
                        hp_filter_bind(&tc->filter, &tc->in_header);
                    }

                    header_processed = true;

                    /* If output format is CSV/TSV, write projected header */
                    if (tc->config.out_format == HP_FORMAT_CSV || tc->config.out_format == HP_FORMAT_TSV) {
                        uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                        for (size_t i = 0; i < tc->out_header.count; i++) {
                            if (i > 0) fputc(out_delim, out_stream);
                            write_csv_field(out_stream, tc->out_header.names[i], strlen(tc->out_header.names[i]), out_delim);
                        }
                        fputc('\n', out_stream);
                    }

                    src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
                    continue;
                }

                tc->stats.rows_read++;

                /* Apply predicate filter */
                if (!hp_filter_matches(&tc->filter, &raw_row)) {
                    src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
                    continue;
                }

                /* Apply column projection */
                hp_row_t out_row;
                if (tc->projection.active) {
                    hp_project_row(&tc->projection, &raw_row, &out_row);
                } else {
                    out_row = raw_row;
                }

                /* Serialize to output format */
                if (tc->config.out_format == HP_FORMAT_NDJSON) {
                    const char *line = NULL;
                    size_t line_len = 0;
                    err = hp_json_ser_row(&tc->json_ser, &tc->out_header, &out_row, &line, &line_len);
                    if (err != HP_OK) return err;

                    fwrite(line, 1, line_len, out_stream);
                    tc->stats.bytes_out += line_len;
                } else {
                    uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    for (size_t i = 0; i < out_row.field_count; i++) {
                        if (i > 0) fputc(out_delim, out_stream);
                        write_csv_field(out_stream, out_row.fields[i].ptr, out_row.fields[i].len, out_delim);
                    }
                    fputc('\n', out_stream);
                }

                tc->stats.rows_emitted++;

                src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            }

            if (!made_progress && is_eof) {
                break;
            }
        }
    } else if (tc->config.in_format == HP_FORMAT_NDJSON) {
        /* NDJSON input stream -> parse line by line */
        char line_buf[64 * 1024];
        char val_arena[64 * 1024];

        while (fgets(line_buf, sizeof(line_buf), in_stream)) {
            size_t llen = strlen(line_buf);
            tc->stats.bytes_in += llen;

            hp_row_t raw_row;
            hp_error_t err = hp_json_parse_line(line_buf, llen, &tc->in_header, &raw_row, val_arena, sizeof(val_arena));
            if (err != HP_OK) continue;

            if (!header_processed) {
                if (tc->projection.active) {
                    hp_projection_bind(&tc->projection, &tc->in_header);
                    hp_project_header(&tc->projection, &tc->in_header, &tc->out_header);
                } else {
                    tc->out_header.count = 0;
                    for (size_t i = 0; i < tc->in_header.count; i++) {
                        tc->out_header.names[tc->out_header.count++] = strdup(tc->in_header.names[i]);
                    }
                }
                if (tc->filter.count > 0) {
                    hp_filter_bind(&tc->filter, &tc->in_header);
                }
                header_processed = true;

                if (tc->config.out_format == HP_FORMAT_CSV || tc->config.out_format == HP_FORMAT_TSV) {
                    uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    for (size_t i = 0; i < tc->out_header.count; i++) {
                        if (i > 0) fputc(out_delim, out_stream);
                        write_csv_field(out_stream, tc->out_header.names[i], strlen(tc->out_header.names[i]), out_delim);
                    }
                    fputc('\n', out_stream);
                }
            }

            tc->stats.rows_read++;
            if (!hp_filter_matches(&tc->filter, &raw_row)) continue;

            hp_row_t out_row;
            if (tc->projection.active) {
                hp_project_row(&tc->projection, &raw_row, &out_row);
            } else {
                out_row = raw_row;
            }

            if (tc->config.out_format == HP_FORMAT_NDJSON) {
                const char *line = NULL;
                size_t line_len = 0;
                err = hp_json_ser_row(&tc->json_ser, &tc->out_header, &out_row, &line, &line_len);
                if (err != HP_OK) return err;
                fwrite(line, 1, line_len, out_stream);
                tc->stats.bytes_out += line_len;
            } else {
                uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                for (size_t i = 0; i < out_row.field_count; i++) {
                    if (i > 0) fputc(out_delim, out_stream);
                    write_csv_field(out_stream, out_row.fields[i].ptr, out_row.fields[i].len, out_delim);
                }
                fputc('\n', out_stream);
            }
            tc->stats.rows_emitted++;
        }
    }

    double t1 = get_time_sec();
    tc->stats.elapsed_seconds = t1 - t0;
    return HP_OK;
}
