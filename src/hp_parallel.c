/* Parallel chunked pipeline (--jobs N). POSIX implementation; on Windows
 * the entry point reports HP_ERR_UNSUPPORTED and callers fall back to the
 * single-threaded transcoder. */

/* fopencookie (worker output queues) is a GNU extension: it must be visible
 * before the first standard header is included. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hyperpipe/hp_parallel.h"

#if !defined(_WIN32)

#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HP_JOB_MAX 64
#define HP_QUEUE_PAGE_SIZE (256 * 1024)
/* Total output bytes all non-head workers may buffer while the coordinator
 * drains the head worker in chunk order. The cap is what bounds parallel
 * run memory independently of input size; the coordinator splits it evenly
 * between the non-head workers, so a 2-job run lets the trailing worker
 * buffer up to 64 MiB while 16 jobs get 4 MiB each. */
#define HP_QUEUE_TOTAL_BUDGET (64u * 1024 * 1024)

/* ---- bounded ordered page queue ------------------------------------------
 * Workers push serialized pages; the coordinator pops them in chunk order.
 * Pushing blocks while the queue holds more than its share of
 * HP_QUEUE_TOTAL_BUDGET, which caps total output memory independent of
 * input size. The stop flag breaks both sides out of their waits when
 * a run is aborted (worker failure, write failure, spawn failure). */

typedef struct hp_page_node_s {
    struct hp_page_node_s *next;
    size_t len;
    uint8_t *data;
} hp_page_node_t;

typedef struct hp_out_queue_s {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    hp_page_node_t *head;
    hp_page_node_t *tail;
    size_t bytes;
    size_t max_bytes;    /* this queue's share of HP_QUEUE_TOTAL_BUDGET */
    bool producer_done;
    bool stop;
} hp_out_queue_t;

static void queue_init(hp_out_queue_t *q, size_t max_bytes) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->head = q->tail = NULL;
    q->bytes = 0;
    q->max_bytes = max_bytes;
    q->producer_done = false;
    q->stop = false;
}

/* Frees any queued pages and the queue primitives themselves. Only safe
 * after the producing worker has been joined. */
