/* Parallel chunked pipeline (--jobs N).
 *
 * The input file is cut at fixed offsets into ranges a few MiB long, several
 * per thread so the work balances. Each range becomes a chunk of whole
 * records: the chunk starts at the first record that begins inside its
 * range. In NDJSON that is the byte after the first newline. In CSV a
 * newline ends a record only outside quotes, so a first pass counts the
 * quotes of every range in parallel; a prefix XOR of the counts gives the
 * quoting state at each cut, and from there the next record start is one
 * short scan away.
 *
 * Worker threads then take chunks in file order and transcode each into its
 * own memory buffer. The calling thread writes finished chunks strictly in
 * order, which reproduces the single-threaded byte stream exactly. At most
 * `window` chunks are claimed ahead of the writer, which bounds memory
 * independently of the input size. */

#include "hyperpipe/hp_parallel.h"
#include "hp_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
    #include <io.h>
#endif

#define HP_JOB_MAX 64
#define HP_RANGE_MIN (1u << 20)
#define HP_RANGE_MAX (8u << 20)
#define HP_RANGES_PER_JOB 8
#define HP_READ_BLOCK (256 * 1024)
#define HP_HEADER_MAX (64u << 20)
#define HP_AUTO_MIN_SIZE (16u << 20)
#define HP_AUTO_MAX_JOBS 16

/* ---- file access ------------------------------------------------------------ */

/* Size of a regular file; false for pipes, devices and errors. Windows needs
 * the explicit 64-bit calls: its off_t and long stay 32 bits wide. */
static bool regular_file_size(FILE *f, uint64_t *size) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG) return false;
#else
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) return false;
#endif
    *size = (uint64_t)st.st_size;
    return true;
}

