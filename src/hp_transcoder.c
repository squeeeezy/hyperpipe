#include "hyperpipe/hp_transcoder.h"
#include <time.h>
#include <ctype.h>

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
    hp_gzip_deflate_t *gz;   /* non-NULL: flush compresses through zlib */
    hp_error_t error;        /* first I/O or compression error, if any */
} hp_page_buffer_t;

HP_INLINE void hp_page_buffer_flush(hp_page_buffer_t *pb) {
    if (pb && pb->len > 0) {
        if (pb->gz) {
            const unsigned char *out = NULL;
            size_t out_len = 0;
            hp_error_t err = hp_gzip_deflate_push(pb->gz, pb->data, pb->len, &out, &out_len);
            if (err != HP_OK) {
                if (pb->error == HP_OK) pb->error = err;
            } else if (out_len > 0) {
                fwrite(out, 1, out_len, pb->stream);
            }
        } else {
            fwrite(pb->data, 1, pb->len, pb->stream);
        }
        pb->len = 0;
    }
}

/* Flushes any pending bytes and, for gzip output, writes the stream trailer.
 * Returns the first deferred error, if any. */
static hp_error_t hp_page_buffer_finish(hp_page_buffer_t *pb) {
    hp_page_buffer_flush(pb);
    if (pb->gz && pb->error == HP_OK) {
        const unsigned char *out = NULL;
        size_t out_len = 0;
        hp_error_t err = hp_gzip_deflate_finish(pb->gz, &out, &out_len);
        if (err != HP_OK) {
            pb->error = err;
        } else if (out_len > 0) {
            fwrite(out, 1, out_len, pb->stream);
        }
    }
    if (pb->error == HP_OK && ferror(pb->stream)) {
        pb->error = HP_ERR_IO;
    }
    return pb->error;
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

/* Serializes one NDJSON row straight into the page buffer. The common case
 * emits with zero intermediate copies (the row is written directly at the
 * page-buffer write cursor); only rows whose worst-case escaped size does
 * not fit the remaining page space fall back to a flush-retry or to the
 * growable serializer buffer. */
static hp_error_t emit_ndjson_row_pb(hp_page_buffer_t *pb,
                                     hp_json_ser_t *ser,
                                     const hp_header_t *header,
                                     const hp_row_t *row,
                                     size_t *out_line_len) {
    size_t line_len = 0;
    if (hp_json_ser_row_direct(ser, header, row, (char *)pb->data + pb->len,
                               pb->cap - pb->len, &line_len) == HP_OK) {
        pb->len += line_len;
        *out_line_len = line_len;
        return HP_OK;
    }

    /* Page nearly full: flush and retry with the whole empty page */
    hp_page_buffer_flush(pb);
    if (hp_json_ser_row_direct(ser, header, row, (char *)pb->data, pb->cap,
                               &line_len) == HP_OK) {
        pb->len = line_len;
        *out_line_len = line_len;
        return HP_OK;
    }

    /* Pathological row larger than one page: growable-buffer fallback */
    const char *line = NULL;
    hp_error_t err = hp_json_ser_row(ser, header, row, &line, &line_len);
    if (err != HP_OK) return err;
    hp_page_buffer_write(pb, line, line_len);
    *out_line_len = line_len;
    return HP_OK;
}

/* Writes one CSV field, quoting it whenever the value contains the
 * delimiter, a quote or a line break. type_quoted preserves the string
 * identity of zero-length values: an empty field that arrived as a typed
 * string ("" in the source or "" in NDJSON) must be written back as "",
 * otherwise it degenerates to a blank field and loses its type. Non-empty
 * values keep the classic content-driven rule, which preserves byte
 * identity for data that never needed quoting. */
static void write_csv_field_pb(hp_page_buffer_t *pb, const char *p, size_t len,
                               uint8_t delim, bool type_quoted) {
    bool needs_quotes = (type_quoted && len == 0);
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

    /* Output page buffer: flushed with one fwrite per fill. 256 KiB keeps
     * the working set inside L2 while amortizing write syscalls; measured
     * flat vs 64 KiB on the reference host, better on slower storage. */
    tc->page_buf_cap = 256 * 1024;
    tc->page_buf_data = (uint8_t *)malloc(tc->page_buf_cap);
    if (!tc->page_buf_data) {
        hp_sliding_buffer_free(&tc->ring_buf);
        return HP_ERR_NOMEM;
    }

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
    tc->json_ser.emit_string = g_hp_dispatch.json_emit_string;

    /* Output gzip: resolve libz up front so a missing library fails fast
     * with a dedicated error code instead of mid-stream. */
    if (cfg->gzip_out) {
        hp_gzip_load();
        if (!hp_gzip_available()) {
            hp_sliding_buffer_free(&tc->ring_buf);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return HP_ERR_GZIP_UNAVAILABLE;
        }
        tc->gz_out = (hp_gzip_deflate_t *)malloc(sizeof(hp_gzip_deflate_t));
        if (!tc->gz_out) {
            hp_sliding_buffer_free(&tc->ring_buf);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return HP_ERR_NOMEM;
        }
        hp_error_t gerr = hp_gzip_deflate_start(tc->gz_out);
        if (gerr != HP_OK) {
            free(tc->gz_out);
            tc->gz_out = NULL;
            hp_sliding_buffer_free(&tc->ring_buf);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return gerr;
        }
    }

    if (cfg->select_spec) {
        hp_error_t serr = hp_projection_init(&tc->projection, cfg->select_spec);
        if (serr != HP_OK) {
            hp_sliding_buffer_free(&tc->ring_buf);
            hp_json_ser_free(&tc->json_ser);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return serr;
        }
    }
    if (cfg->where_spec) {
        hp_error_t ferr = hp_filter_init(&tc->filter, cfg->where_spec);
        if (ferr != HP_OK) {
            hp_sliding_buffer_free(&tc->ring_buf);
            hp_json_ser_free(&tc->json_ser);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return ferr;
        }
    }

    /* Declared types (--schema): int hints accelerate numeric filter
     * predicates. They are applied at bind time, once the header is known. */
    if (cfg->schema_path) {
        hp_error_t herr = hp_filter_load_hints(&tc->filter, cfg->schema_path);
        if (herr != HP_OK) {
            hp_sliding_buffer_free(&tc->ring_buf);
            hp_json_ser_free(&tc->json_ser);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return herr;
        }
    }

    if (cfg->summary) {
        tc->summary = (hp_summary_t *)malloc(sizeof(hp_summary_t));
        if (!tc->summary) {
            hp_sliding_buffer_free(&tc->ring_buf);
            hp_json_ser_free(&tc->json_ser);
            free(tc->page_buf_data);
            tc->page_buf_data = NULL;
            return HP_ERR_NOMEM;
        }
        hp_summary_init(tc->summary);
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
    hp_reader_free(&tc->in_reader);
    hp_json_ser_free(&tc->json_ser);
    hp_header_free(&tc->in_header);
    hp_header_free(&tc->out_header);
    hp_keymap_free(&tc->keymap);
    if (tc->summary) {
        free(tc->summary);
        tc->summary = NULL;
    }
    if (tc->page_buf_data) {
        free(tc->page_buf_data);
        tc->page_buf_data = NULL;
    }
    if (tc->gz_out) {
        hp_gzip_deflate_free(tc->gz_out);
        free(tc->gz_out);
        tc->gz_out = NULL;
    }
    if (tc->quarantine_file) {
        fclose(tc->quarantine_file);
        tc->quarantine_file = NULL;
    }
}

/* Shared post-header binding: projection, filter, JSON key fragments.
 * Called by the streaming path once the header is known (first row for CSV,
 * first record's schema for NDJSON) and by parallel workers with a preset
 * header. Returns the first binding error, if any. */
static hp_error_t hp_bind_stream_header(hp_transcoder_t *tc) {
    if (tc->projection.active) {
        hp_error_t err = hp_projection_bind(&tc->projection, &tc->in_header);
        if (err != HP_OK) return err;
        hp_project_header(&tc->projection, &tc->in_header, &tc->out_header);
    } else {
        tc->out_header.count = 0;
        for (size_t i = 0; i < tc->in_header.count; i++) {
            tc->out_header.names[tc->out_header.count++] = strdup(tc->in_header.names[i]);
        }
    }

    if (tc->filter.count > 0) {
        hp_error_t err = hp_filter_bind(&tc->filter, &tc->in_header);
        if (err != HP_OK) return err;
    }

    if (tc->config.out_format == HP_FORMAT_NDJSON) {
        hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);
    }
    return HP_OK;
}

hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream) {
    if (!tc || !in_stream || !out_stream) return HP_ERR_INVALID_ARG;

    /* Input reader: plain stdio passthrough, or transparent inflate when the
     * stream turns out to be gzip. hp_reader_init is re-entry safe. */
    hp_reader_init(&tc->in_reader, in_stream);

    double t0 = get_time_sec();
    bool header_processed = false;

    hp_page_buffer_t pb;
    pb.data = tc->page_buf_data;
    pb.cap = tc->page_buf_cap;
    pb.len = 0;
    pb.stream = out_stream;
    pb.gz = tc->gz_out;
    pb.error = HP_OK;

    uint64_t rows_read_local = 0;
    uint64_t rows_emitted_local = 0;
    uint64_t rows_dropped_local = 0;

    size_t bytes_read = 0;
    hp_error_t err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
    if (err != HP_OK) return err;
    tc->stats.bytes_in += bytes_read;

    /* One row buffer reused by every parse call in this run. Keeping it here
     * (instead of inside the row loop) documents that its lifetime spans the
     * whole stream and avoids re-entering its ~24 KB scope per iteration. */
    hp_row_t raw_row;

    size_t available = 0;
    const uint8_t *src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);

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

    /* Parallel workers receive the resolved header from the coordinator:
     * their chunks begin exactly at a data-row boundary, so no header row is
     * parsed and the state the single-stream path reaches after its first
     * row is reproduced directly. */
    if (tc->config.preset_header) {
        const hp_header_t *pre = tc->config.preset_header;
        for (size_t i = 0; i < pre->count && i < HP_MAX_COLUMNS; i++) {
            tc->in_header.names[tc->in_header.count++] = strdup(pre->names[i] ? pre->names[i] : "");
        }
        hp_error_t berr = hp_bind_stream_header(tc);
        if (berr != HP_OK) return berr;
        header_processed = true;
    }

    if (tc->config.in_format == HP_FORMAT_CSV || tc->config.in_format == HP_FORMAT_TSV) {
        hp_error_t (*parse_row_fn)(hp_csv_parser_t *, const uint8_t *, size_t, size_t *, hp_row_t *, bool) = 
            g_hp_dispatch.csv_parse_row ? g_hp_dispatch.csv_parse_row : hp_csv_parse_row_scalar;

        const bool is_csv_out = (tc->config.out_format == HP_FORMAT_CSV || tc->config.out_format == HP_FORMAT_TSV);
        const uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';

        bool proj_is_3 = false;
        int64_t c0 = -1, c1 = -1, c2 = -1;
        bool filt_is_single_eq = false;
        int64_t f_col = -1;
        const char *f_val = NULL;
        size_t f_len = 0;
        bool limit_reached = false;
        bool resyncing = false;   /* set after an oversized-record drop:
                                     discard bytes until the next '\n' */

        while (1) {
            src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            if (available == 0) {
                if (tc->ring_buf.eof_reached) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
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
                /* --jobs span enforcement: a row that begins at or past the
                 * worker's span belongs to the next worker. bytes_in counts
                 * every byte read so far, so the current row's absolute
                 * start offset is bytes_in minus the unparsed window size. */
                if (tc->config.span_end &&
                    tc->stats.bytes_in - (uint64_t)available >= tc->config.span_end) {
                    limit_reached = true;
                    break;
                }

                size_t consumed = 0;

                if (resyncing) {
                    /* Oversized record was dropped: skip to the next line
                     * boundary so its tail is never parsed as fake rows */
                    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                    size_t skip = nl ? (size_t)(nl - src + 1) : available;
                    if (tc->quarantine_file) {
                        fwrite(src, 1, skip, tc->quarantine_file);
                    }
                    src += skip;
                    available -= skip;
                    made_progress = true;
                    if (nl) resyncing = false;
                    continue;
                }

                err = parse_row_fn(&tc->csv_parser, src, available, &consumed, &raw_row, is_eof);
                if (err != HP_OK) {
                    if (tc->config.on_error == HP_ON_ERROR_FAIL) {
                        tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                    size_t skip = nl ? (size_t)(nl - src + 1) : available;
                    if (tc->quarantine_file) {
                        fwrite(src, 1, skip, tc->quarantine_file);
                    }
                    rows_dropped_local++;
                    made_progress = true;
                    src += skip;
                    available -= skip;
                    continue;
                }

                if (consumed == 0) {
                    break;
                }

                made_progress = true;

                if (raw_row.field_count == 0) {
                    src += consumed;
                    available -= consumed;
                    continue;
                }

                if (!header_processed) {
                    tc->in_header.count = 0;
                    for (size_t i = 0; i < raw_row.field_count && i < HP_MAX_COLUMNS; i++) {
                        char buf[256];
                        size_t flen = raw_row.fields[i].len < sizeof(buf) - 1 ? raw_row.fields[i].len : sizeof(buf) - 1;
                        memcpy(buf, raw_row.fields[i].ptr, flen);
                        buf[flen] = '\0';
                        tc->in_header.names[tc->in_header.count++] = strdup(buf);
                    }

                    hp_error_t berr = hp_bind_stream_header(tc);
                    if (berr != HP_OK) {
                        tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
                        hp_page_buffer_flush(&pb);
                        return berr;
                    }

                    header_processed = true;

                    proj_is_3 = (tc->projection.active && tc->projection.count == 3);
                    if (proj_is_3) {
                        c0 = tc->projection.col_indices[0];
                        c1 = tc->projection.col_indices[1];
                        c2 = tc->projection.col_indices[2];
                    }

                    filt_is_single_eq = (tc->filter.count == 1 && tc->filter.preds[0].op == HP_OP_EQ && tc->filter.preds[0].col_index >= 0);
                    if (filt_is_single_eq) {
                        f_col = tc->filter.preds[0].col_index;
                        f_val = tc->filter.preds[0].str_val;
                        f_len = tc->filter.preds[0].str_val_len;
                    }

                    if (is_csv_out && !tc->config.count_only && !tc->config.summary) {
                        /* An empty header name is written as "" so it stays
                         * distinguishable from an absent cell after a
                         * roundtrip; non-empty names stay content-driven.
                         * --count/--summary produce no rows, so they emit no
                         * header line either. */
                        for (size_t i = 0; i < tc->out_header.count; i++) {
                            size_t name_len = strlen(tc->out_header.names[i]);
                            if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                            write_csv_field_pb(&pb, tc->out_header.names[i], name_len, out_delim, name_len == 0);
                        }
                        hp_page_buffer_putc(&pb, '\n');
                    }

                    src += consumed;
                    available -= consumed;
                    continue;
                }

                rows_read_local++;

                if (filt_is_single_eq) {
                    if ((size_t)f_col >= raw_row.field_count) {
                        src += consumed;
                        available -= consumed;
                        continue;
                    }
                    const hp_str_t *f = &raw_row.fields[f_col];
                    if (f->len != f_len || memcmp(f->ptr, f_val, f_len) != 0) {
                        src += consumed;
                        available -= consumed;
                        continue;
                    }
                } else if (!hp_filter_matches(&tc->filter, &raw_row)) {
                    src += consumed;
                    available -= consumed;
                    continue;
                }

                if (tc->config.summary) {
                    /* --summary: fold the row into the column statistics;
                     * no row output is produced */
                    hp_summary_observe(tc->summary, &tc->in_header, &raw_row);
                    rows_emitted_local++;
                    if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                        limit_reached = true;
                        break;
                    }
                    src += consumed;
                    available -= consumed;
                    continue;
                }

                if (tc->config.count_only) {
                    /* --count: count matching rows without materializing any
                     * output; --head/--limit still applies to the count */
                    rows_emitted_local++;
                    if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                        limit_reached = true;
                        break;
                    }
                    src += consumed;
                    available -= consumed;
                    continue;
                }

                if (is_csv_out) {
                    if (pb.len + 2048 >= pb.cap) {
                        hp_page_buffer_flush(&pb);
                    }

                    if (proj_is_3) {
                        if ((size_t)c0 < raw_row.field_count &&
                            (size_t)c1 < raw_row.field_count &&
                            (size_t)c2 < raw_row.field_count &&
                            !raw_row.fields[c0].has_escapes &&
                            !raw_row.fields[c1].has_escapes &&
                            !raw_row.fields[c2].has_escapes) {
                            /* Fast path: clean unquoted fields, raw memcpy */
                            const hp_str_t *f0 = &raw_row.fields[c0];
                            const hp_str_t *f1 = &raw_row.fields[c1];
                            const hp_str_t *f2 = &raw_row.fields[c2];

                            memcpy(pb.data + pb.len, f0->ptr, f0->len);
                            pb.len += f0->len;
                            pb.data[pb.len++] = out_delim;

                            memcpy(pb.data + pb.len, f1->ptr, f1->len);
                            pb.len += f1->len;
                            pb.data[pb.len++] = out_delim;

                            memcpy(pb.data + pb.len, f2->ptr, f2->len);
                            pb.len += f2->len;
                        } else {
                            /* Quoted or short-row: re-serialize each field
                             * so embedded delimiters/newlines are re-quoted */
                            const int64_t cols[3] = { c0, c1, c2 };
                            for (int pi = 0; pi < 3; pi++) {
                                if (pi > 0) pb.data[pb.len++] = out_delim;
                                if (cols[pi] >= 0 && (size_t)cols[pi] < raw_row.field_count) {
                                    const hp_str_t *f = &raw_row.fields[cols[pi]];
                                    if (!f->has_escapes) {
                                        memcpy(pb.data + pb.len, f->ptr, f->len);
                                        pb.len += f->len;
                                    } else {
                                        write_csv_field_pb(&pb, f->ptr, f->len, out_delim, f->has_escapes);
                                    }
                                }
                            }
                        }
                    } else if (tc->projection.active) {
                        const size_t proj_cnt = tc->projection.count;
                        for (size_t i = 0; i < proj_cnt; i++) {
                            if (i > 0) pb.data[pb.len++] = out_delim;
                            int64_t col_idx = tc->projection.col_indices[i];
                            if (col_idx >= 0 && (size_t)col_idx < raw_row.field_count) {
                                const hp_str_t *f = &raw_row.fields[col_idx];
                                if (!f->has_escapes) {
                                    memcpy(pb.data + pb.len, f->ptr, f->len);
                                    pb.len += f->len;
                                } else {
                                    write_csv_field_pb(&pb, f->ptr, f->len, out_delim, f->has_escapes);
                                }
                            }
                        }
                    } else {
                        /* Full-row emission: CSV input emits exactly the
                         * fields each row carries. Ragged rows keep their
                         * real width instead of being silently truncated to
                         * the header width - data loss is never the right
                         * default for a transcoder. (NDJSON input has its
                         * own emission loop, locked to the first-row
                         * schema, where late keys genuinely cannot be
                         * represented without a header rewrite.) */
                        for (size_t i = 0; i < raw_row.field_count; i++) {
                            if (i > 0) pb.data[pb.len++] = out_delim;
                            const hp_str_t *f = &raw_row.fields[i];
                            if (!f->has_escapes) {
                                memcpy(pb.data + pb.len, f->ptr, f->len);
                                pb.len += f->len;
                            } else {
                                write_csv_field_pb(&pb, f->ptr, f->len, out_delim, f->has_escapes);
                            }
                        }
                    }
                    pb.data[pb.len++] = '\n';
                } else {
                    /* NDJSON output never mutates the parsed row, so it is
                     * serialized in place. With a projection, hp_project_row
                     * copies only the selected columns instead of the whole
                     * row struct. */
                    hp_row_t proj_row;
                    const hp_row_t *out_row_ptr;
                    if (tc->projection.active) {
                        hp_project_row(&tc->projection, &raw_row, &proj_row);
                        out_row_ptr = &proj_row;
                    } else {
                        out_row_ptr = &raw_row;
                    }

                    size_t line_len = 0;
                    err = emit_ndjson_row_pb(&pb, &tc->json_ser, &tc->out_header, out_row_ptr, &line_len);
                    if (err != HP_OK) {
                        tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
                        hp_page_buffer_flush(&pb);
                        return err;
                    }

                    tc->stats.bytes_out += line_len;
                }

                rows_emitted_local++;
                if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                    limit_reached = true;
                    break;
                }
                src += consumed;
                available -= consumed;
            }

            tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
            if (limit_reached) break;

            if (!made_progress) {
                if (is_eof) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
                if (err != HP_OK) {
                    if (err == HP_ERR_BUFFER_OVERFLOW && tc->config.on_error != HP_ON_ERROR_FAIL) {
                        /* Row larger than the ring buffer: drop the buffered
                         * span and resynchronize at the next line boundary */
                        if (tc->quarantine_file && available > 0) {
                            fwrite(src, 1, available, tc->quarantine_file);
                        }
                        rows_dropped_local++;
                        tc->ring_buf.head = tc->ring_buf.tail;
                        resyncing = true;
                        continue;
                    }
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
            }
        }
    } else if (tc->config.in_format == HP_FORMAT_NDJSON) {
        char val_arena[64 * 1024];
        bool limit_reached = false;
        bool resyncing = false;   /* skip to the next '\n' after an oversized line */
        hp_row_t raw_row;

        /* Parallel workers with a preset header (see hp_parallel.h) bind
         * their keymap here; single-stream runs build it when the first
         * parsed row binds the schema below. */
        tc->keymap_active = false;
        if (tc->config.preset_header &&
            hp_keymap_build(&tc->keymap, &tc->in_header) == HP_OK) {
            tc->keymap_active = true;
        }

        while (1) {
            src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
            if (available == 0) {
                if (tc->ring_buf.eof_reached) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
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
                /* --jobs span enforcement, same rule as the CSV loop: a row
                 * that begins at or past the worker's span belongs to the
                 * next worker. bytes_in counts every byte read so far, so
                 * the current line's absolute start offset is bytes_in
                 * minus the unparsed window size. */
                if (tc->config.span_end &&
                    tc->stats.bytes_in - (uint64_t)available >= tc->config.span_end) {
                    limit_reached = true;
                    break;
                }

                if (resyncing) {
                    /* Oversized line was dropped: skip to the next line
                     * boundary so its tail is never parsed as fake rows */
                    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                    size_t skip = nl ? (size_t)(nl - src + 1) : available;
                    if (tc->quarantine_file) {
                        fwrite(src, 1, skip, tc->quarantine_file);
                    }
                    src += skip;
                    available -= skip;
                    made_progress = true;
                    if (nl) resyncing = false;
                    continue;
                }

                const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
                if (!nl && !is_eof) {
                    break;
                }

                size_t line_len = nl ? (size_t)(nl - src + 1) : available;

                /* Trailing whitespace is stripped in place: the parser gets
                 * a bounded span into the ring buffer and no line copy is
                 * made. Fields of the parsed row point into that span and
                 * stay valid until the row has been consumed below - the
                 * ring buffer head only advances after the row is emitted
                 * (or rejected). */
                size_t strip_len = line_len;
                while (strip_len > 0 && (src[strip_len - 1] == '\r' || src[strip_len - 1] == '\n' ||
                                         src[strip_len - 1] == ' ' || src[strip_len - 1] == '\t' ||
                                         src[strip_len - 1] == '\v' || src[strip_len - 1] == '\f')) {
                    strip_len--;
                }

                made_progress = true;

                if (strip_len == 0) {
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                err = hp_json_parse_line_ex((const char *)src, strip_len, &tc->in_header,
                                            tc->keymap_active ? &tc->keymap : NULL,
                                            &raw_row, val_arena, sizeof(val_arena));
                if (err != HP_OK) {
                    if (tc->config.on_error == HP_ON_ERROR_FAIL) {
                        tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    if (tc->quarantine_file) {
                        fwrite(src, 1, line_len, tc->quarantine_file);
                    }
                    rows_dropped_local++;
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                if (!header_processed && tc->in_header.count > 0) {
                    hp_error_t berr = hp_bind_stream_header(tc);
                    if (berr != HP_OK) {
                        tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
                        hp_page_buffer_flush(&pb);
                        return berr;
                    }

                    /* Hash index over the first row's schema for O(1) key
                     * resolution; the parser keeps it in sync when keys
                     * appear mid-stream. A failed allocation merely falls
                     * back to the linear walk. */
                    if (hp_keymap_build(&tc->keymap, &tc->in_header) == HP_OK) {
                        tc->keymap_active = true;
                    }

                    header_processed = true;

                    if ((tc->config.out_format == HP_FORMAT_CSV || tc->config.out_format == HP_FORMAT_TSV) &&
                        !tc->config.count_only && !tc->config.summary) {
                        uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                        /* Empty header name canonicalizes to "" (see above);
                         * --count/--summary emit no header line */
                        for (size_t i = 0; i < tc->out_header.count; i++) {
                            size_t name_len = strlen(tc->out_header.names[i]);
                            if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                            write_csv_field_pb(&pb, tc->out_header.names[i], name_len, out_delim, name_len == 0);
                        }
                        hp_page_buffer_putc(&pb, '\n');
                    }
                }

                /* Schema growth mid-stream: single-stream NDJSON output
                 * can follow it (the header line is not yet written).
                 * Parallel workers keep the preset schema instead: a
                 * worker's growth order would depend on which rows it
                 * happens to see first, so keys beyond the first line
                 * serialize under their column index - the same lock rule
                 * CSV output documents. */
                if (!tc->config.preset_header &&
                    !tc->projection.active && tc->config.out_format == HP_FORMAT_NDJSON &&
                    tc->in_header.count > tc->out_header.count) {
                    for (size_t i = tc->out_header.count; i < tc->in_header.count; i++) {
                        tc->out_header.names[tc->out_header.count++] = strdup(tc->in_header.names[i]);
                    }
                    hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);
                }

                rows_read_local++;

                if (!hp_filter_matches(&tc->filter, &raw_row)) {
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                if (tc->config.summary) {
                    /* --summary: fold the row into the column statistics;
                     * no row output is produced */
                    hp_summary_observe(tc->summary, &tc->in_header, &raw_row);
                    rows_emitted_local++;
                    if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                        limit_reached = true;
                        break;
                    }
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                if (tc->config.count_only) {
                    /* --count: count matching rows without materializing any
                     * output; --head/--limit still applies to the count */
                    rows_emitted_local++;
                    if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                        limit_reached = true;
                        break;
                    }
                    src += line_len;
                    available -= line_len;
                    continue;
                }

                hp_row_t proj_row;
                const hp_row_t *out_row_ptr;
                if (tc->projection.active) {
                    hp_project_row(&tc->projection, &raw_row, &proj_row);
                    out_row_ptr = &proj_row;
                } else {
                    out_row_ptr = &raw_row;
                }

                if (tc->config.out_format == HP_FORMAT_NDJSON) {
                    size_t out_line_len = 0;
                    err = emit_ndjson_row_pb(&pb, &tc->json_ser, &tc->out_header, out_row_ptr, &out_line_len);
                    if (err != HP_OK) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    tc->stats.bytes_out += out_line_len;
                } else {
                    uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    for (size_t i = 0; i < out_row_ptr->field_count; i++) {
                        if (i > 0) hp_page_buffer_putc(&pb, out_delim);
                        write_csv_field_pb(&pb, out_row_ptr->fields[i].ptr, out_row_ptr->fields[i].len, out_delim, out_row_ptr->fields[i].has_escapes);
                    }
                    hp_page_buffer_putc(&pb, '\n');
                }
                rows_emitted_local++;
                if (tc->config.limit_rows && rows_emitted_local >= tc->config.limit_rows) {
                    limit_reached = true;
                    break;
                }
                src += line_len;
                available -= line_len;
            }

            /* Publish the local cursor: with zero-copy parsing the ring
             * buffer head advances here, once per outer iteration, exactly
             * when the last parsed row has been consumed. */
            tc->ring_buf.head = (size_t)(src - tc->ring_buf.data);
            if (limit_reached) break;

            if (!made_progress) {
                if (is_eof) break;
                err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
                if (err != HP_OK) {
                    if (err == HP_ERR_BUFFER_OVERFLOW && tc->config.on_error != HP_ON_ERROR_FAIL) {
                        /* Line larger than the ring buffer: quarantine the
                         * buffered span and resynchronize at the next line
                         * boundary so its tail is never parsed as fake rows */
                        if (tc->quarantine_file && available > 0) {
                            fwrite(src, 1, available, tc->quarantine_file);
                        }
                        rows_dropped_local++;
                        tc->ring_buf.head = tc->ring_buf.tail;
                        resyncing = true;
                        continue;
                    }
                    hp_page_buffer_flush(&pb);
                    return err;
                }
                tc->stats.bytes_in += bytes_read;
            }
        }
    }

    hp_error_t finish_err = hp_page_buffer_finish(&pb);
    double t1 = get_time_sec();

    /* --summary prints its table after the (empty) row stream, directly to
     * the output stream: it is the run's only output. */
    if (tc->config.summary && tc->summary) {
        hp_summary_print(tc->summary, out_stream);
    }

    tc->stats.rows_read += rows_read_local;
    tc->stats.rows_emitted += rows_emitted_local;
    tc->stats.rows_dropped += rows_dropped_local;
    tc->stats.elapsed_seconds = t1 - t0;
    return finish_err;
}
