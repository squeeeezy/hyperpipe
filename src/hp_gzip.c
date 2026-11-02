#include "hyperpipe/hp_gzip.h"

/* zlib binding state ---------------------------------------------------- */
#if defined(_WIN32)
    #include <windows.h>
    static HMODULE g_zlib_handle = NULL;
    /* The uintptr_t round-trip is the ISO C-clean way to convert an object
     * pointer (dlsym/GetProcAddress result) to a function pointer. */
    #define HP_DLOPEN(name)      LoadLibraryA(name)
    #define HP_DLSYM(handle, ty, name) (ty)(uintptr_t)GetProcAddress((handle), name)
#else
    #include <dlfcn.h>
    static void *g_zlib_handle = NULL;
    #define HP_DLOPEN(name)      dlopen(name, RTLD_NOW | RTLD_LOCAL)
    #define HP_DLSYM(handle, ty, name) (ty)(uintptr_t)dlsym((handle), name)
#endif

static hp_z_inflate_init_fn  g_inflate_init2_;
static hp_z_inflate_fn       g_inflate;
static hp_z_inflate_reset_fn g_inflate_reset;
static hp_z_inflate_end_fn   g_inflate_end;
static hp_z_deflate_init_fn  g_deflate_init2_;
static hp_z_deflate_fn       g_deflate;
static hp_z_deflate_end_fn   g_deflate_end;
static bool g_zlib_probe_done = false;
static bool g_zlib_ok = false;

void hp_gzip_load(void) {
    if (g_zlib_probe_done) return;
    g_zlib_probe_done = true;

#if defined(_WIN32)
    g_zlib_handle = HP_DLOPEN("zlib1.dll");
#else
    /* Prefer the versioned soname, fall back to the bare linker name. */
    g_zlib_handle = HP_DLOPEN("libz.so.1");
    if (!g_zlib_handle) {
        g_zlib_handle = HP_DLOPEN("libz.so");
    }
#endif
    if (!g_zlib_handle) return;

    g_inflate_init2_  = HP_DLSYM(g_zlib_handle, hp_z_inflate_init_fn,  "inflateInit2_");
    g_inflate         = HP_DLSYM(g_zlib_handle, hp_z_inflate_fn,       "inflate");
    g_inflate_reset   = HP_DLSYM(g_zlib_handle, hp_z_inflate_reset_fn, "inflateReset");
    g_inflate_end     = HP_DLSYM(g_zlib_handle, hp_z_inflate_end_fn,   "inflateEnd");
    g_deflate_init2_  = HP_DLSYM(g_zlib_handle, hp_z_deflate_init_fn,  "deflateInit2_");
    g_deflate         = HP_DLSYM(g_zlib_handle, hp_z_deflate_fn,       "deflate");
    g_deflate_end     = HP_DLSYM(g_zlib_handle, hp_z_deflate_end_fn,   "deflateEnd");

    g_zlib_ok = g_inflate_init2_ && g_inflate && g_inflate_reset && g_inflate_end &&
                g_deflate_init2_ && g_deflate && g_deflate_end;
}

bool hp_gzip_available(void) {
    hp_gzip_load();
    return g_zlib_ok;
}

/* zlib only compares the major version digit and the struct size, so a
 * minimal version string is sufficient for the mirror layout above. */
#define HP_ZLIB_VERSION "1"
#define HP_Z_STREAM_SIZE ((int)sizeof(hp_zstream_t))

#define HP_INFLATE_WINDOW_AUTO 47  /* 15 + 32: detect gzip or zlib header */
#define HP_DEFLATE_WINDOW_GZ   31  /* 15 + 16: produce a gzip-wrapped stream */
#define HP_DEFLATE_MEM_LEVEL   8
#define HP_Z_DEFAULT_STRATEGY  0

/* ------------------------------------------------------------------ */
/* Decompression                                                      */
/* ------------------------------------------------------------------ */

#define HP_GZIP_STAGING_CAP (256 * 1024)

hp_error_t hp_gzip_inflate_start(hp_gzip_inflate_t *gz) {
    if (!gz) return HP_ERR_INVALID_ARG;
    if (!hp_gzip_available()) return HP_ERR_GZIP_UNAVAILABLE;
    memset(gz, 0, sizeof(*gz));

    gz->staging_cap = HP_GZIP_STAGING_CAP;
    gz->staging = (unsigned char *)malloc(gz->staging_cap);
    if (!gz->staging) return HP_ERR_NOMEM;

    gz->z.zalloc = NULL;
    gz->z.zfree = NULL;
    gz->z.opaque = NULL;
    int rc = g_inflate_init2_(&gz->z, HP_INFLATE_WINDOW_AUTO,
                              HP_ZLIB_VERSION, HP_Z_STREAM_SIZE);
    if (rc != HP_Z_OK) {
        free(gz->staging);
        gz->staging = NULL;
        return HP_ERR_GENERIC;
    }
    return HP_OK;
}

