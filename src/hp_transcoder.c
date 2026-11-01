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

typedef struct hp_page_buffer_s {
    uint8_t *data;
    size_t cap;
    size_t len;
    FILE *stream;
} hp_page_buffer_t;

HP_INLINE void hp_page_buffer_flush(hp_page_buffer_t *pb) {
    if (pb && pb->len > 0) {
        fwrite(pb->data, 1, pb->len, pb->stream);
        pb->len = 0;
    }
}

HP_INLINE void hp_page_buffer_write(hp_page_buffer_t *pb, const void *src, size_t size) {
    if (size >= pb->cap) {
        hp_page_buffer_flush(pb);
        fwrite(src, 1, size, pb->stream);
        return;
    }
    if (pb->len + size > pb->cap) {
        hp_page_buffer_flush(pb);
    }
    memcpy(pb->data + pb->len, src, size);
    pb->len += size;
}

HP_INLINE void hp_page_buffer_putc(hp_page_buffer_t *pb, uint8_t ch) {
    if (pb->len >= pb->cap) {
        hp_page_buffer_flush(pb);
    }
    pb->data[pb->len++] = ch;
}

static void write_csv_field_pb(hp_page_buffer_t *pb, const char *p, size_t len, uint8_t delim) {
    bool needs_quotes = false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == delim || p[i] == '"' || p[i] == '\n' || p[i] == '\r') {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes) {
        hp_page_buffer_write(pb, p, len);
    } else {
        hp_page_buffer_putc(pb, '"');
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '"') hp_page_buffer_putc(pb, '"');
            hp_page_buffer_putc(pb, (uint8_t)p[i]);
        }
        hp_page_buffer_putc(pb, '"');
    }
}

hp_format_t hp_sniff_format(const uint8_t *sample, size_t len) {
    if (!sample || len == 0) return HP_FORMAT_CSV;

    size_t i = 0;
    if (len >= 3 && sample[0] == 0xEF && sample[1] == 0xBB && sample[2] == 0xBF) {
        i = 3;
    }
    while (i < len && (sample[i] == ' ' || sample[i] == '\t' || sample[i] == '\r' || sample[i] == '\n')) {
        i++;
    }
    if (i >= len) return HP_FORMAT_CSV;

    if (sample[i] == '{') {
        return HP_FORMAT_NDJSON;
    }

    size_t commas = 0;
    size_t tabs = 0;
    for (size_t k = i; k < len && sample[k] != '\n' && sample[k] != '\r'; k++) {
        if (sample[k] == ',') commas++;
        else if (sample[k] == '\t') tabs++;
    }

    if (tabs > commas) {
        return HP_FORMAT_TSV;
    }
    return HP_FORMAT_CSV;
}

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg) {
    if (!tc || !cfg) return HP_ERR_INVALID_ARG;
    memset(tc, 0, sizeof(*tc));
    tc->config = *cfg;

    size_t cap = cfg->buffer_capacity ? cfg->buffer_capacity : HP_DEFAULT_BUFFER_CAPACITY;
    hp_error_t err = hp_sliding_buffer_init(&tc->ring_buf, cap);
    if (err != HP_OK) return err;

    /* Allocate 2 MiB page buffer for high-throughput batch writes */
    tc->page_buf_cap = 2 * 1024 * 1024;
    tc->page_buf_data = (uint8_t *)malloc(tc->page_buf_cap);
    if (!tc->page_buf_data) {
        hp_sliding_buffer_free(&tc->ring_buf);
        return HP_ERR_NOMEM;
    }

    /* Initialize SIMD runtime dispatch */
    hp_dispatch_init(cfg->simd_mode);

    hp_format_t parser_fmt = (cfg->in_format == HP_FORMAT_AUTO) ? HP_FORMAT_CSV : cfg->in_format;
    err = hp_csv_parser_init(&tc->csv_parser, parser_fmt, cfg->simd_mode);
    if (err != HP_OK) {
        hp_sliding_buffer_free(&tc->ring_buf);
        free(tc->page_buf_data);
        tc->page_buf_data = NULL;
        return err;
    }

    err = hp_json_ser_init(&tc->json_ser, 4096, cfg->simd_mode);
    if (err != HP_OK) {
        hp_sliding_buffer_free(&tc->ring_buf);
        free(tc->page_buf_data);
        tc->page_buf_data = NULL;
        return err;
    }

    tc->json_ser.scan_escapes = g_hp_dispatch.json_scan_escapes;

    if (cfg->select_spec) {
        hp_projection_init(&tc->projection, cfg->select_spec);
    }
    if (cfg->where_spec) {
        hp_filter_init(&tc->filter, cfg->where_spec);
    }

    if (cfg->on_error == HP_ON_ERROR_QUARANTINE && cfg->quarantine_path) {
        tc->quarantine_file = fopen(cfg->quarantine_path, "wb");
        if (!tc->quarantine_file) {
            hp_sliding_buffer_free(&tc->ring_buf);
            hp_json_ser_free(&tc->json_ser);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return HP_ERR_IO;
        }
    }

    return HP_OK;
}

