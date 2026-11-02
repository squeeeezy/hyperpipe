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
    size_t (*sink)(void *ctx, const void *data, size_t len); /* replaces stream */
    void *sink_ctx;
    hp_gzip_deflate_t *gz;   /* non-NULL: output is compressed through zlib */
    hp_error_t error;        /* first I/O or compression error, if any */
} hp_page_buffer_t;

/* Delivers bytes that are already in their final (possibly compressed)
 * form to the sink or the stream. */
static void hp_page_buffer_send(hp_page_buffer_t *pb, const void *data, size_t len) {
    if (len == 0) return;
    size_t n = pb->sink ? pb->sink(pb->sink_ctx, data, len)
                        : fwrite(data, 1, len, pb->stream);
    if (n != len && pb->error == HP_OK) pb->error = HP_ERR_IO;
}

/* Every output byte passes through here, so gzip output also covers the
 * records too large to be staged in the page. Output stops at the first
 * error. */
static void hp_page_buffer_emit(hp_page_buffer_t *pb, const void *data, size_t len) {
    if (pb->error != HP_OK || len == 0) return;
    if (pb->gz) {
        const unsigned char *out = NULL;
        size_t out_len = 0;
        hp_error_t err = hp_gzip_deflate_push(pb->gz, data, len, &out, &out_len);
        if (err != HP_OK) {
            pb->error = err;
            return;
        }
        data = out;
        len = out_len;
    }
    hp_page_buffer_send(pb, data, len);
}

HP_INLINE void hp_page_buffer_flush(hp_page_buffer_t *pb) {
    hp_page_buffer_emit(pb, pb->data, pb->len);
    pb->len = 0;
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
        } else {
            hp_page_buffer_send(pb, out, out_len);
        }
    }
    if (pb->error == HP_OK && pb->stream && ferror(pb->stream)) {
        pb->error = HP_ERR_IO;
    }
    return pb->error;
}

HP_INLINE void hp_page_buffer_write(hp_page_buffer_t *pb, const void *src, size_t size) {
    if (size >= pb->cap) {
        hp_page_buffer_flush(pb);
        hp_page_buffer_emit(pb, src, size);
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

/* Copies a field. Most fields are a few bytes long, where a library memcpy
 * call costs more than the copy itself; up to 32 bytes the copy is done as
 * two possibly overlapping fixed-size moves that touch only [src, src+len). */
HP_INLINE uint8_t *copy_field(uint8_t *dst, const char *src, size_t len) {
    if (len >= 16) {
        if (len > 32) {
            memcpy(dst, src, len);
        } else {
            memcpy(dst, src, 16);
            memcpy(dst + len - 16, src + len - 16, 16);
        }
    } else if (len >= 8) {
        memcpy(dst, src, 8);
        memcpy(dst + len - 8, src + len - 8, 8);
    } else if (len >= 4) {
        memcpy(dst, src, 4);
        memcpy(dst + len - 4, src + len - 4, 4);
    } else {
        for (size_t i = 0; i < len; i++) dst[i] = (uint8_t)src[i];
    }
    return dst + len;
}

/* True when any byte of the 64-bit word equals c ("has zero byte" test on
 * w ^ c, exact as a yes/no answer). */
HP_INLINE bool word_has_byte(uint64_t w, uint8_t c) {
    const uint64_t ones = 0x0101010101010101ull;
    uint64_t x = w ^ (ones * c);
    return ((x - ones) & ~x & (ones << 7)) != 0;
}

HP_INLINE bool word_has_csv_special(uint64_t w, uint8_t delim) {
    return word_has_byte(w, delim) | word_has_byte(w, '"') |
           word_has_byte(w, '\n') | word_has_byte(w, '\r');
}

/* Whether a value needs quoting in CSV output, checked eight bytes at a
 * time; the last word overlaps the previous one instead of falling back to
 * a byte loop, so only bytes of the value are ever read. */
static bool has_csv_special(const char *p, size_t len, uint8_t delim) {
    uint64_t w;
    if (len < 8) {
        for (size_t i = 0; i < len; i++) {
            if (p[i] == delim || p[i] == '"' || p[i] == '\n' || p[i] == '\r') return true;
        }
        return false;
    }
    for (size_t i = 0; i + 8 <= len; i += 8) {
        memcpy(&w, p + i, 8);
        if (word_has_csv_special(w, delim)) return true;
    }
    memcpy(&w, p + len - 8, 8);
    return word_has_csv_special(w, delim);
}

/* Worst case for one CSV field: every byte a quote that gets doubled, plus
 * the two enclosing quotes. */
#define CSV_FIELD_BOUND(len) (2 * (len) + 2)

/* Writes one CSV field at out, quoting it whenever the value contains the
 * delimiter, a quote or a line break, and returns the new end. The caller
 * guarantees CSV_FIELD_BOUND(len) free bytes. type_quoted preserves the
 * string identity of zero-length values: an empty field that arrived as a
 * typed string ("" in the source or "" in NDJSON) must be written back as
 * "", otherwise it degenerates to a blank field and loses its type.
 * Non-empty values keep the classic content-driven rule, which preserves
 * byte identity for data that never needed quoting. */
static uint8_t *put_csv_field(uint8_t *out, const char *p, size_t len,
                              uint8_t delim, bool type_quoted) {
    bool needs_quotes = (type_quoted && len == 0) || has_csv_special(p, len, delim);
    if (!needs_quotes) return copy_field(out, p, len);
    *out++ = '"';
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '"') *out++ = '"';
        *out++ = (uint8_t)p[i];
    }
    *out++ = '"';
    return out;
}

