#ifndef HYPERPIPE_HP_JSON_SER_H
#define HYPERPIPE_HP_JSON_SER_H

#include "hp_common.h"

typedef struct hp_json_ser_s {
    char *out_buf;
    size_t capacity;
    size_t pos;

    /* Vectorized scanner function pointer */
    size_t (*scan_escapes)(const uint8_t *p, size_t len);
} hp_json_ser_t;

hp_error_t hp_json_ser_init(hp_json_ser_t *ser, size_t initial_capacity, hp_simd_mode_t simd);
void hp_json_ser_free(hp_json_ser_t *ser);
void hp_json_ser_reset(hp_json_ser_t *ser);

/* Appends an escaped JSON string value enclosed in quotes: "..." */
hp_error_t hp_json_ser_write_string(hp_json_ser_t *ser, const char *str, size_t len);

/* Serializes a complete row with given header column names to NDJSON line */
hp_error_t hp_json_ser_row(hp_json_ser_t *ser,
                           const hp_header_t *header,
                           const hp_row_t *row,
                           const char **out_line,
                           size_t *out_line_len);

/* Reverse conversion: parses an NDJSON line into an hp_row_t and extracts header */
hp_error_t hp_json_parse_line(const char *line,
                              size_t len,
                              hp_header_t *header,
                              hp_row_t *out_row,
                              char *value_arena,
                              size_t arena_size);

/* Vectorized escape scanners */
size_t hp_json_scan_escapes_scalar(const uint8_t *p, size_t len);
size_t hp_json_scan_escapes_avx2(const uint8_t *p, size_t len);
size_t hp_json_scan_escapes_avx512(const uint8_t *p, size_t len);

#endif /* HYPERPIPE_HP_JSON_SER_H */
