#include "hyperpipe/hp_ring_buffer.h"

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

hp_error_t hp_sliding_buffer_refill(hp_sliding_buffer_t *buf, FILE *stream, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!buf || !stream) return HP_ERR_INVALID_ARG;
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

    size_t n = fread(buf->data + buf->tail, 1, available, stream);
    buf->tail += n;
    /* Clear SIMD padding zone */
    memset(buf->data + buf->tail, 0, HP_SIMD_PADDING);

    if (bytes_read) *bytes_read = n;

    if (n < available) {
        if (feof(stream)) {
            buf->eof_reached = true;
        } else if (ferror(stream)) {
            return HP_ERR_IO;
        }
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
