#ifndef HYPERPIPE_HP_RING_BUFFER_H
#define HYPERPIPE_HP_RING_BUFFER_H

#include "hp_common.h"

#define HP_DEFAULT_BUFFER_CAPACITY (256 * 1024)   /* 256 KiB ultra-compact L2/L3-aligned ring buffer */
#define HP_SIMD_PADDING 64                        /* 64 bytes safety padding for AVX-512 loads */

/*
 * Sliding linear ring buffer designed for streaming SIMD parsers.
 * Guarantees that any readable span is contiguous in memory, eliminating
 * boundary split handling inside vectorized inner loops.
 *
 * Memory Layout:
 * [ ... parsed bytes (head) ... | ... unparsed active data ... | ... free space (tail) ... | SIMD padding ]
 *                               ^ head                         ^ tail                      ^ capacity
 */
typedef struct hp_sliding_buffer_s {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    bool eof_reached;
} hp_sliding_buffer_t;

hp_error_t hp_sliding_buffer_init(hp_sliding_buffer_t *buf, size_t capacity);
void hp_sliding_buffer_free(hp_sliding_buffer_t *buf);

/* Returns pointer to active readable linear window and its length */
HP_INLINE const uint8_t *hp_sliding_buffer_read_ptr(const hp_sliding_buffer_t *buf, size_t *out_len) {
    if (out_len) *out_len = buf->tail - buf->head;
    return buf->data + buf->head;
}

/* Consume n bytes from the front of the active window */
HP_INLINE void hp_sliding_buffer_consume(hp_sliding_buffer_t *buf, size_t n) {
    buf->head += n;
    if (buf->head > buf->tail) {
        buf->head = buf->tail;
    }
}

/* Slide unconsumed data to buffer origin and read next chunk from stream */
hp_error_t hp_sliding_buffer_refill(hp_sliding_buffer_t *buf, FILE *stream, size_t *bytes_read);

/* Direct memory feeder for testing and in-memory streams */
hp_error_t hp_sliding_buffer_feed(hp_sliding_buffer_t *buf, const uint8_t *src, size_t len, size_t *fed);

#endif /* HYPERPIPE_HP_RING_BUFFER_H */