static bool seek_to(FILE *f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

/* Writes at an absolute offset without using a shared file position, so
 * several threads can fill one output file at the same time. */
static bool write_at(FILE *f, uint64_t offset, const uint8_t *data, size_t len) {
#if defined(_WIN32)
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    while (len > 0) {
        DWORD part = len > (1u << 30) ? (1u << 30) : (DWORD)len;
        OVERLAPPED at;
        memset(&at, 0, sizeof(at));
        at.Offset = (DWORD)offset;
        at.OffsetHigh = (DWORD)(offset >> 32);
        DWORD done = 0;
        if (!WriteFile(h, data, part, &done, &at) || done == 0) return false;
        data += done;
        len -= done;
        offset += done;
    }
#else
    int fd = fileno(f);
    while (len > 0) {
        ssize_t done = pwrite(fd, data, len, (off_t)offset);
        if (done <= 0) return false;
        data += done;
        len -= (size_t)done;
        offset += (uint64_t)done;
    }
#endif
    return true;
}

/* Whether workers may write their chunks straight into the output, and at
 * which offset the output starts. Only a regular file qualifies, and only
 * on POSIX: NTFS zero-fills the gap whenever a write lands past the file's
 * valid data length, so out-of-order chunks would write most of the file
 * twice. Everything else gets the ordered writer. */
static bool can_write_at_offsets(FILE *out, uint64_t *base) {
#if defined(_WIN32)
    (void)out;
    (void)base;
    return false;
#else
    uint64_t size;
    off_t pos = ftello(out);
    if (pos < 0 || !regular_file_size(out, &size)) return false;
    *base = (uint64_t)pos;
    return true;
#endif
}

static double now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- record boundaries ---------------------------------------------------------- */

/* Offset just past the first newline at or after `pos` that lies outside
 * quotes, given the quoting state before `pos`; the file size when the
 * record runs to the end of the file. */
static uint64_t scan_to_record_end(FILE *f, uint64_t pos, bool in_quotes,
                                   bool quote_aware, uint64_t file_size) {
    uint8_t block[4096];
    if (!seek_to(f, pos)) return file_size;
    size_t got;
    while ((got = fread(block, 1, sizeof(block), f)) > 0) {
        for (size_t k = 0; k < got; k++, pos++) {
            if (quote_aware && block[k] == '"') {
                in_quotes = !in_quotes;
            } else if (block[k] == '\n' && !in_quotes) {
                return pos + 1;
            }
        }
    }
    return file_size;
}

/* Offset of the first record that starts at or after `from`, given the
 * quoting state before `from`. The scan begins one byte early because a
 * newline right before `from` makes `from` itself a record start. */
static uint64_t next_record_start(FILE *f, uint64_t from, bool in_quotes,
                                  bool quote_aware, uint64_t file_size) {
    if (from == 0) return 0;
    uint8_t prev;
    if (!seek_to(f, from - 1) || fread(&prev, 1, 1, f) != 1) return file_size;
    if (quote_aware && prev == '"') in_quotes = !in_quotes;
    return scan_to_record_end(f, from - 1, in_quotes, quote_aware, file_size);
}

/* ---- growable output buffer ------------------------------------------------- */

typedef struct hp_buf_s {
    uint8_t *data;
    size_t len;
    size_t cap;
} hp_buf_t;

/* Transcoder sink: appends a page to the chunk's buffer. A short count on
 * allocation failure makes the transcoder report an I/O error. */
static size_t buf_append(void *ctx, const void *data, size_t len) {
    hp_buf_t *b = (hp_buf_t *)ctx;
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 1u << 20;
        while (cap < b->len + len) cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(b->data, cap);
        if (!grown) return 0;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return len;
}

static void buf_free(hp_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* ---- shared state ------------------------------------------------------------ */

typedef struct hp_chunk_s {
    uint64_t cut;              /* raw range start; records start at or after it */
    bool odd_quotes;           /* the range holds an odd number of quotes */
    bool cut_in_quotes;        /* quoting state just before `cut` */
    hp_buf_t out;
    hp_buf_t quarantine;
    hp_transcoder_stats_t stats;
    hp_error_t err;
    bool done;
    bool placed;               /* `offset` is final: the worker may write */
    uint64_t offset;
} hp_chunk_t;

typedef struct hp_pool_s {
    const char *path;
    uint64_t file_size;
    bool quote_aware;
    hp_transcoder_config_t cfg;     /* template for every chunk */
    const hp_header_t *header;      /* bound by chunks after the first */
    FILE *out;
    bool positional;                /* workers write their own chunks */

    hp_chunk_t *chunks;
    size_t n_chunks;

    hp_mutex_t mu;
    hp_cond_t progress;             /* a chunk finished or was written */
    size_t next;                    /* next range or chunk to claim */
    size_t written;                 /* chunks already written out */
    size_t window;                  /* 0: no limit (quote counting) */
    bool stop;

    /* Written-out buffers kept for reuse: their pages are already mapped,
     * so later chunks skip both the growth copies and the page faults. */
    hp_buf_t spare[HP_JOB_MAX + 2];
    size_t n_spare;
} hp_pool_t;

/* Claims the next range or chunk. A claimed chunk also takes a spare output
 * buffer when one is available. */
static bool claim(hp_pool_t *pool, size_t *index) {
    hp_mutex_lock(&pool->mu);
    while (pool->window && !pool->stop && pool->next < pool->n_chunks &&
           pool->next >= pool->written + pool->window) {
        hp_cond_wait(&pool->progress, &pool->mu);
    }
    bool ok = !pool->stop && pool->next < pool->n_chunks;
    if (ok) {
        *index = pool->next++;
        if (pool->window && pool->n_spare > 0) {
            pool->chunks[*index].out = pool->spare[--pool->n_spare];
        }
    }
    hp_mutex_unlock(&pool->mu);
    return ok;
}

/* Returns a written-out buffer to the spare list, or frees it when full */
static void recycle(hp_pool_t *pool, hp_buf_t *b) {
    b->len = 0;
    if (pool->n_spare < sizeof(pool->spare) / sizeof(pool->spare[0])) {
        pool->spare[pool->n_spare++] = *b;
        b->data = NULL;
        b->cap = 0;
    } else {
        buf_free(b);
    }
}

static int start_threads(hp_pool_t *pool, int jobs, hp_thread_fn fn, hp_thread_t *threads) {
    int started = 0;
    pool->next = 0;
    while (started < jobs && hp_thread_create(&threads[started], fn, pool)) started++;
    return started;
}

/* ---- phase 1: quote parity per range ------------------------------------------ */

/* Written as a plain loop on purpose: compilers vectorize it into byte
 * compares and subtracts, which runs at memory speed. */
static size_t count_quotes(const uint8_t *p, size_t n) {
    size_t quotes = 0;
    for (size_t i = 0; i < n; i++) quotes += (p[i] == '"');
    return quotes;
}

static void *count_quotes_main(void *arg) {
    hp_pool_t *pool = (hp_pool_t *)arg;
    FILE *f = fopen(pool->path, "rb");
    uint8_t *block = (uint8_t *)malloc(HP_READ_BLOCK);
    size_t i;
    while (claim(pool, &i)) {
        hp_chunk_t *c = &pool->chunks[i];
        uint64_t end = (i + 1 < pool->n_chunks) ? pool->chunks[i + 1].cut : pool->file_size;
        uint64_t left = end - c->cut;
        size_t quotes = 0;
        if (!f || !block || !seek_to(f, c->cut)) {
            c->err = HP_ERR_IO;
            left = 0;
        }
        while (left > 0) {
            size_t want = left < HP_READ_BLOCK ? (size_t)left : HP_READ_BLOCK;
            size_t got = fread(block, 1, want, f);
            if (got == 0) {
                c->err = HP_ERR_IO;
                break;
            }
            quotes += count_quotes(block, got);
            left -= got;
        }
        c->odd_quotes = quotes & 1;
    }
    free(block);
    if (f) fclose(f);
    return NULL;
}

/* ---- phase 2: transcode chunks ------------------------------------------------- */

static void *transcode_main(void *arg) {
    hp_pool_t *pool = (hp_pool_t *)arg;
    FILE *f = fopen(pool->path, "rb");
    size_t i;
    while (claim(pool, &i)) {
        hp_chunk_t *c = &pool->chunks[i];
        c->err = f ? HP_OK : HP_ERR_IO;

        uint64_t start = 0, end = pool->file_size;
        if (c->err == HP_OK) {
            start = next_record_start(f, c->cut, c->cut_in_quotes,
                                      pool->quote_aware, pool->file_size);
            if (i + 1 < pool->n_chunks) {
                const hp_chunk_t *next = &pool->chunks[i + 1];
                end = next_record_start(f, next->cut, next->cut_in_quotes,
                                        pool->quote_aware, pool->file_size);
            }
        }

        /* A record longer than a range leaves the ranges it covers empty */
        if (c->err == HP_OK && end > start) {
            hp_transcoder_config_t cfg = pool->cfg;
            cfg.span_end = end - start;
            cfg.limit_rows = 0;
            cfg.lock_schema = true;
            cfg.preset_header = (i > 0) ? pool->header : NULL;
            cfg.sink = buf_append;
            cfg.sink_ctx = &c->out;
            if (cfg.on_error == HP_ON_ERROR_QUARANTINE) {
                cfg.quarantine_path = NULL;
                cfg.quarantine_sink = buf_append;
                cfg.quarantine_ctx = &c->quarantine;
            }

            hp_transcoder_t tc;
            if (!seek_to(f, start)) {
                c->err = HP_ERR_IO;
            } else if ((c->err = hp_transcoder_init(&tc, &cfg)) == HP_OK) {
                c->err = hp_transcoder_run(&tc, f, NULL);
                c->stats = tc.stats;
                hp_transcoder_free(&tc);
            }
        }

        hp_mutex_lock(&pool->mu);
        c->done = true;
        if (c->err != HP_OK) pool->stop = true;
        hp_cond_broadcast(&pool->progress);

        /* Positional output: once the writer has summed the sizes of the
         * chunks before this one, the chunk lands at its offset directly */
        if (pool->positional && c->err == HP_OK) {
            while (!c->placed && !pool->stop) hp_cond_wait(&pool->progress, &pool->mu);
            if (c->placed) {
                hp_mutex_unlock(&pool->mu);
                bool ok = write_at(pool->out, c->offset, c->out.data, c->out.len);
                hp_mutex_lock(&pool->mu);
                if (!ok) {
                    c->err = HP_ERR_IO;
                    pool->stop = true;
                    hp_cond_broadcast(&pool->progress);
                }
            }
            recycle(pool, &c->out);
        }
        hp_mutex_unlock(&pool->mu);
    }
    if (f) fclose(f);
    return NULL;
}

/* ---- header ---------------------------------------------------------------------- */

/* Parses the first record with the reference scalar parsers so chunks after
 * the first can bind filter and projection before their first row. A header
 * the parsers reject reports HP_ERR_UNSUPPORTED: the caller then runs the
 * single-threaded path, whose fault-tolerance mode owns that record. */
static hp_error_t parse_header(FILE *f, uint64_t file_size, hp_format_t fmt,
                               bool quote_aware, hp_header_t *out) {
    memset(out, 0, sizeof(*out));
    uint64_t len = scan_to_record_end(f, 0, false, quote_aware, file_size);
    if (len == 0 || len > HP_HEADER_MAX) return HP_ERR_UNSUPPORTED;

    char *span = (char *)malloc((size_t)len);
    hp_row_t *row = (hp_row_t *)malloc(sizeof(*row));
    hp_error_t err = (span && row) ? HP_OK : HP_ERR_NOMEM;
    if (err == HP_OK && (!seek_to(f, 0) || fread(span, 1, (size_t)len, f) != len)) {
        err = HP_ERR_IO;
    }

    if (err == HP_OK && fmt == HP_FORMAT_NDJSON) {
        char *arena = (char *)malloc((size_t)len);
        err = arena ? hp_json_parse_line(span, (size_t)len, out, row, arena, (size_t)len)
                    : HP_ERR_NOMEM;
        free(arena);
    } else if (err == HP_OK) {
        hp_csv_parser_t parser;
        size_t consumed = 0;
        err = hp_csv_parser_init(&parser, fmt, HP_SIMD_SCALAR);
        if (err == HP_OK) {
            err = hp_csv_parse_row_scalar(&parser, (const uint8_t *)span, (size_t)len,
                                          &consumed, row, true);
        }
        for (size_t i = 0; err == HP_OK && i < row->field_count; i++) {
            char *name = (char *)malloc(row->fields[i].len + 1);
            if (!name) {
                err = HP_ERR_NOMEM;
                break;
            }
            memcpy(name, row->fields[i].ptr, row->fields[i].len);
            name[row->fields[i].len] = '\0';
            out->names[out->count++] = name;
        }
        hp_csv_parser_free(&parser);
    }
    free(span);
    free(row);
    if (err != HP_OK && err != HP_ERR_NOMEM && err != HP_ERR_IO) err = HP_ERR_UNSUPPORTED;
    return err;
}

/* ---- coordinator ------------------------------------------------------------------ */

int hp_parallel_auto_jobs(const char *input_path) {
    FILE *f = fopen(input_path, "rb");
    if (!f) return 1;
    uint64_t size = 0;
    bool regular = regular_file_size(f, &size);
    fclose(f);
    if (!regular || size < HP_AUTO_MIN_SIZE) return 1;

    int n = hp_cpu_count();
    if (n > HP_AUTO_MAX_JOBS) n = HP_AUTO_MAX_JOBS;
    return n > 1 ? n : 1;
}

hp_error_t hp_parallel_run(const hp_transcoder_config_t *base_cfg,
                           const char *input_path,
                           FILE *out_stream,
                           int jobs,
                           hp_transcoder_stats_t *stats_out) {
    if (!base_cfg || !input_path || !out_stream || jobs <= 1 || !stats_out) {
        return HP_ERR_INVALID_ARG;
    }
    if (jobs > HP_JOB_MAX) jobs = HP_JOB_MAX;
    double t0 = now_seconds();

    FILE *probe = fopen(input_path, "rb");
    if (!probe) return HP_ERR_IO;

    uint64_t file_size = 0;
    if (!regular_file_size(probe, &file_size)) {
        fclose(probe);
        return HP_ERR_INVALID_ARG;
    }

    /* The format sniff and the gzip check read the same prefix from offset 0 */
    uint8_t sample[4096];
    size_t have = fread(sample, 1, sizeof(sample), probe);
    if (have >= 2 && sample[0] == 0x1F && sample[1] == 0x8B) {
        fclose(probe);
        return HP_ERR_UNSUPPORTED;
    }
    hp_format_t in_fmt = base_cfg->in_format;
    if (in_fmt == HP_FORMAT_AUTO) in_fmt = hp_sniff_format(sample, have);

    /* Chunk cuts assume balanced quotes. A stray quote in dirty CSV shifts
     * them, while the sequential parser resynchronizes at the next line in
     * skip and quarantine modes, so those modes keep the sequential path.
     * NDJSON records always end at a newline and stay parallel. */
    if (in_fmt != HP_FORMAT_NDJSON && base_cfg->on_error != HP_ON_ERROR_FAIL) {
        fclose(probe);
        return HP_ERR_UNSUPPORTED;
    }

    /* The quote count uses the dispatch table: initialize it even when this
     * is the first library call of the process. */
    hp_dispatch_init(base_cfg->simd_mode);

    hp_pool_t pool;
    memset(&pool, 0, sizeof(pool));
    pool.path = input_path;
    pool.file_size = file_size;
    pool.quote_aware = (in_fmt != HP_FORMAT_NDJSON);

    /* Output format resolves as in hp_transcoder_run: CSV/TSV input
     * defaults to NDJSON output, NDJSON input to CSV. */
    pool.cfg = *base_cfg;
    pool.cfg.in_format = in_fmt;
    if (pool.cfg.out_format == HP_FORMAT_AUTO) {
        pool.cfg.out_format = (in_fmt == HP_FORMAT_NDJSON) ? HP_FORMAT_CSV : HP_FORMAT_NDJSON;
    }

    hp_header_t header;
    hp_error_t err = parse_header(probe, file_size, in_fmt, pool.quote_aware, &header);
    fclose(probe);
    if (err != HP_OK) {
        hp_header_free(&header);
        return err;
    }
    pool.header = &header;

    uint64_t range = file_size / ((uint64_t)jobs * HP_RANGES_PER_JOB);
    if (range < HP_RANGE_MIN) range = HP_RANGE_MIN;
    if (range > HP_RANGE_MAX) range = HP_RANGE_MAX;
    pool.n_chunks = (size_t)((file_size + range - 1) / range);

    pool.chunks = (hp_chunk_t *)calloc(pool.n_chunks, sizeof(hp_chunk_t));
    if (!pool.chunks) {
        hp_header_free(&header);
        return HP_ERR_NOMEM;
    }
    for (size_t i = 0; i < pool.n_chunks; i++) pool.chunks[i].cut = (uint64_t)i * range;

    hp_mutex_init(&pool.mu);
    hp_cond_init(&pool.progress);
    hp_thread_t threads[HP_JOB_MAX];

    if (pool.quote_aware && pool.n_chunks > 1) {
        int started = start_threads(&pool, jobs, count_quotes_main, threads);
        for (int i = 0; i < started; i++) hp_thread_join(threads[i]);
        if (started == 0) err = HP_ERR_NOMEM;

        bool in_quotes = false;
        for (size_t i = 0; i < pool.n_chunks; i++) {
            if (pool.chunks[i].err != HP_OK) err = pool.chunks[i].err;
            pool.chunks[i].cut_in_quotes = in_quotes;
            in_quotes ^= pool.chunks[i].odd_quotes;
        }
    }

    FILE *quarantine = NULL;
    if (err == HP_OK && pool.cfg.on_error == HP_ON_ERROR_QUARANTINE && pool.cfg.quarantine_path) {
        quarantine = fopen(pool.cfg.quarantine_path, "wb");
        if (!quarantine) err = HP_ERR_IO;
    }

    /* Workers write their own chunks when the output allows it; otherwise
     * this thread writes them below, in chunk order. */
    uint64_t out_base = 0;
    fflush(out_stream);
    pool.out = out_stream;
    pool.positional = can_write_at_offsets(out_stream, &out_base);
    uint64_t out_end = out_base;

    pool.window = (size_t)jobs + 2;
    int started = 0;
    if (err == HP_OK) {
        started = start_threads(&pool, jobs, transcode_main, threads);
        if (started == 0) err = HP_ERR_NOMEM;
    }

    memset(stats_out, 0, sizeof(*stats_out));
    for (size_t i = 0; err == HP_OK && i < pool.n_chunks; i++) {
        hp_chunk_t *c = &pool.chunks[i];
        hp_mutex_lock(&pool.mu);
        while (!c->done) hp_cond_wait(&pool.progress, &pool.mu);
        hp_mutex_unlock(&pool.mu);

        if (c->err != HP_OK) {
            err = c->err;
        } else if (pool.positional) {
            c->offset = out_end;
            out_end += c->out.len;
        } else if (fwrite(c->out.data, 1, c->out.len, out_stream) != c->out.len) {
            err = HP_ERR_IO;
        }
        if (err == HP_OK && quarantine &&
            fwrite(c->quarantine.data, 1, c->quarantine.len, quarantine) != c->quarantine.len) {
            err = HP_ERR_IO;
        }

        stats_out->rows_read += c->stats.rows_read;
        stats_out->rows_emitted += c->stats.rows_emitted;
        stats_out->rows_dropped += c->stats.rows_dropped;
        stats_out->bytes_in += c->stats.bytes_in;
        stats_out->bytes_out += c->stats.bytes_out;
        buf_free(&c->quarantine);

        hp_mutex_lock(&pool.mu);
        if (pool.positional) {
            c->placed = true;
        } else {
            recycle(&pool, &c->out);
        }
        pool.written = i + 1;
        if (err != HP_OK) pool.stop = true;
        hp_cond_broadcast(&pool.progress);
        hp_mutex_unlock(&pool.mu);
    }

    hp_mutex_lock(&pool.mu);
    pool.stop = true;
    hp_cond_broadcast(&pool.progress);
    hp_mutex_unlock(&pool.mu);
    for (int i = 0; i < started; i++) hp_thread_join(threads[i]);
    for (size_t i = 0; err == HP_OK && i < pool.n_chunks; i++) err = pool.chunks[i].err;
    if (pool.positional && !seek_to(out_stream, out_end) && err == HP_OK) err = HP_ERR_IO;

    if (quarantine) fclose(quarantine);
    for (size_t i = 0; i < pool.n_chunks; i++) {
        buf_free(&pool.chunks[i].out);
        buf_free(&pool.chunks[i].quarantine);
    }
    for (size_t i = 0; i < pool.n_spare; i++) buf_free(&pool.spare[i]);
    free(pool.chunks);
    hp_cond_destroy(&pool.progress);
    hp_mutex_destroy(&pool.mu);
    hp_header_free(&header);
    stats_out->elapsed_seconds = now_seconds() - t0;
    return err;
}
