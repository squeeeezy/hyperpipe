#ifndef HYPERPIPE_HP_COMMON_H
#define HYPERPIPE_HP_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_MSC_VER)
    #define HP_ALIGNED(n) __declspec(align(n))
    #define HP_INLINE static __forceinline
    #define HP_HOT
#elif defined(__GNUC__) || defined(__clang__)
    #define HP_ALIGNED(n) __attribute__((aligned(n)))
    #define HP_INLINE static inline __attribute__((always_inline))
    #define HP_HOT __attribute__((hot))
#else
    #define HP_ALIGNED(n)
    #define HP_INLINE static inline
    #define HP_HOT
#endif

typedef enum hp_error_e {
    HP_OK = 0,
    HP_ERR_GENERIC = -1,
    HP_ERR_NOMEM = -2,
    HP_ERR_IO = -3,
    HP_ERR_PARSE_QUOTE = -4,
    HP_ERR_PARSE_DELIM = -5,
    HP_ERR_BUFFER_OVERFLOW = -6,
    HP_ERR_INVALID_ARG = -7,
    HP_ERR_UNSUPPORTED_SIMD = -8,
    HP_ERR_COLUMN_NOT_FOUND = -9,
    HP_ERR_GZIP_UNAVAILABLE = -10,
    HP_ERR_UNSUPPORTED = -11,
    HP_ERR_PARSE_JSON = -12
} hp_error_t;

typedef enum hp_format_e {
    HP_FORMAT_AUTO = 0,
    HP_FORMAT_CSV = 1,
    HP_FORMAT_TSV = 2,
    HP_FORMAT_NDJSON = 3
} hp_format_t;

typedef enum hp_on_error_e {
    HP_ON_ERROR_FAIL = 0,
    HP_ON_ERROR_SKIP = 1,
    HP_ON_ERROR_QUARANTINE = 2
} hp_on_error_t;

typedef enum hp_simd_mode_e {
    HP_SIMD_AUTO = 0,
    HP_SIMD_SCALAR = 1,
    HP_SIMD_AVX2 = 2,
    HP_SIMD_AVX512 = 3
} hp_simd_mode_t;

typedef struct hp_str_s {
    const char *ptr;
    size_t len;
    /* true when the value originated in quoted/string form ("x,y", "123",
     * JSON strings). Such values must travel through the re-serializing write
     * path (write_csv_field_pb / hp_json_ser_write_string) so embedded
     * delimiters, newlines and quotes are re-escaped on output. Clean
     * zero-copy fields keep this flag false and are emitted with raw memcpy. */
    bool has_escapes;
    /* true for a JSON object or array value from NDJSON input: JSON text
     * that NDJSON output writes verbatim and CSV output writes as text */
    bool is_json;
    /* false when the field is absent from this record (sparse NDJSON rows).
     * Absent fields are omitted from NDJSON objects and serialized as empty
     * strings in CSV/TSV output. */
    bool present;
} hp_str_t;

#define HP_MAX_COLUMNS 1024

typedef struct hp_row_s {
    hp_str_t fields[HP_MAX_COLUMNS];
    size_t field_count;
    size_t row_index;
} hp_row_t;

typedef struct hp_header_s {
    char *names[HP_MAX_COLUMNS];
    size_t count;
} hp_header_t;

HP_INLINE void hp_header_free(hp_header_t *h) {
    if (!h) return;
    for (size_t i = 0; i < h->count; i++) {
        free(h->names[i]);
        h->names[i] = NULL;
    }
    h->count = 0;
}

HP_INLINE int64_t hp_header_find_column(const hp_header_t *h, const char *name) {
    if (!h || !name) return -1;
    for (size_t i = 0; i < h->count; i++) {
        if (strcmp(h->names[i], name) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

#endif /* HYPERPIPE_HP_COMMON_H */