static void queue_destroy(hp_out_queue_t *q) {
    hp_page_node_t *n = q->head;
    while (n) {
        hp_page_node_t *next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* stdio cookie write: splits the producer's page-buffer flushes into queue
 * nodes. A stop request turns into a short write, so stdio flags an error
 * and the worker's run unwinds; bytes delivered before the stop stay valid. */
static ssize_t queue_write(void *cookie, const char *buf, size_t size) {
    hp_out_queue_t *q = (hp_out_queue_t *)cookie;
    size_t off = 0;

    while (off < size) {
        pthread_mutex_lock(&q->mu);
        while (q->bytes >= q->max_bytes && !q->stop) {
            pthread_cond_wait(&q->not_full, &q->mu);
        }
        bool stop = q->stop;
        pthread_mutex_unlock(&q->mu);
        if (stop) {
            return (ssize_t)off;
        }

        size_t take = size - off;
        if (take > HP_QUEUE_PAGE_SIZE) take = HP_QUEUE_PAGE_SIZE;

        hp_page_node_t *node = (hp_page_node_t *)malloc(sizeof(*node));
        if (!node) return (ssize_t)off;
        node->data = (uint8_t *)malloc(take);
        if (!node->data) {
            free(node);
            return (ssize_t)off;
        }
        memcpy(node->data, buf + off, take);
        node->len = take;
        node->next = NULL;

        pthread_mutex_lock(&q->mu);
        if (q->tail) q->tail->next = node;
        else q->head = node;
        q->tail = node;
        q->bytes += take;
        pthread_mutex_unlock(&q->mu);
        pthread_cond_signal(&q->not_empty);

        off += take;
    }
    return (ssize_t)size;
}

static void queue_producer_done(hp_out_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->producer_done = true;
    pthread_mutex_unlock(&q->mu);
    pthread_cond_signal(&q->not_empty);
}

static void queue_request_stop(hp_out_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->stop = true;
    pthread_mutex_unlock(&q->mu);
    pthread_cond_signal(&q->not_full);
    pthread_cond_signal(&q->not_empty);
}

/* Pops one page, blocking until a page arrives, the producer finishes, or a
 * stop is requested. Returns NULL in the latter two cases. */
static hp_page_node_t *queue_pop(hp_out_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    while (q->head == NULL && !q->producer_done && !q->stop) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    hp_page_node_t *node = q->head;
    if (node) {
        q->head = node->next;
        if (!q->head) q->tail = NULL;
        q->bytes -= node->len;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mu);
    return node;
}

/* ---- worker --------------------------------------------------------------- */

typedef struct hp_worker_s {
    pthread_t thread;
    const char *input_path;
    uint64_t chunk_start;
    uint64_t chunk_end;
    hp_transcoder_config_t cfg;
    hp_header_t preset;              /* deep copy for workers > 0 */
    char *quarantine_part;           /* NULL when not quarantining */
    hp_out_queue_t queue;
    hp_transcoder_stats_t stats;
    hp_error_t err;
} hp_worker_t;

static void *hp_worker_main(void *arg) {
    hp_worker_t *w = (hp_worker_t *)arg;

    FILE *in = fopen(w->input_path, "rb");
    if (!in) {
        w->err = HP_ERR_IO;
        queue_producer_done(&w->queue);
        return NULL;
    }
    if (fseeko(in, (off_t)w->chunk_start, SEEK_SET) != 0) {
        fclose(in);
        w->err = HP_ERR_IO;
        queue_producer_done(&w->queue);
        return NULL;
    }

    cookie_io_functions_t io_funcs;
    memset(&io_funcs, 0, sizeof(io_funcs));
    io_funcs.write = queue_write;
    FILE *out = fopencookie(&w->queue, "w", io_funcs);
    if (!out) {
        fclose(in);
        w->err = HP_ERR_NOMEM;
        queue_producer_done(&w->queue);
        return NULL;
    }
    /* Unbuffered: the transcoder's 256 KiB page buffer already batches the
     * writes, so stdio buffering would only add a copy. */
    setvbuf(out, NULL, _IONBF, 0);

    hp_transcoder_t tc;
    w->err = hp_transcoder_init(&tc, &w->cfg);
    if (w->err == HP_OK) {
        w->err = hp_transcoder_run(&tc, in, out);
        w->stats = tc.stats;
        hp_transcoder_free(&tc);
    }

    fclose(out);
    fclose(in);
    queue_producer_done(&w->queue);
    return NULL;
}

/* ---- chunk planning --------------------------------------------------------- */

/* One sequential pass that records chunk starts and captures the header row
 * bytes. A boundary is the end of a line that begins outside a quoted
 * field: RFC 4180 doubles embedded quotes, so a running quote parity tells
 * whether a newline terminates a record. NDJSON input runs with quote_aware
 * = false: a raw newline always ends a line there, matching the
 * single-stream parser.
 *
 * The scan classifies bytes through the dispatchable plan_masks kernel
 * (newline/quote bitmaps per 64-byte chunk) instead of a per-byte branch,
 * because planning reads the whole file before the last worker can start:
 * its speed sets the floor for the entire pipeline. The scan also stops as
 * soon as every chunk boundary is known - the tail of the file belongs to
 * the last worker by construction, so reading it would be pure overhead. */
static hp_error_t hp_plan_chunks(FILE *f, uint64_t file_size, int jobs,
                                 bool quote_aware,
                                 uint64_t *starts, int *n_chunks,
                                 char **header_span, size_t *header_len) {
    const uint64_t target = file_size / (uint64_t)jobs;
    int n = 0;
    starts[n++] = 0;

    *header_span = NULL;
    *header_len = 0;
    size_t header_cap = 0;

    void (*masks)(const uint8_t *, size_t, uint64_t *, uint64_t *) =
        g_hp_dispatch.plan_masks ? g_hp_dispatch.plan_masks
                                 : hp_csv_plan_masks_scalar;

    uint64_t offset = 0;
    uint64_t last_boundary = 0;
    bool in_quotes = false;
    bool header_done = false;

    if (target == 0) {
        *n_chunks = 1;
        return HP_OK;
    }

    uint8_t buf[1 << 16];
    while (offset < file_size && n < jobs) {
        size_t want = sizeof(buf);
        if (file_size - offset < want) want = (size_t)(file_size - offset);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;

        /* Header capture: the first raw line, verbatim, independent of
         * quoting (the same span the single-stream path would call its
         * header row). The coordinator parses it once and hands it to
         * every worker > 0 as the preset schema. */
        if (!header_done) {
            const uint8_t *hl = (const uint8_t *)memchr(buf, '\n', got);
            size_t take = hl ? (size_t)(hl - buf) + 1 : got;
            if (*header_len + take + 1 > header_cap) {
                size_t new_cap = header_cap ? header_cap * 2 : 8192;
                while (*header_len + take + 1 > new_cap) new_cap *= 2;
                char *nb = (char *)realloc(*header_span, new_cap);
                if (!nb) {
                    free(*header_span);
                    *header_span = NULL;
                    return HP_ERR_NOMEM;
                }
                *header_span = nb;
                header_cap = new_cap;
            }
            memcpy(*header_span + *header_len, buf, take);
            *header_len += take;
            if (hl) header_done = true;
        }

        for (size_t base = 0; base < got && n < jobs; base += 64) {
            size_t cl = (got - base > 64) ? 64 : got - base;
            uint64_t nl_mask, qm_mask;
            masks(buf + base, cl, &nl_mask, &qm_mask);

            if (quote_aware && (qm_mask != 0 || in_quotes)) {
                if (qm_mask == 0) {
                    /* Parity is odd and the chunk holds no quotes: every
                     * byte lies inside a quoted field, so no record can
                     * end here and the entry parity carries over. */
                    continue;
                }

                /* Quoted region: walk quotes and newlines in position
                 * order. A local parity seeded from the chunk entry state
                 * decides whether a newline ends a record; the chunk exit
                 * parity is that walk's final value. */
                bool parity = in_quotes;
                uint64_t bits = nl_mask | qm_mask;
                while (bits) {
                    size_t rel = (size_t)__builtin_ctzll(bits);
                    bits &= bits - 1;
                    if (qm_mask & (1ull << rel)) {
                        parity = !parity;
                    } else if (!parity) {
                        uint64_t line_end = offset + base + rel + 1;
                        if (line_end - last_boundary >= target && n < jobs &&
                            line_end < file_size) {
                            starts[n++] = line_end;
                            last_boundary = line_end;
                        }
                    }
                }
                in_quotes = parity;
            } else {
                /* Common case: no quotes in this chunk and even parity -
                 * every newline ends a record. */
                while (nl_mask) {
                    size_t rel = (size_t)__builtin_ctzll(nl_mask);
                    nl_mask &= nl_mask - 1;
                    uint64_t line_end = offset + base + rel + 1;
                    if (line_end - last_boundary >= target && n < jobs &&
                        line_end < file_size) {
                        starts[n++] = line_end;
                        last_boundary = line_end;
                    }
                }
            }
        }
        offset += got;
    }

    *n_chunks = n;
    return HP_OK;
}

/* Parses the captured header span with the reference scalar parsers so that
 * workers > 0 can bind filter/projection before their first row. CSV/TSV
 * spans parse as one record; NDJSON spans resolve the key order of the
 * first line, exactly as the single-stream path's first row does. A header
 * the reference parsers reject reports HP_ERR_UNSUPPORTED so the CLI falls
 * back to the single-threaded path, whose own fault-tolerance mode owns the
 * offending record (fail, skip or quarantine). */
static hp_error_t hp_parse_header_span(hp_format_t fmt, const char *span,
                                       size_t len, hp_header_t *out) {
    if (fmt == HP_FORMAT_NDJSON) {
        hp_row_t probe;              /* scratch: only the key names matter */
        char arena[4096];
        memset(out, 0, sizeof(*out));
        hp_error_t err = hp_json_parse_line(span, len, out, &probe,
                                            arena, sizeof(arena));
        return (err == HP_OK) ? HP_OK : HP_ERR_UNSUPPORTED;
    }

    hp_csv_parser_t parser;
    hp_error_t err = hp_csv_parser_init(&parser, fmt, HP_SIMD_SCALAR);
    if (err != HP_OK) return err;

    hp_row_t row;
    size_t consumed = 0;
    err = hp_csv_parse_row_scalar(&parser, (const uint8_t *)span, len,
                                  &consumed, &row, true);
    if (err != HP_OK) err = HP_ERR_UNSUPPORTED;

    out->count = 0;
    for (size_t i = 0; err == HP_OK && i < row.field_count && i < HP_MAX_COLUMNS; i++) {
        char name[256];
        size_t flen = row.fields[i].len < sizeof(name) - 1
                          ? row.fields[i].len : sizeof(name) - 1;
        memcpy(name, row.fields[i].ptr, flen);
        name[flen] = '\0';
        out->names[out->count] = strdup(name);
        if (!out->names[out->count]) err = HP_ERR_NOMEM;
        else out->count++;
    }
    hp_csv_parser_free(&parser);
    return err;
}

static void hp_free_header(hp_header_t *h) {
    for (size_t i = 0; i < h->count; i++) {
        free(h->names[i]);
    }
    h->count = 0;
}

/* Appends each worker's quarantine part file to the user-visible quarantine
 * destination, in chunk order, and removes the parts. */
static void hp_merge_quarantine(const hp_transcoder_config_t *base,
                                const hp_worker_t *workers, int n_chunks) {
    if (!base->quarantine_path) return;
    FILE *dst = fopen(base->quarantine_path, "wb");
    if (!dst) return;
    for (int i = 0; i < n_chunks; i++) {
        if (!workers[i].quarantine_part) continue;
        FILE *src = fopen(workers[i].quarantine_part, "rb");
        if (src) {
            char cp[65536];
            size_t r;
            while ((r = fread(cp, 1, sizeof(cp), src)) > 0) {
                fwrite(cp, 1, r, dst);
            }
            fclose(src);
        }
        remove(workers[i].quarantine_part);
    }
    fclose(dst);
}

/* ---- coordinator ------------------------------------------------------------- */

hp_error_t hp_parallel_run(const hp_transcoder_config_t *base_cfg,
                           const char *input_path,
                           FILE *out_stream,
                           int jobs,
                           hp_transcoder_stats_t *stats_out) {
    if (!base_cfg || !input_path || !out_stream || jobs <= 1 || !stats_out) {
        return HP_ERR_INVALID_ARG;
    }
    if (jobs > HP_JOB_MAX) return HP_ERR_INVALID_ARG;

    FILE *probe = fopen(input_path, "rb");
    if (!probe) return HP_ERR_IO;

    /* Regular file only: chunk planning and worker positioning both need
     * seekable byte offsets. */
    struct stat st;
    if (fstat(fileno(probe), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(probe);
        return HP_ERR_INVALID_ARG;
    }

    /* Input sniffing must see the file from offset 0: the gzip magic check
     * and the format sniff read the same prefix, so a 2-byte magic peek
     * before the sample would shift the sample past the leading `{` of an
     * NDJSON file and misclassify it as CSV. */
    uint8_t sample[4096];
    size_t have = fread(sample, 1, sizeof(sample), probe);
    bool is_gzip = (have >= 2 && sample[0] == 0x1F && sample[1] == 0x8B);

    hp_format_t in_fmt = base_cfg->in_format;
    if (in_fmt == HP_FORMAT_AUTO) {
        in_fmt = hp_sniff_format(sample, have);
    }
    if (is_gzip) {
        fclose(probe);
        return HP_ERR_UNSUPPORTED;
    }

    uint64_t file_size = (uint64_t)st.st_size;
    if (fseeko(probe, 0, SEEK_SET) != 0) {
        fclose(probe);
        return HP_ERR_IO;
    }

    /* The boundary scan consumes the dispatch table; initialize it for the
     * requested mode even when hp_parallel_run is the first library entry
     * the process calls (the CLI initializes it before transcoding, and
     * workers re-init under the same mutex). */
    hp_dispatch_init(base_cfg->simd_mode);

    bool quote_aware = (in_fmt == HP_FORMAT_CSV);
    uint64_t *starts = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)jobs);
    if (!starts) {
        fclose(probe);
        return HP_ERR_NOMEM;
    }

    char *header_span = NULL;
    size_t header_len = 0;
    int n_chunks = 1;
    hp_error_t err = hp_plan_chunks(probe, file_size, jobs, quote_aware,
                                    starts, &n_chunks,
                                    &header_span, &header_len);
    fclose(probe);
    if (err != HP_OK) {
        free(starts);
        return err;
    }

    /* Resolved output format, mirroring hp_transcoder_run's rule: CSV/TSV
     * input defaults to NDJSON output, NDJSON input defaults to CSV. */
    hp_transcoder_config_t template_cfg = *base_cfg;
    template_cfg.in_format = in_fmt;
    if (template_cfg.out_format == HP_FORMAT_AUTO) {
        template_cfg.out_format = (in_fmt == HP_FORMAT_NDJSON)
                                      ? HP_FORMAT_CSV
                                      : HP_FORMAT_NDJSON;
    }

    hp_error_t setup_err = HP_OK;
    hp_header_t header;
    memset(&header, 0, sizeof(header));
    bool have_header = false;
    if (n_chunks > 1 && header_span) {
        err = hp_parse_header_span(in_fmt, header_span, header_len, &header);
        if (err == HP_OK) {
            have_header = true;
        } else {
            /* A header the reference parsers reject degrades to the
             * single-threaded path instead of failing the run. */
            setup_err = HP_ERR_UNSUPPORTED;
        }
    }

    hp_worker_t *workers = NULL;
    if (setup_err == HP_OK) {
        workers = (hp_worker_t *)calloc((size_t)n_chunks, sizeof(hp_worker_t));
        if (!workers) setup_err = HP_ERR_NOMEM;
    }

    for (int i = 0; setup_err == HP_OK && i < n_chunks; i++) {
        hp_worker_t *w = &workers[i];
        w->input_path = input_path;
        w->chunk_start = starts[i];
        w->chunk_end = (i + 1 < n_chunks) ? starts[i + 1] : file_size;
        w->err = HP_OK;
        queue_init(&w->queue, n_chunks > 1
                                  ? HP_QUEUE_TOTAL_BUDGET / (size_t)(n_chunks - 1)
                                  : HP_QUEUE_TOTAL_BUDGET);

        w->cfg = template_cfg;
        w->cfg.span_end = w->chunk_end - w->chunk_start;
        w->cfg.limit_rows = 0;   /* row limits are a coordinator decision */
        if (i > 0 && have_header) {
            /* Deep copy: every worker owns its header strings. */
            for (size_t k = 0; k < header.count; k++) {
                w->preset.names[w->preset.count] = strdup(header.names[k]);
                if (!w->preset.names[w->preset.count]) {
                    setup_err = HP_ERR_NOMEM;
                    break;
                }
                w->preset.count++;
            }
            w->cfg.preset_header = &w->preset;
        }
        if (setup_err == HP_OK &&
            base_cfg->on_error == HP_ON_ERROR_QUARANTINE && base_cfg->quarantine_path) {
            size_t part_len = strlen(base_cfg->quarantine_path) + 32;
            w->quarantine_part = (char *)malloc(part_len);
            if (!w->quarantine_part) {
                setup_err = HP_ERR_NOMEM;
            } else {
                snprintf(w->quarantine_part, part_len, "%s.part%d",
                         base_cfg->quarantine_path, i);
                w->cfg.quarantine_path = w->quarantine_part;
            }
        }
    }

    int started = 0;
    for (; setup_err == HP_OK && started < n_chunks; started++) {
        if (pthread_create(&workers[started].thread, NULL,
                           hp_worker_main, &workers[started]) != 0) {
            setup_err = HP_ERR_NOMEM;
            break;
        }
    }

    if (setup_err != HP_OK) {
        /* Unwind deterministically: stop whoever is running, drain their
         * queues, join, then free everything. workers stays NULL when the
         * failure happened before allocation (bad header span, OOM). */
        if (workers) {
            for (int i = 0; i < started; i++) {
                queue_request_stop(&workers[i].queue);
            }
            for (int i = 0; i < started; i++) {
                pthread_join(workers[i].thread, NULL);
            }
            for (int i = 0; i < n_chunks; i++) {
                queue_destroy(&workers[i].queue);
                free(workers[i].quarantine_part);
                hp_free_header(&workers[i].preset);
            }
        }
        hp_free_header(&header);
        free(workers);
        free(starts);
        free(header_span);
        return setup_err;
    }

    /* Ordered drain: chunk order is output order, so appending each
     * worker's pages in sequence reproduces the single-threaded byte
     * stream exactly. An abort (worker error or write failure) stops the
     * remaining workers early and discards their pages. */
    hp_error_t run_err = HP_OK;
    bool aborted = false;
    for (int i = 0; i < n_chunks; i++) {
        hp_worker_t *w = &workers[i];
        if (aborted) queue_request_stop(&w->queue);

        hp_page_node_t *node;
        while ((node = queue_pop(&w->queue)) != NULL) {
            if (!aborted && fwrite(node->data, 1, node->len, out_stream) != node->len) {
                run_err = HP_ERR_IO;
                aborted = true;
                queue_request_stop(&w->queue);
            }
            free(node->data);
            free(node);
        }
        if (w->err != HP_OK) {
            if (run_err == HP_OK) run_err = w->err;
            aborted = true;
        }
    }

    for (int i = 0; i < n_chunks; i++) {
        pthread_join(workers[i].thread, NULL);
    }

    if (base_cfg->on_error == HP_ON_ERROR_QUARANTINE) {
        hp_merge_quarantine(base_cfg, workers, n_chunks);
    }

    memset(stats_out, 0, sizeof(*stats_out));
    for (int i = 0; i < n_chunks; i++) {
        stats_out->rows_read += workers[i].stats.rows_read;
        stats_out->rows_emitted += workers[i].stats.rows_emitted;
        stats_out->rows_dropped += workers[i].stats.rows_dropped;
        stats_out->bytes_in += workers[i].stats.bytes_in;
        stats_out->bytes_out += workers[i].stats.bytes_out;
        if (workers[i].stats.elapsed_seconds > stats_out->elapsed_seconds) {
            stats_out->elapsed_seconds = workers[i].stats.elapsed_seconds;
        }
    }

    for (int i = 0; i < n_chunks; i++) {
        queue_destroy(&workers[i].queue);
        free(workers[i].quarantine_part);
        hp_free_header(&workers[i].preset);
    }
    hp_free_header(&header);
    free(workers);
    free(starts);
    free(header_span);

    return run_err;
}

#else /* _WIN32 */

hp_error_t hp_parallel_run(const hp_transcoder_config_t *base_cfg,
                           const char *input_path,
                           FILE *out_stream,
                           int jobs,
                           hp_transcoder_stats_t *stats_out) {
    (void)base_cfg;
    (void)input_path;
    (void)out_stream;
    (void)jobs;
    (void)stats_out;
    return HP_ERR_UNSUPPORTED;
}

#endif /* !_WIN32 */
