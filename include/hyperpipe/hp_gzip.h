#ifndef HYPERPIPE_HP_GZIP_H
#define HYPERPIPE_HP_GZIP_H

#include "hp_common.h"
#include <stdio.h>

/* Runtime-loaded zlib binding.
 *
 * HyperPipe ships with zero hard dependencies: zlib is discovered at runtime
 * through dlopen()/LoadLibrary() the first time gzip support is actually
 * needed, and the rest of the binary never touches it. On systems without
 * libz the tool keeps working for plain files and reports
 * HP_ERR_GZIP_UNAVAILABLE with a clear message only when .gz data is
 * requested.
 *
 * zlib's z_stream ABI has been stable since 1995, so we declare a mirror of
 * the struct locally instead of requiring zlib headers at build time. The
 * mirror must stay field-for-field identical to zlib's z_stream.
 *
 * Decompression is the primary use case (reading .gz logs/exports);
 * compression uses level 1 (Z_BEST_SPEED) to fit HyperPipe's throughput
 * profile. Concatenated multi-member gzip streams are fully supported on
 * read, matching gzip(1) semantics. */

/* Mirror of zlib's z_stream. Do not reorder or extend. */
typedef struct hp_zstream_s {
    const unsigned char *next_in;  /* next input byte */
    unsigned int        avail_in;  /* number of bytes available at next_in */
    unsigned long       total_in;  /* total bytes read so far */

    unsigned char *next_out;       /* next output byte goes here */
    unsigned int  avail_out;       /* remaining free space at next_out */
    unsigned long total_out;       /* total bytes written so far */

    const char *msg;               /* last error message, NULL when none */
    void *state;                   /* internal state, opaque */

    void *(*zalloc)(void *opaque, unsigned items, unsigned size);
    void (*zfree)(void *opaque, void *address);
    void *opaque;                  /* private data for the allocator */

    int data_type;                 /* best guess at data type */
    unsigned long adler;           /* Adler-32 value of uncompressed data */
    unsigned long reserved;        /* reserved for future use */
} hp_zstream_t;

/* zlib constants (mirrored; see zlib.h) */
#define HP_Z_NO_FLUSH   0
#define HP_Z_FINISH     4
#define HP_Z_OK         0
#define HP_Z_STREAM_END 1
#define HP_Z_DEFLATED   8

typedef int (*hp_z_inflate_init_fn)(hp_zstream_t *strm, int window_bits,
                                    const char *version, int stream_size);
typedef int (*hp_z_inflate_fn)(hp_zstream_t *strm, int flush);
typedef int (*hp_z_inflate_reset_fn)(hp_zstream_t *strm);
typedef int (*hp_z_inflate_end_fn)(hp_zstream_t *strm);
typedef int (*hp_z_deflate_init_fn)(hp_zstream_t *strm, int level, int method,
                                    int window_bits, int mem_level, int strategy,
                                    const char *version, int stream_size);
typedef int (*hp_z_deflate_fn)(hp_zstream_t *strm, int flush);
typedef int (*hp_z_deflate_end_fn)(hp_zstream_t *strm);

/* Loads libz once per process. Idempotent; never aborts on failure, it just
 * leaves hp_gzip_available() false so callers can degrade gracefully. */
void hp_gzip_load(void);

/* True when the zlib functions were resolved successfully. */
bool hp_gzip_available(void);

/* ------------------------------------------------------------------ */
/* Decompression: FILE* -> memory                                     */
/* ------------------------------------------------------------------ */

typedef struct hp_gzip_inflate_s {
    hp_zstream_t z;
    unsigned char *staging;      /* raw compressed bytes are read here */
    size_t staging_cap;
    unsigned char member_magic[2]; /* fed back to inflate after member switch */
    bool src_eof;                /* underlying FILE* is drained */
    bool done;                   /* all members decoded */
    bool failed;
} hp_gzip_inflate_t;

/* Prepares a decompressor. Allocates an internal staging buffer. */
hp_error_t hp_gzip_inflate_start(hp_gzip_inflate_t *gz);

/* Seeds the decompressor with input bytes that were already read from the
 * source (the sniffed gzip header block). The bytes are copied into the
 * staging buffer; `dst` may then safely receive decompressed output. */
hp_error_t hp_gzip_inflate_stage(hp_gzip_inflate_t *gz,
                                 const unsigned char *data, size_t len);

/* Inflates from `src` into `dst`, filling dst as far as possible. Returns
 * HP_OK with *n_out set (possibly 0 only at end of stream). Sets *src_eof
 * when the underlying stream is fully consumed. Concatenated gzip members
 * are transparently continued. */
hp_error_t hp_gzip_inflate_read(hp_gzip_inflate_t *gz, FILE *src,
                                unsigned char *dst, size_t cap,
                                size_t *n_out, bool *src_eof);

void hp_gzip_inflate_free(hp_gzip_inflate_t *gz);

/* ------------------------------------------------------------------ */
/* Compression: memory -> chunks (caller writes them out)             */
/* ------------------------------------------------------------------ */

/* Default compression level: fastest, fits the throughput profile. */
#define HP_GZIP_DEFAULT_LEVEL 1

typedef struct hp_gzip_deflate_s {
    hp_zstream_t z;
    unsigned char *scratch;      /* compressed output staging */
    size_t scratch_cap;
    bool started;
    bool finished;
} hp_gzip_deflate_t;

hp_error_t hp_gzip_deflate_start(hp_gzip_deflate_t *dz);

/* Compresses src into internal scratch memory; *out stays valid until the
 * next push/finish call on the same deflate stream. len may be 0. */
hp_error_t hp_gzip_deflate_push(hp_gzip_deflate_t *dz,
                                const unsigned char *src, size_t len,
                                const unsigned char **out, size_t *out_len);

/* Flushes the trailing gzip footer; *out holds the final bytes. */
hp_error_t hp_gzip_deflate_finish(hp_gzip_deflate_t *dz,
                                  const unsigned char **out, size_t *out_len);

void hp_gzip_deflate_free(hp_gzip_deflate_t *dz);

#endif /* HYPERPIPE_HP_GZIP_H */