void hp_transcoder_free(hp_transcoder_t *tc) {
    if (!tc) return;
    hp_sliding_buffer_free(&tc->ring_buf);
    hp_json_ser_free(&tc->json_ser);
    hp_header_free(&tc->in_header);
    hp_header_free(&tc->out_header);
    if (tc->page_buf_data) {
        free(tc->page_buf_data);
        tc->page_buf_data = NULL;
    }
    if (tc->quarantine_file) {
        fclose(tc->quarantine_file);
        tc->quarantine_file = NULL;
    }
}

hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream) {
    if (!tc || !in_stream || !out_stream) return HP_ERR_INVALID_ARG;

    double t0 = get_time_sec();
    bool header_processed = false;

    hp_page_buffer_t pb;
    pb.data = tc->page_buf_data;
    pb.cap = tc->page_buf_cap;
    pb.len = 0;
    pb.stream = out_stream;

    /* Prime ring buffer */
    size_t bytes_read = 0;
    hp_error_t err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
    if (err != HP_OK) return err;
    tc->stats.bytes_in += bytes_read;

    size_t available = 0;
    const uint8_t *src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);

    /* Sniff format from buffered data */
    if (tc->config.in_format == HP_FORMAT_AUTO) {
        tc->config.in_format = hp_sniff_format(src, available);
        if (tc->config.in_format == HP_FORMAT_TSV) {
            tc->csv_parser.delim = '\t';
        }
    }

    if (tc->config.out_format == HP_FORMAT_AUTO) {
        if (tc->config.in_format == HP_FORMAT_NDJSON) {
            tc->config.out_format = HP_FORMAT_CSV;
        } else {
            tc->config.out_format = HP_FORMAT_NDJSON;
        }
    }

    if (tc->config.in_format == HP_FORMAT_CSV || tc->config.in_format == HP_FORMAT_TSV) {
        /* CSV/TSV input stream */
        while (1) {
            src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            if (available == 0) {
                if (tc->ring_buf.eof_reached) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
                if (err != HP_OK) {
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
                src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
                if (available == 0 && tc->ring_buf.eof_reached) break;
            }

            bool is_eof = tc->ring_buf.eof_reached;
            bool made_progress = false;

            while (available > 0) {
                size_t consumed = 0;
                hp_row_t raw_row;

                err = hp_csv_parse_row(&tc->csv_parser, src, available, &consumed, &raw_row, is_eof);
                if (err != HP_OK) {
                    if (tc->config.on_error == HP_ON_ERROR_FAIL) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    /* Forward sync to next unescaped newline */
                    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                    size_t skip = nl ? (size_t)(nl - src + 1) : available;
                    if (tc->quarantine_file) {
                        fwrite(src, 1, skip, tc->quarantine_file);
                    }
                    hp_sliding_buffer_consume(&tc->ring_buf, skip);
                    tc->stats.rows_dropped++;
                    made_progress = true;
                    src += skip;
                    available -= skip;
                    continue;
                }

                if (consumed == 0) {
                    /* Incomplete row at buffer tail -> refill buffer */
                    break;
                }

                hp_sliding_buffer_consume(&tc->ring_buf, consumed);
                made_progress = true;

                if (raw_row.field_count == 0) {
                    src += consumed;
                    available -= consumed;
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
                            if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                            write_csv_field_pb(&pb, tc->out_header.names[i], strlen(tc->out_header.names[i]), out_delim);
                        }
                        hp_page_buffer_putc(&pb, '\n');
                    }

                    src += consumed;
                    available -= consumed;
                    continue;
                }

                tc->stats.rows_read++;

                /* Apply predicate filter */
                if (!hp_filter_matches(&tc->filter, &raw_row)) {
                    src += consumed;
                    available -= consumed;
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
                    if (err != HP_OK) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }

                    hp_page_buffer_write(&pb, line, line_len);
                    tc->stats.bytes_out += line_len;
                } else {
                    uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    for (size_t i = 0; i < out_row.field_count; i++) {
                        if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                        write_csv_field_pb(&pb, out_row.fields[i].ptr, out_row.fields[i].len, out_delim);
                    }
                    hp_page_buffer_putc(&pb, '\n');
                }

                tc->stats.rows_emitted++;
                src += consumed;
                available -= consumed;
            }

            if (!made_progress) {
                if (is_eof) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
                if (err != HP_OK) {
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
            }
        }
    } else if (tc->config.in_format == HP_FORMAT_NDJSON) {
        char val_arena[64 * 1024];
        char line_buf[64 * 1024];

        while (1) {
            src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            if (available == 0) {
                if (tc->ring_buf.eof_reached) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
                if (err != HP_OK) {
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
                src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
                if (available == 0 && tc->ring_buf.eof_reached) break;
            }

            bool is_eof = tc->ring_buf.eof_reached;
            bool made_progress = false;

            while (available > 0) {
                const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                if (!nl && !is_eof) {
                    break;
                }

                size_t line_len = nl ? (size_t)(nl - src + 1) : available;
                if (line_len >= sizeof(line_buf)) {
                    hp_page_buffer_flush(&pb);
                    return HP_ERR_BUFFER_OVERFLOW;
                }

                memcpy(line_buf, src, line_len);
                line_buf[line_len] = '\0';

                hp_sliding_buffer_consume(&tc->ring_buf, line_len);
                made_progress = true;

                /* Strip trailing CR/LF for parsing */
                size_t strip_len = line_len;
                while (strip_len > 0 && (line_buf[strip_len - 1] == '\r' || line_buf[strip_len - 1] == '\n' || isspace((unsigned char)line_buf[strip_len - 1]))) {
                    line_buf[--strip_len] = '\0';
                }

                if (strip_len == 0) {
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                hp_row_t raw_row;
                err = hp_json_parse_line(line_buf, strip_len, &tc->in_header, &raw_row, val_arena, sizeof(val_arena));
                if (err != HP_OK) {
                    if (tc->config.on_error == HP_ON_ERROR_FAIL) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    if (tc->quarantine_file) {
                        fwrite(line_buf, 1, line_len, tc->quarantine_file);
                    }
                    tc->stats.rows_dropped++;
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                if (!header_processed && tc->in_header.count > 0) {
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
                            if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                            write_csv_field_pb(&pb, tc->out_header.names[i], strlen(tc->out_header.names[i]), out_delim);
                        }
                        hp_page_buffer_putc(&pb, '\n');
                    }
                }

                tc->stats.rows_read++;

                if (!hp_filter_matches(&tc->filter, &raw_row)) {
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                hp_row_t out_row;
                if (tc->projection.active) {
                    hp_project_row(&tc->projection, &raw_row, &out_row);
                } else {
                    out_row = raw_row;
                }

                if (tc->config.out_format == HP_FORMAT_NDJSON) {
                    const char *line = NULL;
                    size_t out_line_len = 0;
                    err = hp_json_ser_row(&tc->json_ser, &tc->out_header, &out_row, &line, &out_line_len);
                    if (err != HP_OK) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    hp_page_buffer_write(&pb, line, out_line_len);
                    tc->stats.bytes_out += out_line_len;
                } else {
                    uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    for (size_t i = 0; i < out_row.field_count; i++) {
                        if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                        write_csv_field_pb(&pb, out_row.fields[i].ptr, out_row.fields[i].len, out_delim);
                    }
                    hp_page_buffer_putc(&pb, '\n');
                }
                tc->stats.rows_emitted++;
                src += line_len;
                available -= line_len;
            }

            if (!made_progress) {
                if (is_eof) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, in_stream, &bytes_read);
                if (err != HP_OK) {
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
            }
        }
    }

    hp_page_buffer_flush(&pb);
    double t1 = get_time_sec();
    tc->stats.elapsed_seconds = t1 - t0;
    return HP_OK;
}
