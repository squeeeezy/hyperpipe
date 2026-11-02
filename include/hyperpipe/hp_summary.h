#ifndef HYPERPIPE_HP_SUMMARY_H
#define HYPERPIPE_HP_SUMMARY_H

#include "hp_common.h"
#include "hp_filter.h"
#include <stdio.h>

/* --summary: streaming per-column statistics.
 *
 * observe() is called once per filtered row from the transcoder loop. It
 * allocates nothing and never retains field pointers - rows reference ring
 * buffer memory that is overwritten as the stream advances, so every value
 * is fully consumed (converted or copied, bounded) before the call returns.
 * That keeps the memory profile O(columns), independent of stream size. */

typedef struct hp_summary_col_s {
    char name[256];        /* header name, copied once on first observation */
    bool named;
    uint64_t present;      /* fields observed in this column */
    uint64_t absent;       /* rows where this column was not present */
    uint64_t empty;        /* present but zero-length */
    uint64_t num_count;    /* values matching the JSON number grammar */
    uint64_t int_count;    /* numeric values without fraction/exponent */
    bool has_numeric;
    double min_num;
    double max_num;
    double sum;
    bool has_string;
    char min_str[48];      /* lexicographic min/max, truncated copies */
    size_t min_str_len;
    char max_str[48];
    size_t max_str_len;
} hp_summary_col_t;

typedef struct hp_summary_s {
    hp_summary_col_t cols[HP_MAX_COLUMNS];
    size_t col_count;      /* high-water mark of observed columns */
    uint64_t rows;         /* rows observed */
} hp_summary_t;

/* Zeroes the accumulator. No heap allocation anywhere in this module. */
void hp_summary_init(hp_summary_t *s);

/* Folds one filtered row into the statistics. */
void hp_summary_observe(hp_summary_t *s, const hp_header_t *header,
                        const hp_row_t *row);

/* Prints the aligned statistics table. Type inference: "int" when every
 * numeric value was an integer, "number" for mixed int/float, "mixed" when
 * both numeric and non-numeric values occur, "string" otherwise, "-" for
 * columns with no values at all. */
void hp_summary_print(const hp_summary_t *s, FILE *out);

#endif /* HYPERPIPE_HP_SUMMARY_H */
