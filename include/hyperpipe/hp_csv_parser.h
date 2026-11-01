#ifndef HYPERPIPE_HP_CSV_PARSER_H
#define HYPERPIPE_HP_CSV_PARSER_H

#include "hp_common.h"

#define HP_MAX_ROW_BYTES (64 * 1024)

typedef struct hp_csv_parser_s {
    uint8_t delim;
    uint8_t quote;

    /* Parser state machine */
    bool in_quotes;
    bool quote_just_closed;

    /* Current row reconstruction arena for unescaping '""' -> '"' */
    char unescape_arena[HP_MAX_ROW_BYTES];
    size_t arena_pos;

    /* Function pointers for vectorized acceleration */
    size_t (*scan_special)(const uint8_t *p, size_t len, uint8_t delim);
    size_t (*scan_quote)(const uint8_t *p, size_t len);
} hp_csv_parser_t;

hp_error_t hp_csv_parser_init(hp_csv_parser_t *parser, hp_format_t fmt, hp_simd_mode_t simd);
void hp_csv_parser_reset(hp_csv_parser_t *parser);

hp_error_t hp_csv_parse_row(hp_csv_parser_t *parser,
                            const uint8_t *src,
                            size_t len,
                            size_t *consumed,
                            hp_row_t *out_row,
                            bool is_eof);

hp_error_t hp_csv_parse_row_scalar(hp_csv_parser_t *parser,
                                   const uint8_t *src,
                                   size_t len,
                                   size_t *consumed,
                                   hp_row_t *out_row,
                                   bool is_eof);

hp_error_t hp_csv_parse_row_avx512(hp_csv_parser_t *parser,
                                   const uint8_t *src,
                                   size_t len,
                                   size_t *consumed,
                                   hp_row_t *out_row,
                                   bool is_eof);

/* Scalar scanner functions */
size_t hp_csv_scan_special_scalar(const uint8_t *p, size_t len, uint8_t delim);
size_t hp_csv_scan_quote_scalar(const uint8_t *p, size_t len);

/* AVX2 scanner functions */
size_t hp_csv_scan_special_avx2(const uint8_t *p, size_t len, uint8_t delim);
size_t hp_csv_scan_quote_avx2(const uint8_t *p, size_t len);

/* AVX-512 scanner functions */
size_t hp_csv_scan_special_avx512(const uint8_t *p, size_t len, uint8_t delim);
size_t hp_csv_scan_quote_avx512(const uint8_t *p, size_t len);

#endif /* HYPERPIPE_HP_CSV_PARSER_H */