/* Page-buffered put_csv_field(). A field too large for one page (only an
 * enormous value gets there) is streamed through the checked writers. */
static void write_csv_field_pb(hp_page_buffer_t *pb, const char *p, size_t len,
                               uint8_t delim, bool type_quoted) {
    if (CSV_FIELD_BOUND(len) <= pb->cap) {
        if (pb->len + CSV_FIELD_BOUND(len) > pb->cap) hp_page_buffer_flush(pb);
        uint8_t *end = put_csv_field(pb->data + pb->len, p, len, delim, type_quoted);
        pb->len = (size_t)(end - pb->data);
        return;
    }

    if (!has_csv_special(p, len, delim)) {
        hp_page_buffer_write(pb, p, len);
        return;
    }
    hp_page_buffer_putc(pb, '"');
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '"') hp_page_buffer_putc(pb, '"');
        hp_page_buffer_putc(pb, (uint8_t)p[i]);
    }
    hp_page_buffer_putc(pb, '"');
}

/* Writes one CSV/TSV record parsed from raw_len input bytes. Without a
 * projection (cols == NULL) the row is emitted at its own width, so ragged
 * rows keep every field instead of being cut to the header. Projected
 * columns missing from the row are left empty.
 *
 * A field without escapes is copied verbatim only when it is known not to
 * contain the output delimiter, which holds for CSV-to-CSV and TSV-to-TSV.
 * Otherwise (quote_clean) every field goes through the quoting rules: a TSV
 * cell may hold commas, an NDJSON value may hold anything.
 *
 * Every field is a slice of the raw record (or its unescaped copy, which is
 * never longer), so the record's worst-case output size follows from
 * raw_len alone: the whole record is reserved with one check and written
 * without per-field bounds checks. Only a record too large for one page
 * takes the checked path. */