hp_error_t hp_gzip_inflate_stage(hp_gzip_inflate_t *gz,
                                 const unsigned char *data, size_t len) {
    if (!gz || !gz->staging) return HP_ERR_INVALID_ARG;
    /* The first staged chunk is as large as the caller's read buffer; grow
     * the staging area on demand so any first-read size fits. */
    if (len > gz->staging_cap) {
        unsigned char *nb = (unsigned char *)realloc(gz->staging, len);
        if (!nb) return HP_ERR_NOMEM;
        gz->staging = nb;
        gz->staging_cap = len;
    }
    memcpy(gz->staging, data, len);
    gz->z.next_in = gz->staging;
    gz->z.avail_in = (unsigned int)len;
    return HP_OK;
}

hp_error_t hp_gzip_inflate_read(hp_gzip_inflate_t *gz, FILE *src,
                                unsigned char *dst, size_t cap,
                                size_t *n_out, bool *src_eof) {
    if (!gz || !src || !dst || !n_out || !src_eof) return HP_ERR_INVALID_ARG;
    *n_out = 0;
    if (gz->failed) return HP_ERR_GENERIC;

    gz->z.next_out = dst;
    gz->z.avail_out = (unsigned int)cap;

    while (gz->z.avail_out > 0 && !gz->done) {
        if (gz->z.avail_in == 0 && !gz->src_eof) {
            size_t n = fread(gz->staging, 1, gz->staging_cap, src);
            if (n == 0) {
                if (ferror(src)) {
                    gz->failed = true;
                    return HP_ERR_IO;
                }
                gz->src_eof = true;
            } else {
                gz->z.next_in = gz->staging;
                gz->z.avail_in = (unsigned int)n;
            }
        }

        if (gz->z.avail_in == 0 && gz->src_eof) {
            /* Truncated stream: deliver whatever was produced so far; the
             * next call reports EOF through done + n_out == 0. */
            gz->done = true;
            break;
        }

        int before_out = (int)gz->z.avail_out;
        int rc = g_inflate(&gz->z, HP_Z_NO_FLUSH);
        size_t produced = (size_t)(before_out - (int)gz->z.avail_out);
        *n_out += produced;

        if (rc == HP_Z_STREAM_END) {
            /* Multi-member stream: gzip(1) concatenation continues with a
             * fresh member header; anything else after a member is trailing
             * data, which gzip semantics ignore. Peek the next two bytes
             * from leftover input first, then from the stream. On a member
             * switch, leftover input stays queued untouched; stream-peeked
             * magic is fed back as synthetic input because inflate must see
             * a complete header after inflateReset. */
            unsigned char magic[2];
            bool consumed_input = false;
            size_t magic_n = 0;
            while (magic_n < 2) {
                if (gz->z.avail_in >= 2 && magic_n == 0) {
                    /* Both bytes sit in leftover input: peek without
                     * consuming, so a member switch can resume exactly at
                     * the header inflate is about to expect. */
                    magic[0] = gz->z.next_in[0];
                    magic[1] = gz->z.next_in[1];
                    magic_n = 2;
                    break;
                }
                if (gz->z.avail_in > 0) {
                    magic[magic_n++] = gz->z.next_in[0];
                    gz->z.next_in++;
                    gz->z.avail_in--;
                    consumed_input = true;
                } else {
                    int c = fgetc(src);
                    if (c == EOF) {
                        if (ferror(src)) {
                            gz->failed = true;
                            return HP_ERR_IO;
                        }
                        break;
                    }
                    magic[magic_n++] = (unsigned char)c;
                    consumed_input = true;
                }
            }

            bool next_member = (magic_n == 2 && magic[0] == 0x1F && magic[1] == 0x8B);
            if (next_member && g_inflate_reset(&gz->z) == HP_Z_OK) {
                if (consumed_input) {
                    /* Peeked bytes came from the stream or stripped the
                     * leftover tail: restore them as synthetic input so the
                     * new member sees a complete gzip header. */
                    gz->member_magic[0] = magic[0];
                    gz->member_magic[1] = magic[1];
                    gz->z.next_in = gz->member_magic;
                    gz->z.avail_in = 2;
                }
                /* else: the full header is still queued; nothing to feed */
                continue;
            }
            gz->done = true;
            *src_eof = true;
            break;
        }
        if (rc != HP_Z_OK) {
            gz->failed = true;
            return HP_ERR_GENERIC; /* corrupt gzip data */
        }
        if (produced == 0 && gz->z.avail_in == 0 && gz->src_eof) {
            gz->done = true;
            break;
        }
    }

    if (gz->done && gz->z.avail_out > 0) {
        *src_eof = true;
    }
    return HP_OK;
}

