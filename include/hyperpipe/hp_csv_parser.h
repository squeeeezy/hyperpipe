#ifndef HYPERPIPE_HP_CSV_PARSER_H
#define HYPERPIPE_HP_CSV_PARSER_H

#include "hp_common.h"

typedef struct hp_csv_parser_s {
    uint8_t delim;
    uint8_t quote;

    /* Parser state machine */
    bool in_quotes;
    bool quote_just_closed;

    /* Unescaped copies ('""' -> '"') of the current record's fields. An
     * unescaped field is never longer than its source, so the arena is sized
     * to the parse window on first use and never moves while a record is
     * being built. Records without escapes never allocate it. */
    char *unescape_arena;
    size_t arena_cap;
    size_t arena_pos;

    /* Function pointers for vectorized acceleration */
    size_t (*scan_special)(const uint8_t *p, size_t len, uint8_t delim);
    size_t (*scan_quote)(const uint8_t *p, size_t len);
} hp_csv_parser_t;

hp_error_t hp_csv_parser_init(hp_csv_parser_t *parser, hp_format_t fmt, hp_simd_mode_t simd);
void hp_csv_parser_reset(hp_csv_parser_t *parser);
void hp_csv_parser_free(hp_csv_parser_t *parser);

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

hp_error_t hp_csv_parse_row_avx2(hp_csv_parser_t *parser,
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

/* ---- Chunk planner bitmaps (--jobs boundary scan) -------------------------
 *
 * For one chunk of up to 64 bytes, bit i of *nl marks p[i] == '\n' and
 * bit i of *qm marks p[i] == '"'. Only chunk_len bytes are read; callers
 * pass byte ranges that end at a buffer boundary or rely on the masked
 * loads of the SIMD twins. All three implementations return identical
 * bitmaps (differential-tested), so the parallel planner consumes them
 * interchangeably through the dispatch table. */
void hp_csv_plan_masks_scalar(const uint8_t *p, size_t chunk_len,
                              uint64_t *nl, uint64_t *qm);
void hp_csv_plan_masks_avx2(const uint8_t *p, size_t chunk_len,
                            uint64_t *nl, uint64_t *qm);
void hp_csv_plan_masks_avx512(const uint8_t *p, size_t chunk_len,
                              uint64_t *nl, uint64_t *qm);

#endif /* HYPERPIPE_HP_CSV_PARSER_H */
