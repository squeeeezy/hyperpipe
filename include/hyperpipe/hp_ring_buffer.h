#ifndef HYPERPIPE_HP_RING_BUFFER_H
#define HYPERPIPE_HP_RING_BUFFER_H

#include "hp_common.h"
#include "hp_gzip.h"
#include <stdio.h>

#define HP_DEFAULT_BUFFER_CAPACITY (512 * 1024)   /* 512 KiB L2/L3-aligned ring buffer */
#define HP_SIMD_PADDING 64                        /* 64 bytes safety padding for AVX-512 loads */

/* Byte source for the sliding buffer: a stdio stream with transparent gzip
 * decompression. The first two bytes of the stream decide everything: a
 * 0x1F 0x8B magic switches the reader into inflate mode (libz is located at
 * runtime; see hp_gzip.h), anything else is streamed through unchanged. */
typedef struct hp_reader_s {
    FILE *stream;              /* underlying byte source */
    bool sniffed;              /* gzip magic check already performed */
    bool gz_active;            /* decompression engaged */
    bool eof;                  /* final byte of the logical stream produced */
    hp_gzip_inflate_t gz;      /* decompression state (valid when gz_active) */
} hp_reader_t;

/* Releases decompression resources (no-op for plain streams). */
void hp_reader_free(hp_reader_t *r);

/* Prepares a reader for the stream. Never allocates; decompression state is
 * created lazily on the first read if (and only if) the data is gzip. Safe
 * to call on a previously used reader: stale decompression state is
 * released first. */
HP_INLINE void hp_reader_init(hp_reader_t *r, FILE *stream) {
    hp_reader_free(r);
    r->stream = stream;
    r->sniffed = false;
    r->gz_active = false;
    r->eof = false;
}

/* Reads up to cap bytes into dst. *n_out == 0 means logical end of stream.
 * Returns HP_ERR_GZIP_UNAVAILABLE when the data is gzip but libz is absent,
 * HP_ERR_IO on stream errors, HP_ERR_GENERIC on corrupt gzip data. */
hp_error_t hp_reader_read(hp_reader_t *r, uint8_t *dst, size_t cap, size_t *n_out);

/* Releases decompression resources (no-op for plain streams). */
void hp_reader_free(hp_reader_t *r);

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

/* Slide unconsumed data to buffer origin and read the next chunk through
 * the reader (transparent gzip supported) */
hp_error_t hp_sliding_buffer_refill(hp_sliding_buffer_t *buf, hp_reader_t *reader, size_t *bytes_read);

/* Direct memory feeder for testing and in-memory streams */
hp_error_t hp_sliding_buffer_feed(hp_sliding_buffer_t *buf, const uint8_t *src, size_t len, size_t *fed);

#endif /* HYPERPIPE_HP_RING_BUFFER_H */
