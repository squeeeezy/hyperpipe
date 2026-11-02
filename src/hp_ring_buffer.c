#include "hyperpipe/hp_ring_buffer.h"

/* Reads through the reader, engaging transparent gzip decompression when
 * the stream starts with the gzip magic. Plain streams cost one fread per
 * call plus a two-byte check on the first. */
hp_error_t hp_reader_read(hp_reader_t *r, uint8_t *dst, size_t cap, size_t *n_out) {
    *n_out = 0;
    if (!r || !dst || cap == 0) return HP_ERR_INVALID_ARG;
    if (r->eof) return HP_OK;

    if (!r->gz_active) {
        /* A byte limit marks a span of a plain file (parallel workers). A
         * span may start anywhere in the data, so it is never sniffed for
         * the gzip magic. */
        if (r->limited) {
            if (cap > r->limit) cap = (size_t)r->limit;
            r->sniffed = true;
        }
        size_t n = cap ? fread(dst, 1, cap, r->stream) : 0;
        if (r->limited) r->limit -= n;
        if (n == 0) {
            if (ferror(r->stream)) return HP_ERR_IO;
            r->eof = true;
            return HP_OK;
        }

        if (!r->sniffed) {
            r->sniffed = true;
            if (n >= 2 && dst[0] == 0x1F && dst[1] == 0x8B) {
                hp_error_t err = hp_gzip_inflate_start(&r->gz);
                if (err != HP_OK) return err; /* includes GZIP_UNAVAILABLE */
                /* The sniffed bytes are compressed input; stage them so the
                 * output can be inflated straight into dst. */
                err = hp_gzip_inflate_stage(&r->gz, dst, n);
                if (err != HP_OK) return err;
                r->gz_active = true;
                return hp_gzip_inflate_read(&r->gz, r->stream, dst, cap, n_out, &r->eof);
            }
        }

        if ((n < cap && feof(r->stream)) || (r->limited && r->limit == 0)) {
            r->eof = true;
        }
        *n_out = n;
        return HP_OK;
    }

    return hp_gzip_inflate_read(&r->gz, r->stream, dst, cap, n_out, &r->eof);
}

void hp_reader_free(hp_reader_t *r) {
    if (!r) return;
    if (r->gz_active) {
        hp_gzip_inflate_free(&r->gz);
        r->gz_active = false;
    }
}

hp_error_t hp_sliding_buffer_init(hp_sliding_buffer_t *buf, size_t capacity) {
    if (!buf) return HP_ERR_INVALID_ARG;
    if (capacity < 1024) capacity = 1024;

    /* Add SIMD safety padding so AVX-512 can load past valid tail without segfault */
    size_t alloc_size = capacity + HP_SIMD_PADDING;
    buf->data = (uint8_t *)malloc(alloc_size);
    if (!buf->data) return HP_ERR_NOMEM;

    memset(buf->data, 0, alloc_size);
    buf->capacity = capacity;
    buf->head = 0;
    buf->tail = 0;
    buf->eof_reached = false;
    return HP_OK;
}

void hp_sliding_buffer_free(hp_sliding_buffer_t *buf) {
    if (!buf) return;
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->capacity = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->eof_reached = false;
}

hp_error_t hp_sliding_buffer_refill(hp_sliding_buffer_t *buf, hp_reader_t *reader, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!buf || !reader) return HP_ERR_INVALID_ARG;
    if (buf->eof_reached) return HP_OK;

    /* Slide unconsumed active data back to offset 0 */
    size_t active_len = buf->tail - buf->head;
    if (buf->head > 0) {
        if (active_len > 0) {
            memmove(buf->data, buf->data + buf->head, active_len);
        }
        buf->head = 0;
        buf->tail = active_len;
    }

    /* Free space available at the tail */
    size_t available = buf->capacity - buf->tail;
    if (available == 0) {
        /* Buffer is full and no progress was made -> record exceeds capacity */
        return HP_ERR_BUFFER_OVERFLOW;
    }

    size_t n = 0;
    hp_error_t err = hp_reader_read(reader, buf->data + buf->tail, available, &n);
    if (err != HP_OK) return err;
    buf->tail += n;
    /* Clear SIMD padding zone */
    memset(buf->data + buf->tail, 0, HP_SIMD_PADDING);

    if (bytes_read) *bytes_read = n;

    if (reader->eof) {
        buf->eof_reached = true;
    }

    return HP_OK;
}

hp_error_t hp_sliding_buffer_feed(hp_sliding_buffer_t *buf, const uint8_t *src, size_t len, size_t *fed) {
    if (fed) *fed = 0;
    if (!buf || !src) return HP_ERR_INVALID_ARG;

    /* Slide unconsumed active data back to offset 0 */
    size_t active_len = buf->tail - buf->head;
    if (buf->head > 0) {
        if (active_len > 0) {
            memmove(buf->data, buf->data + buf->head, active_len);
        }
        buf->head = 0;
        buf->tail = active_len;
    }

    size_t available = buf->capacity - buf->tail;
    size_t to_copy = (len < available) ? len : available;
    if (to_copy > 0) {
        memcpy(buf->data + buf->tail, src, to_copy);
        buf->tail += to_copy;
        memset(buf->data + buf->tail, 0, HP_SIMD_PADDING);
        if (fed) *fed = to_copy;
    }

    return HP_OK;
}