static void emit_csv_row_pb(hp_page_buffer_t *pb, const hp_row_t *row,
                            const int64_t *cols, size_t col_count, uint8_t delim,
                            size_t raw_len, bool quote_clean) {
    const size_t n = cols ? col_count : row->field_count;
    /* A projection may name a column twice, so each output field is bounded
     * by the whole record; a full row's fields partition it. */
    const size_t bound = cols ? n * (CSV_FIELD_BOUND(raw_len) + 1) + 1
                              : CSV_FIELD_BOUND(raw_len) + 3 * n + 1;

    if (bound <= pb->cap) {
        if (pb->len + bound > pb->cap) hp_page_buffer_flush(pb);
        uint8_t *out = pb->data + pb->len;
        for (size_t i = 0; i < n; i++) {
            if (i > 0) *out++ = delim;
            int64_t idx = cols ? cols[i] : (int64_t)i;
            if (idx < 0 || (size_t)idx >= row->field_count) continue;

            const hp_str_t *f = &row->fields[idx];
            if (f->has_escapes || quote_clean) {
                out = put_csv_field(out, f->ptr, f->len, delim, f->has_escapes);
            } else {
                out = copy_field(out, f->ptr, f->len);
            }
        }
        *out++ = '\n';
        pb->len = (size_t)(out - pb->data);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        if (i > 0) hp_page_buffer_putc(pb, delim);
        int64_t idx = cols ? cols[i] : (int64_t)i;
        if (idx < 0 || (size_t)idx >= row->field_count) continue;

        const hp_str_t *f = &row->fields[idx];
        if (f->has_escapes || quote_clean) {
            write_csv_field_pb(pb, f->ptr, f->len, delim, f->has_escapes);
        } else {
            hp_page_buffer_write(pb, f->ptr, f->len);
        }
    }
    hp_page_buffer_putc(pb, '\n');
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
    hp_csv_parser_free(&tc->csv_parser);
    free(tc->value_arena);
    tc->value_arena = NULL;
    tc->value_arena_cap = 0;
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
    if (!tc || !in_stream || (!out_stream && !tc->config.sink)) return HP_ERR_INVALID_ARG;

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
    pb.sink = tc->config.sink;
    pb.sink_ctx = tc->config.sink_ctx;
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

        const int64_t *out_cols = tc->projection.active ? tc->projection.col_indices : NULL;
        const bool quote_clean = tc->csv_parser.delim != out_delim;
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

                if (!hp_filter_matches(&tc->filter, &raw_row)) {
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
                    emit_csv_row_pb(&pb, &raw_row, out_cols, tc->projection.count, out_delim, consumed,
                                    quote_clean);
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

                /* Decoded string values are never longer than their escaped
                 * source, so an arena as large as the line always suffices. */
                if (strip_len > tc->value_arena_cap) {
                    char *arena = (char *)realloc(tc->value_arena, strip_len);
                    if (!arena) {
                        hp_page_buffer_flush(&pb);
                        return HP_ERR_NOMEM;
                    }
                    tc->value_arena = arena;
                    tc->value_arena_cap = strip_len;
                }
                err = hp_json_parse_line_ex((const char *)src, strip_len, &tc->in_header,
                                            tc->keymap_active ? &tc->keymap : NULL,
                                            &raw_row, tc->value_arena, tc->value_arena_cap);
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

                if (tc->config.out_format == HP_FORMAT_NDJSON) {
                    hp_row_t proj_row;
                    const hp_row_t *out_row_ptr = &raw_row;
                    if (tc->projection.active) {
                        hp_project_row(&tc->projection, &raw_row, &proj_row);
                        out_row_ptr = &proj_row;
                    }
                    size_t out_line_len = 0;
                    err = emit_ndjson_row_pb(&pb, &tc->json_ser, &tc->out_header, out_row_ptr, &out_line_len);
                    if (err != HP_OK) {
                        hp_page_buffer_flush(&pb);
                        return err;
                    }
                    tc->stats.bytes_out += out_line_len;
                } else {
                    /* Every value is a slice of the line or a decoded copy no
                     * longer than its source, so the line length bounds the
                     * record just as it does for CSV input. */
                    const uint8_t out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
                    emit_csv_row_pb(&pb, &raw_row,
                                    tc->projection.active ? tc->projection.col_indices : NULL,
                                    tc->projection.count, out_delim, strip_len, true);
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
    if (tc->config.summary && tc->summary && out_stream) {
        hp_summary_print(tc->summary, out_stream);
    }

    tc->stats.rows_read += rows_read_local;
    tc->stats.rows_emitted += rows_emitted_local;
    tc->stats.rows_dropped += rows_dropped_local;
    tc->stats.elapsed_seconds = t1 - t0;
    return finish_err;
}