void hp_gzip_inflate_free(hp_gzip_inflate_t *gz) {
    if (!gz) return;
    if (gz->staging) {
        if (g_zlib_ok && gz->z.state) {
            g_inflate_end(&gz->z);
        }
        free(gz->staging);
        gz->staging = NULL;
    }
    gz->done = true;
}

/* ------------------------------------------------------------------ */
/* Compression                                                        */
/* ------------------------------------------------------------------ */

hp_error_t hp_gzip_deflate_start(hp_gzip_deflate_t *dz) {
    if (!dz) return HP_ERR_INVALID_ARG;
    if (!hp_gzip_available()) return HP_ERR_GZIP_UNAVAILABLE;
    memset(dz, 0, sizeof(*dz));

    /* deflateBound() is not resolved; a generous scratch avoids any growth
     * logic: one page in, worst-case slightly-larger output (stored blocks
     * add at most 5 bytes per 16 KB plus the gzip header/footer). */
    dz->scratch_cap = HP_GZIP_STAGING_CAP + HP_GZIP_STAGING_CAP / 4096 * 5 + 32768;
    dz->scratch = (unsigned char *)malloc(dz->scratch_cap);
    if (!dz->scratch) return HP_ERR_NOMEM;

    dz->z.zalloc = NULL;
    dz->z.zfree = NULL;
    dz->z.opaque = NULL;
    int rc = g_deflate_init2_(&dz->z, HP_GZIP_DEFAULT_LEVEL, HP_Z_DEFLATED,
                              HP_DEFLATE_WINDOW_GZ, HP_DEFLATE_MEM_LEVEL,
                              HP_Z_DEFAULT_STRATEGY,
                              HP_ZLIB_VERSION, HP_Z_STREAM_SIZE);
    if (rc != HP_Z_OK) {
        free(dz->scratch);
        dz->scratch = NULL;
        return HP_ERR_GENERIC;
    }
    dz->started = true;
    return HP_OK;
}

hp_error_t hp_gzip_deflate_push(hp_gzip_deflate_t *dz,
                                const unsigned char *src, size_t len,
                                const unsigned char **out, size_t *out_len) {
    if (!dz || !dz->started || (!src && len > 0) || !out || !out_len) {
        return HP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;

    dz->z.next_in = src;
    dz->z.avail_in = (unsigned int)len;
    dz->z.next_out = dz->scratch;
    dz->z.avail_out = (unsigned int)dz->scratch_cap;

    int rc = g_deflate(&dz->z, HP_Z_NO_FLUSH);
    if (rc != HP_Z_OK) return HP_ERR_GENERIC;

    *out = dz->scratch;
    *out_len = dz->scratch_cap - dz->z.avail_out;
    return HP_OK;
}

hp_error_t hp_gzip_deflate_finish(hp_gzip_deflate_t *dz,
                                  const unsigned char **out, size_t *out_len) {
    if (!dz || !dz->started || !out || !out_len) return HP_ERR_INVALID_ARG;
    if (dz->finished) {
        *out = NULL;
        *out_len = 0;
        return HP_OK;
    }

    dz->z.next_in = NULL;
    dz->z.avail_in = 0;
    dz->z.next_out = dz->scratch;
    dz->z.avail_out = (unsigned int)dz->scratch_cap;

    int rc = g_deflate(&dz->z, HP_Z_FINISH);
    if (rc != HP_Z_STREAM_END) return HP_ERR_GENERIC;

    *out = dz->scratch;
    *out_len = dz->scratch_cap - dz->z.avail_out;
    dz->finished = true;
    return HP_OK;
}

void hp_gzip_deflate_free(hp_gzip_deflate_t *dz) {
    if (!dz) return;
    if (dz->scratch) {
        if (g_zlib_ok && dz->z.state) {
            g_deflate_end(&dz->z);
        }
        free(dz->scratch);
        dz->scratch = NULL;
    }
    dz->started = false;
    dz->finished = true;
}
