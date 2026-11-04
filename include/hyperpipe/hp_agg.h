#ifndef HYPERPIPE_HP_AGG_H
#define HYPERPIPE_HP_AGG_H

#include "hp_common.h"

/* --group-by and --agg: hash aggregation.
 *
 * Every row that passes the filter is folded into the group of its key, the
 * bytes of the --group-by columns; without --group-by there is one group.
 * Groups come out in the order their first row arrived.
 *
 * Sums, minimums and maximums are exact decimals while the values have at
 * most 18 significant digits and the sum fits in 128 bits; past that they
 * fall back to double. Exact results do not depend on the order rows are
 * folded in, so parallel workers aggregate their chunks and hp_agg_merge()
 * combines them, in chunk order, into the single-threaded result.
 *
 * Memory grows with the number of groups, not with the input. */

#define HP_AGG_MAX_KEYS 16
#define HP_AGG_MAX_FNS 32

typedef enum hp_agg_fn_e {
    HP_AGG_COUNT,          /* rows in the group */
    HP_AGG_SUM,            /* the next four skip values that are not numbers */
    HP_AGG_AVG,
    HP_AGG_MIN,
    HP_AGG_MAX
} hp_agg_fn_t;

typedef struct hp_agg_spec_s {
    hp_agg_fn_t fn;
    char column[128];      /* empty for count */
    int64_t col_index;
    char name[160];        /* the output column: "count", "sum(bytes)" */
} hp_agg_spec_t;

typedef struct hp_agg_group_s hp_agg_group_t;
typedef struct hp_agg_acc_s hp_agg_acc_t;

typedef struct hp_agg_s {
    char keys[HP_AGG_MAX_KEYS][128];
    int64_t key_index[HP_AGG_MAX_KEYS];
    size_t key_count;
    hp_agg_spec_t fns[HP_AGG_MAX_FNS];
    size_t fn_count;
    /* Why hp_agg_init() failed, and the offset in the --agg text */
    const char *error;
    size_t error_at;

    /* Open addressing over group ids; a slot holds id + 1, 0 when free */
    uint32_t *slots;
    size_t slot_mask;
    hp_agg_group_t *groups;
    size_t group_count;
    size_t group_cap;
    hp_agg_acc_t *acc;     /* fn_count accumulators per group */
    char *key_bytes;       /* every group's encoded key */
    size_t key_len;
    size_t key_cap;
} hp_agg_t;

/* Bytes of output one aggregate value may take in hp_agg_row() */
#define HP_AGG_VALUE_MAX 48

/* Parses the comma-separated --group-by columns and --agg functions, either
 * of which may be NULL; --group-by alone counts the rows of each group.
 * HP_ERR_INVALID_ARG for a bad --group-by, HP_ERR_AGG with error/error_at
 * for a bad --agg. */
hp_error_t hp_agg_init(hp_agg_t *agg, const char *group_by, const char *fns);
void hp_agg_free(hp_agg_t *agg);

/* Resolves column names (or 0-based positions) against the header. On
 * HP_ERR_COLUMN_NOT_FOUND, *missing names the column. */
hp_error_t hp_agg_bind(hp_agg_t *agg, const hp_header_t *header, const char **missing);

/* The output columns: the group-by columns, then one per function */
hp_error_t hp_agg_header(const hp_agg_t *agg, hp_header_t *out);

hp_error_t hp_agg_observe(hp_agg_t *agg, const hp_row_t *row);

/* Folds src into dst; src's new groups go after dst's */
hp_error_t hp_agg_merge(hp_agg_t *dst, const hp_agg_t *src);

/* Rows hp_agg_row() produces: one per group, and one for an empty input
 * without --group-by, as SQL does */
size_t hp_agg_rows(const hp_agg_t *agg);

/* Fills out with row i. Key fields point into the table, aggregate values
 * into scratch, which needs HP_AGG_VALUE_MAX bytes per function. */
void hp_agg_row(const hp_agg_t *agg, size_t i, hp_row_t *out, char *scratch);

#endif /* HYPERPIPE_HP_AGG_H */
