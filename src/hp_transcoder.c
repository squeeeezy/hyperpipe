#include "hyperpipe/hp_transcoder.h"
#include "hp_clock.h"
#include <ctype.h>

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

/* Nonzero when any byte of the 64-bit word equals c ("has zero byte" test
 * on w ^ c, exact as a yes/no answer). */
HP_INLINE uint64_t word_byte_mask(uint64_t w, uint8_t c) {
    const uint64_t ones = 0x0101010101010101ull;
    uint64_t x = w ^ (ones * c);
    return (x - ones) & ~x & (ones << 7);
}

HP_INLINE bool word_has_csv_special(uint64_t w, uint8_t delim) {
    return (word_byte_mask(w, delim) | word_byte_mask(w, '"') |
            word_byte_mask(w, '\n') | word_byte_mask(w, '\r')) != 0;
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

/* Malformed or oversized input that fault tolerance skipped, byte for byte */
static void quarantine_write(hp_transcoder_t *tc, const uint8_t *data, size_t len) {
    if (len == 0) return;
    if (tc->config.quarantine_sink) {
        tc->config.quarantine_sink(tc->config.quarantine_ctx, data, len);
    } else if (tc->quarantine_file) {
        fwrite(data, 1, len, tc->quarantine_file);
    }
}

const char *hp_strerror(hp_error_t err) {
    switch (err) {
        case HP_OK:                   return "success";
        case HP_ERR_NOMEM:            return "out of memory";
        case HP_ERR_IO:               return "read or write error";
        case HP_ERR_PARSE_QUOTE:      return "malformed CSV quoting";
        case HP_ERR_PARSE_DELIM:      return "malformed CSV record";
        case HP_ERR_BUFFER_OVERFLOW:  return "record larger than the input buffer (see --buffer-size) "
                                             "or more than 1024 columns";
        case HP_ERR_INVALID_ARG:      return "invalid argument";
        case HP_ERR_UNSUPPORTED_SIMD: return "SIMD engine not supported by this CPU";
        case HP_ERR_COLUMN_NOT_FOUND: return "column not found in the input header";
        case HP_ERR_GZIP_UNAVAILABLE: return "gzip support needs zlib, which was not found";
        case HP_ERR_UNSUPPORTED:      return "not supported for this input";
        case HP_ERR_PARSE_JSON:       return "malformed JSON";
        case HP_ERR_FILTER:           return "invalid filter";
        case HP_ERR_AGG:              return "invalid --agg";
        case HP_ERR_GENERIC:          break;
    }
    return "internal error";
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

static hp_error_t init_parts(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg) {
    size_t cap = cfg->buffer_capacity ? cfg->buffer_capacity : HP_DEFAULT_BUFFER_CAPACITY;
    hp_error_t err = hp_sliding_buffer_init(&tc->ring_buf, cap);
    if (err != HP_OK) return err;

    /* Output page buffer: flushed with one fwrite per fill. 256 KiB keeps
     * the working set inside L2 while amortizing write syscalls; measured
     * flat vs 64 KiB on the reference host, better on slower storage. */
    tc->page_buf_cap = 256 * 1024;
    tc->page_buf_data = (uint8_t *)malloc(tc->page_buf_cap);
    if (!tc->page_buf_data) return HP_ERR_NOMEM;

    hp_dispatch_init(cfg->simd_mode);

    hp_format_t parser_fmt = (cfg->in_format == HP_FORMAT_AUTO) ? HP_FORMAT_CSV : cfg->in_format;
    if ((err = hp_csv_parser_init(&tc->csv_parser, parser_fmt, cfg->simd_mode)) != HP_OK) return err;
    if ((err = hp_json_ser_init(&tc->json_ser, 4096, cfg->simd_mode)) != HP_OK) return err;
    tc->json_ser.scan_escapes = g_hp_dispatch.json_scan_escapes;
    tc->json_ser.emit_string = g_hp_dispatch.json_emit_string;

    /* Output gzip: resolve libz up front so a missing library fails fast
     * with a dedicated error code instead of mid-stream. */
    if (cfg->gzip_out) {
        hp_gzip_load();
        if (!hp_gzip_available()) return HP_ERR_GZIP_UNAVAILABLE;
        tc->gz_out = (hp_gzip_deflate_t *)calloc(1, sizeof(hp_gzip_deflate_t));
        if (!tc->gz_out) return HP_ERR_NOMEM;
        if ((err = hp_gzip_deflate_start(tc->gz_out)) != HP_OK) return err;
    }

    if (cfg->select_spec && (err = hp_projection_init(&tc->projection, cfg->select_spec)) != HP_OK) {
        return err;
    }
    if (cfg->where_spec && (err = hp_filter_init(&tc->filter, cfg->where_spec)) != HP_OK) {
        return err;
    }
    /* Declared types (--schema): int hints accelerate numeric filter
     * predicates. They are applied at bind time, once the header is known. */
    if (cfg->schema_path && (err = hp_filter_load_hints(&tc->filter, cfg->schema_path)) != HP_OK) {
        return err;
    }

    if (cfg->summary) {
        tc->summary = (hp_summary_t *)malloc(sizeof(hp_summary_t));
        if (!tc->summary) return HP_ERR_NOMEM;
        hp_summary_init(tc->summary);
    }
    if (cfg->group_by_spec || cfg->agg_spec) {
        tc->agg = (hp_agg_t *)malloc(sizeof(hp_agg_t));
        if (!tc->agg) return HP_ERR_NOMEM;
        if ((err = hp_agg_init(tc->agg, cfg->group_by_spec, cfg->agg_spec)) != HP_OK) return err;
    }
    if (cfg->sort_spec && !cfg->count_only) {
        tc->sorter = (hp_sorter_t *)malloc(sizeof(hp_sorter_t));
        if (!tc->sorter) return HP_ERR_NOMEM;
        err = hp_sorter_init(tc->sorter, cfg->sort_spec, cfg->limit_rows, cfg->sort_memory);
        if (err != HP_OK) return err;
        tc->sorter->seq = cfg->row_seq_base;
    }

    if (cfg->on_error == HP_ON_ERROR_QUARANTINE && cfg->quarantine_path) {
        tc->quarantine_file = fopen(cfg->quarantine_path, "wb");
        if (!tc->quarantine_file) return HP_ERR_IO;
    }
    return HP_OK;
}

hp_error_t hp_transcoder_init(hp_transcoder_t *tc, const hp_transcoder_config_t *cfg) {
    if (!tc || !cfg) return HP_ERR_INVALID_ARG;
    memset(tc, 0, sizeof(*tc));
    tc->config = *cfg;
    hp_error_t err = init_parts(tc, cfg);
    if (err != HP_OK) hp_transcoder_free(tc);
    return err;
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
    if (tc->agg) {
        hp_agg_free(tc->agg);
        free(tc->agg);
        tc->agg = NULL;
    }
    if (tc->sorter) {
        hp_sorter_free(tc->sorter);
        free(tc->sorter);
        tc->sorter = NULL;
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

static hp_error_t column_not_found(hp_transcoder_t *tc, const char *name) {
    snprintf(tc->stats.error_column, sizeof(tc->stats.error_column), "%s", name);
    return HP_ERR_COLUMN_NOT_FOUND;
}

/* Shared post-header binding: projection, filter, JSON key fragments.
 * Called by the streaming path once the header is known (first row for CSV,
 * first record's schema for NDJSON) and by parallel workers with a preset
 * header. Returns the first binding error, if any. */
static hp_error_t hp_bind_stream_header(hp_transcoder_t *tc) {
    if (tc->agg) {
        const char *missing = NULL;
        hp_error_t err = hp_agg_bind(tc->agg, &tc->in_header, &missing);
        if (err == HP_ERR_COLUMN_NOT_FOUND) return column_not_found(tc, missing);
        if (err == HP_OK) err = hp_agg_header(tc->agg, &tc->out_header);
        if (err != HP_OK) return err;
    } else if (tc->projection.active) {
        hp_error_t err = hp_projection_bind(&tc->projection, &tc->in_header);
        if (err == HP_ERR_COLUMN_NOT_FOUND) {
            size_t i = 0;
            while (tc->projection.col_indices[i] >= 0) i++;
            return column_not_found(tc, tc->projection.col_names[i]);
        }
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
        if (err == HP_ERR_COLUMN_NOT_FOUND) {
            size_t i = 0;
            while (tc->filter.preds[i].col_index >= 0) i++;
            return column_not_found(tc, tc->filter.preds[i].col_name);
        }
        if (err != HP_OK) return err;
    }

    if (tc->sorter && !tc->agg) {
        const char *missing = NULL;
        hp_error_t err = hp_sorter_bind(tc->sorter, &tc->in_header, &missing);
        if (err == HP_ERR_COLUMN_NOT_FOUND) return column_not_found(tc, missing);
        if (err != HP_OK) return err;
    }

    if (tc->config.out_format == HP_FORMAT_NDJSON) {
        hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);
    }
    return HP_OK;
}

/* ---- sorted output ---------------------------------------------------------- */

/* With --sort, rows are written to a page of their own instead of the
 * output, and each line is handed to the sorter. A line too large for the
 * page arrives partly through the sink, in spill. */
typedef struct hp_capture_s {
    hp_page_buffer_t pb;
    uint8_t *spill;
    size_t spill_len, spill_cap;
} hp_capture_t;

static size_t capture_sink(void *ctx, const void *data, size_t len) {
    hp_capture_t *c = (hp_capture_t *)ctx;
    if (c->spill_len + len > c->spill_cap) {
        size_t cap = c->spill_cap ? c->spill_cap : 4096;
        while (cap < c->spill_len + len) cap *= 2;
        uint8_t *spill = (uint8_t *)realloc(c->spill, cap);
        if (!spill) return 0;
        c->spill = spill;
        c->spill_cap = cap;
    }
    memcpy(c->spill + c->spill_len, data, len);
    c->spill_len += len;
    return len;
}

static hp_error_t capture_init(hp_capture_t *c, size_t cap) {
    memset(c, 0, sizeof(*c));
    c->pb = (hp_page_buffer_t){ .cap = cap, .sink = capture_sink, .sink_ctx = c, .error = HP_OK };
    c->pb.data = (uint8_t *)malloc(cap);
    return c->pb.data ? HP_OK : HP_ERR_NOMEM;
}

static void capture_free(hp_capture_t *c) {
    free(c->pb.data);
    free(c->spill);
    memset(c, 0, sizeof(*c));
}

/* Hands the line just written to the sorter, keyed by row */
static hp_error_t capture_take(hp_capture_t *c, hp_sorter_t *sorter, const hp_row_t *row) {
    if (c->pb.error != HP_OK) return c->pb.error;
    const uint8_t *line = c->pb.data;
    size_t len = c->pb.len;
    if (c->spill_len > 0) {
        if (capture_sink(c, c->pb.data, c->pb.len) != c->pb.len) return HP_ERR_NOMEM;
        line = c->spill;
        len = c->spill_len;
    }
    hp_error_t err = hp_sorter_add(sorter, row, line, len);
    c->pb.len = 0;
    c->spill_len = 0;
    return err;
}

static void emit_line(void *ctx, const void *data, size_t len) {
    hp_page_buffer_write((hp_page_buffer_t *)ctx, data, len);
}

/* ---- streaming ------------------------------------------------------------ */

/* State of one hp_transcoder_run() */
typedef struct hp_run_s {
    hp_transcoder_t *tc;
    hp_page_buffer_t pb;
    hp_row_t row;                  /* the record being processed */
    hp_row_t projected;            /* NDJSON output with a projection */
    hp_error_t (*parse_csv)(hp_csv_parser_t *, const uint8_t *, size_t, size_t *, hp_row_t *, bool);
    bool ndjson_in;
    bool header_done;
    bool csv_out;                  /* CSV or TSV output */
    uint8_t out_delim;
    bool quote_clean;              /* clean input fields may hold out_delim */
    bool resyncing;                /* drop input up to the next newline */
    bool sorting;                  /* rows go through capture to the sorter */
    hp_capture_t capture;
    uint64_t skipped;              /* the header row and blank lines */
    uint64_t rows_read;
    uint64_t rows_emitted;
    uint64_t rows_dropped;
} hp_run_t;

/* Parses the next CSV record. *consumed == 0 asks for more input; *raw_len
 * is the record's size in the input, which bounds its output. */
HP_INLINE hp_error_t next_csv_record(hp_run_t *run, const uint8_t *src, size_t available,
                                     bool is_eof, size_t *consumed, size_t *raw_len) {
    hp_error_t err = run->parse_csv(&run->tc->csv_parser, src, available, consumed, &run->row, is_eof);
    *raw_len = *consumed;
    return err;
}

/* Parses the next NDJSON line. Fields point into the ring buffer and stay
 * valid until the caller advances past the line. */
HP_INLINE hp_error_t next_ndjson_record(hp_run_t *run, const uint8_t *src, size_t available,
                                        bool is_eof, size_t *consumed, size_t *raw_len) {
    hp_transcoder_t *tc = run->tc;
    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
    *consumed = 0;
    if (!nl && !is_eof) return HP_OK;
    *consumed = nl ? (size_t)(nl - src + 1) : available;

    size_t len = *consumed;
    while (len > 0 && (src[len - 1] == '\r' || src[len - 1] == '\n' || src[len - 1] == ' ' ||
                       src[len - 1] == '\t' || src[len - 1] == '\v' || src[len - 1] == '\f')) {
        len--;
    }
    *raw_len = len;
    run->row.field_count = 0;
    if (len == 0) return HP_OK;

    /* Decoded strings are never longer than their escaped source, so an
     * arena as large as the line always suffices */
    if (len > tc->value_arena_cap) {
        char *arena = (char *)realloc(tc->value_arena, len);
        if (!arena) return HP_ERR_NOMEM;
        tc->value_arena = arena;
        tc->value_arena_cap = len;
    }
    return hp_json_parse_line_ex((const char *)src, len, &tc->in_header,
                                 tc->keymap_active ? &tc->keymap : NULL,
                                 &run->row, tc->value_arena, tc->value_arena_cap);
}

/* An empty header name is written as "" so it stays distinguishable from
 * an absent cell after a round trip */
static void write_csv_names(hp_page_buffer_t *pb, const hp_header_t *header, uint8_t delim) {
    for (size_t i = 0; i < header->count; i++) {
        size_t len = strlen(header->names[i]);
        if (i > 0) hp_page_buffer_putc(pb, delim);
        write_csv_field_pb(pb, header->names[i], len, delim, len == 0);
    }
    hp_page_buffer_putc(pb, '\n');
}

/* --count and --summary write no rows, so they write no header either; an
 * aggregation writes its header with its rows */
static void write_csv_header(hp_run_t *run) {
    hp_transcoder_t *tc = run->tc;
    if (!run->csv_out || tc->config.count_only || tc->config.summary || tc->agg) return;
    write_csv_names(&run->pb, &tc->out_header, run->out_delim);
}

/* Binds the stream to its header: the first CSV record, or the keys of the
 * first NDJSON line. */
static hp_error_t start_stream(hp_run_t *run) {
    hp_transcoder_t *tc = run->tc;
    if (!run->ndjson_in) {
        for (size_t i = 0; i < run->row.field_count && i < HP_MAX_COLUMNS; i++) {
            const hp_str_t *f = &run->row.fields[i];
            char *name = (char *)malloc(f->len + 1);
            if (!name) return HP_ERR_NOMEM;
            memcpy(name, f->ptr, f->len);
            name[f->len] = '\0';
            tc->in_header.names[tc->in_header.count++] = name;
        }
    }

    hp_error_t err = hp_bind_stream_header(tc);
    if (err != HP_OK) return err;

    /* A hashed index over the first line's keys; the parser keeps it in
     * step as keys appear. Without one, lookups fall back to a linear walk. */
    if (run->ndjson_in && hp_keymap_build(&tc->keymap, &tc->in_header) == HP_OK) {
        tc->keymap_active = true;
    }
    run->header_done = true;
    write_csv_header(run);
    return HP_OK;
}

/* NDJSON output can follow keys that appear mid-stream, since no header
 * line was written. Parallel runs lock the schema instead: growth order
 * would depend on the rows each chunk happens to see first. */
HP_INLINE void follow_schema_growth(hp_run_t *run) {
    hp_transcoder_t *tc = run->tc;
    if (tc->config.lock_schema || tc->projection.active ||
        tc->config.out_format != HP_FORMAT_NDJSON ||
        tc->in_header.count <= tc->out_header.count) {
        return;
    }
    for (size_t i = tc->out_header.count; i < tc->in_header.count; i++) {
        tc->out_header.names[tc->out_header.count++] = strdup(tc->in_header.names[i]);
    }
    hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);
}

/* Handles one record that passed the filter: folds it into the groups or
 * --summary, counts it for --count, or writes it. Sets *done once --head
 * is reached; with groups, --head limits the groups written. */
HP_INLINE hp_error_t deliver_record(hp_run_t *run, size_t raw_len, bool *done) {
    hp_transcoder_t *tc = run->tc;
    if (tc->agg) {
        return hp_agg_observe(tc->agg, &run->row);
    } else if (tc->config.summary) {
        hp_summary_observe(tc->summary, &tc->in_header, &run->row);
    } else if (tc->config.count_only) {
        /* counted below, nothing is written */
    } else {
        hp_page_buffer_t *pb = run->sorting ? &run->capture.pb : &run->pb;
        if (run->csv_out) {
            emit_csv_row_pb(pb, &run->row,
                            tc->projection.active ? tc->projection.col_indices : NULL,
                            tc->projection.count, run->out_delim, raw_len, run->quote_clean);
        } else {
            const hp_row_t *row = &run->row;
            if (tc->projection.active) {
                hp_project_row(&tc->projection, &run->row, &run->projected);
                row = &run->projected;
            }
            size_t line_len = 0;
            hp_error_t err = emit_ndjson_row_pb(pb, &tc->json_ser, &tc->out_header, row, &line_len);
            if (err != HP_OK) return err;
            tc->stats.bytes_out += line_len;
        }
        if (run->sorting) {
            hp_error_t err = capture_take(&run->capture, tc->sorter, &run->row);
            if (err != HP_OK) return err;
        }
    }
    run->rows_emitted++;
    /* Sorting needs every row before the first N are known */
    *done = !run->sorting && tc->config.limit_rows && run->rows_emitted >= tc->config.limit_rows;
    return HP_OK;
}

/* Bytes up to and including the next newline: the unit that fault
 * tolerance drops after a malformed or oversized record */
HP_INLINE size_t line_span(const uint8_t *src, size_t available) {
    const uint8_t *nl = (const uint8_t *)memchr(src, '\n', available);
    return nl ? (size_t)(nl - src + 1) : available;
}

/* Writes the rows of a finished aggregation, at most limit of them (0: all) */
static hp_error_t emit_groups(hp_transcoder_t *tc, hp_page_buffer_t *pb, const hp_agg_t *agg,
                              uint64_t *emitted) {
    const hp_transcoder_config_t *cfg = &tc->config;
    const size_t rows = hp_agg_rows(agg);
    const size_t n = (cfg->limit_rows && cfg->limit_rows < rows) ? (size_t)cfg->limit_rows : rows;
    *emitted = n;
    if (cfg->count_only) return HP_OK;

    hp_capture_t capture;
    hp_sorter_t *sorter = tc->sorter;
    if (sorter) {
        const char *missing = NULL;
        hp_error_t err = hp_sorter_bind(sorter, &tc->out_header, &missing);
        if (err == HP_ERR_COLUMN_NOT_FOUND) return column_not_found(tc, missing);
        if (err == HP_OK) err = capture_init(&capture, tc->page_buf_cap);
        if (err != HP_OK) return err;
    }
    hp_page_buffer_t *out = sorter ? &capture.pb : pb;

    const bool csv_out = cfg->out_format == HP_FORMAT_CSV || cfg->out_format == HP_FORMAT_TSV;
    const uint8_t delim = cfg->out_format == HP_FORMAT_TSV ? '\t' : ',';
    if (csv_out) write_csv_names(pb, &tc->out_header, delim);

    static hp_row_t row;    /* 32 KiB: kept off the stack of a worker */
    char scratch[HP_AGG_MAX_FNS * HP_AGG_VALUE_MAX];
    hp_error_t err = HP_OK;
    for (size_t i = 0; err == HP_OK && i < (sorter ? rows : n); i++) {
        hp_agg_row(agg, i, &row, scratch);
        if (csv_out) {
            size_t raw_len = row.field_count;
            for (size_t f = 0; f < row.field_count; f++) raw_len += row.fields[f].len;
            emit_csv_row_pb(out, &row, NULL, 0, delim, raw_len, true);
        } else {
            size_t line_len = 0;
            err = emit_ndjson_row_pb(out, &tc->json_ser, &tc->out_header, &row, &line_len);
            tc->stats.bytes_out += line_len;
        }
        if (err == HP_OK && sorter) err = capture_take(&capture, sorter, &row);
    }
    if (sorter) {
        if (err == HP_OK) err = hp_sorter_write(sorter, emit_line, pb, emitted);
        capture_free(&capture);
    }
    return err;
}

static hp_error_t finish_run(hp_run_t *run, hp_error_t err) {
    hp_transcoder_t *tc = run->tc;
    if (err == HP_OK && run->sorting && !tc->config.partial) {
        err = hp_sorter_write(tc->sorter, emit_line, &run->pb, &run->rows_emitted);
    } else if (err == HP_OK && run->sorting) {
        err = hp_sorter_finish(tc->sorter);
    }
    if (run->sorting) capture_free(&run->capture);
    if (err == HP_OK && tc->agg && !tc->config.partial) {
        /* An input without rows still has a result, a count of 0, under
         * the columns as they were given */
        if (!run->header_done && (err = hp_agg_header(tc->agg, &tc->out_header)) == HP_OK) {
            hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);
        }
        if (err == HP_OK) err = emit_groups(tc, &run->pb, tc->agg, &run->rows_emitted);
    }
    if (err == HP_OK) {
        err = hp_page_buffer_finish(&run->pb);
    } else {
        hp_page_buffer_flush(&run->pb);
    }
    tc->stats.rows_read += run->rows_read;
    tc->stats.rows_emitted += run->rows_emitted;
    tc->stats.rows_dropped += run->rows_dropped;
    tc->stats.records_in += run->rows_read + run->rows_dropped + run->skipped;
    return err;
}

/* Notes the record at rec as the one the run stops on */
static void mark_error_record(hp_run_t *run, const uint8_t *rec) {
    hp_transcoder_t *tc = run->tc;
    const hp_sliding_buffer_t *ring = &tc->ring_buf;
    size_t buffered_from_rec = ring->tail - (size_t)(rec - ring->data);
    tc->stats.error_record = run->rows_read + run->rows_dropped + run->skipped + 1;
    tc->stats.error_offset = tc->stats.bytes_in - buffered_from_rec;
}

typedef hp_error_t (*hp_next_record_fn)(hp_run_t *, const uint8_t *, size_t, bool, size_t *, size_t *);

/* The streaming loop shared by both input formats. Inlined with a constant
 * next_record, so each format gets its own copy without an indirect call
 * per record. Parse errors go to the fault-tolerance mode; anything else
 * stops the run. */
HP_INLINE hp_error_t run_records(hp_run_t *run, hp_next_record_fn next_record) {
    hp_transcoder_t *tc = run->tc;
    hp_sliding_buffer_t *ring = &tc->ring_buf;
    const bool skip_errors = tc->config.on_error != HP_ON_ERROR_FAIL;
    size_t available = 0, bytes_read = 0;
    const uint8_t *src;
    hp_error_t err;

    for (;;) {
        src = hp_sliding_buffer_read_ptr(ring, &available);
        if (available == 0) {
            if (ring->eof_reached) return HP_OK;
            if ((err = hp_sliding_buffer_refill(ring, &tc->in_reader, &bytes_read)) != HP_OK) return err;
            tc->stats.bytes_in += bytes_read;
            src = hp_sliding_buffer_read_ptr(ring, &available);
            if (available == 0 && ring->eof_reached) return HP_OK;
        }

        const bool is_eof = ring->eof_reached;
        bool progress = false, done = false;

        while (available > 0 && !done) {
            size_t consumed = 0, raw_len = 0;
            if (run->resyncing) {
                /* The rest of a record already counted as dropped */
                consumed = line_span(src, available);
                run->resyncing = src[consumed - 1] != '\n';
                quarantine_write(tc, src, consumed);
            } else if ((err = next_record(run, src, available, is_eof, &consumed, &raw_len)) != HP_OK) {
                if (!skip_errors) {
                    mark_error_record(run, src);
                    ring->head = (size_t)(src - ring->data);
                    return err;
                }
                consumed = line_span(src, available);
                quarantine_write(tc, src, consumed);
                run->rows_dropped++;
            } else if (consumed == 0) {
                break;
            } else if (run->row.field_count > 0 || (run->ndjson_in && raw_len > 0)) {
                bool is_data = true;
                if (!run->header_done && (!run->ndjson_in || tc->in_header.count > 0)) {
                    if ((err = start_stream(run)) != HP_OK) {
                        ring->head = (size_t)(src - ring->data);
                        return err;
                    }
                    is_data = run->ndjson_in;   /* a CSV header row is not data */
                    run->skipped += !is_data;
                }
                if (is_data) {
                    if (run->ndjson_in) follow_schema_growth(run);
                    run->rows_read++;
                    if (hp_filter_matches(&tc->filter, &run->row) &&
                        (err = deliver_record(run, raw_len, &done)) != HP_OK) {
                        ring->head = (size_t)(src - ring->data);
                        return err;
                    }
                }
            } else {
                run->skipped++;
            }
            progress = true;
            src += consumed;
            available -= consumed;
        }

        /* Fields point into the ring buffer, so it advances only here, once
         * every record parsed from this window has been consumed */
        ring->head = (size_t)(src - ring->data);
        if (done) return HP_OK;
        if (progress) continue;
        if (is_eof) return HP_OK;

        err = hp_sliding_buffer_refill(ring, &tc->in_reader, &bytes_read);
        if (err == HP_ERR_BUFFER_OVERFLOW && skip_errors) {
            /* A record larger than the buffer: drop what is buffered and
             * resynchronize at the next newline */
            quarantine_write(tc, src, available);
            run->rows_dropped++;
            ring->head = ring->tail;
            run->resyncing = true;
            continue;
        }
        if (err == HP_ERR_BUFFER_OVERFLOW) mark_error_record(run, src);
        if (err != HP_OK) return err;
        tc->stats.bytes_in += bytes_read;
    }
}

hp_error_t hp_transcoder_run(hp_transcoder_t *tc, FILE *in_stream, FILE *out_stream) {
    if (!tc || !in_stream || (!out_stream && !tc->config.sink)) return HP_ERR_INVALID_ARG;

    /* Plain stdio, or transparent inflate when the stream turns out to be
     * gzip. Parallel workers read exactly their span of the file. */
    hp_reader_init(&tc->in_reader, in_stream);
    if (tc->config.span_end) {
        tc->in_reader.limited = true;
        tc->in_reader.limit = tc->config.span_end;
    }

    double t0 = hp_clock_seconds();
    size_t bytes_read = 0;
    hp_error_t err = hp_sliding_buffer_refill(&tc->ring_buf, &tc->in_reader, &bytes_read);
    if (err != HP_OK) return err;
    tc->stats.bytes_in += bytes_read;

    size_t available = 0;
    const uint8_t *src = hp_sliding_buffer_read_ptr(&tc->ring_buf, &available);
    if (tc->config.in_format == HP_FORMAT_AUTO) {
        tc->config.in_format = hp_sniff_format(src, available);
        if (tc->config.in_format == HP_FORMAT_TSV) tc->csv_parser.delim = '\t';
    }
    if (tc->config.out_format == HP_FORMAT_AUTO) {
        tc->config.out_format = (tc->config.in_format == HP_FORMAT_NDJSON) ? HP_FORMAT_CSV
                                                                            : HP_FORMAT_NDJSON;
    }

    /* About 50 KiB of rows: on the heap rather than the stack of a worker */
    hp_run_t *run = (hp_run_t *)calloc(1, sizeof(*run));
    if (!run) return HP_ERR_NOMEM;
    run->tc = tc;
    run->pb = (hp_page_buffer_t){
        .data = tc->page_buf_data,
        .cap = tc->page_buf_cap,
        .stream = out_stream,
        .sink = tc->config.sink,
        .sink_ctx = tc->config.sink_ctx,
        .gz = tc->gz_out,
        .error = HP_OK,
    };
    run->parse_csv = g_hp_dispatch.csv_parse_row ? g_hp_dispatch.csv_parse_row
                                                 : hp_csv_parse_row_scalar;
    run->ndjson_in = (tc->config.in_format == HP_FORMAT_NDJSON);
    run->csv_out = (tc->config.out_format == HP_FORMAT_CSV || tc->config.out_format == HP_FORMAT_TSV);
    run->out_delim = (tc->config.out_format == HP_FORMAT_TSV) ? '\t' : ',';
    run->quote_clean = run->ndjson_in || tc->csv_parser.delim != run->out_delim;
    run->sorting = tc->sorter && !tc->agg;
    if (run->sorting && (err = capture_init(&run->capture, tc->page_buf_cap)) != HP_OK) {
        free(run);
        return err;
    }

    /* Parallel workers start mid-file with the header the coordinator read */
    tc->keymap_active = false;
    if (tc->config.preset_header) {
        const hp_header_t *pre = tc->config.preset_header;
        for (size_t i = 0; i < pre->count && i < HP_MAX_COLUMNS; i++) {
            tc->in_header.names[tc->in_header.count++] = strdup(pre->names[i] ? pre->names[i] : "");
        }
        err = hp_bind_stream_header(tc);
        if (err == HP_OK && run->ndjson_in && hp_keymap_build(&tc->keymap, &tc->in_header) == HP_OK) {
            tc->keymap_active = true;
        }
        run->header_done = true;
    }

    if (err == HP_OK) {
        err = run->ndjson_in ? run_records(run, next_ndjson_record)
                             : run_records(run, next_csv_record);
    }
    err = finish_run(run, err);

    /* --summary prints its table as the run's only output */
    if (err == HP_OK && tc->config.summary && tc->summary && out_stream) {
        hp_summary_print(tc->summary, out_stream);
    }
    tc->stats.elapsed_seconds = hp_clock_seconds() - t0;
    free(run);
    return err;
}

static hp_page_buffer_t output_page(hp_transcoder_t *tc, FILE *out_stream) {
    return (hp_page_buffer_t){
        .data = tc->page_buf_data,
        .cap = tc->page_buf_cap,
        .stream = out_stream,
        .sink = tc->config.sink,
        .sink_ctx = tc->config.sink_ctx,
        .gz = tc->gz_out,
        .error = HP_OK,
    };
}

hp_error_t hp_transcoder_write_groups(hp_transcoder_t *tc, const hp_agg_t *agg, FILE *out_stream) {
    hp_transcoder_config_t *cfg = &tc->config;
    if (cfg->out_format == HP_FORMAT_AUTO) {
        cfg->out_format = (cfg->in_format == HP_FORMAT_NDJSON) ? HP_FORMAT_CSV : HP_FORMAT_NDJSON;
    }
    hp_error_t err = hp_agg_header(agg, &tc->out_header);
    if (err != HP_OK) return err;
    hp_json_ser_bind_keys(&tc->json_ser, &tc->out_header);

    hp_page_buffer_t pb = output_page(tc, out_stream);
    uint64_t emitted = 0;
    err = emit_groups(tc, &pb, agg, &emitted);
    hp_error_t finish_err = hp_page_buffer_finish(&pb);
    tc->stats.rows_emitted += emitted;
    return err != HP_OK ? err : finish_err;
}

hp_error_t hp_transcoder_write_sorted(hp_transcoder_t *tc, hp_sorter_t *sorter, FILE *out_stream) {
    hp_page_buffer_t pb = output_page(tc, out_stream);
    uint64_t emitted = 0;
    hp_error_t err = hp_sorter_write(sorter, emit_line, &pb, &emitted);
    hp_error_t finish_err = hp_page_buffer_finish(&pb);
    tc->stats.rows_emitted += emitted;
    return err != HP_OK ? err : finish_err;
}
