#ifndef HYPERPIPE_HP_FILTER_H
#define HYPERPIPE_HP_FILTER_H

#include "hp_common.h"

typedef enum hp_pred_op_e {
    HP_OP_NONE = 0,
    HP_OP_EQ,   /* == */
    HP_OP_NEQ,  /* != */
    HP_OP_LT,   /* <  */
    HP_OP_LTE,  /* <= */
    HP_OP_GT,   /* >  */
    HP_OP_GTE,  /* >= */
    HP_OP_CONTAINS,    /* string: field contains value  */
    HP_OP_STARTSWITH,  /* string: field starts with value */
    HP_OP_ENDSWITH     /* string: field ends with value */
} hp_pred_op_t;

typedef struct hp_predicate_s {
    char col_name[128];
    int64_t col_index;
    hp_pred_op_t op;
    char str_val[256];
    size_t str_val_len;
    double num_val;
    int64_t int_val;
    bool is_num;
    bool is_int;
    /* str_val is a positive integer written without sign or leading zeros
     * ("500"); enables the inline equality test in hp_filter_matches() */
    bool is_positive_canonical_int;
    /* set from --schema hints at bind time: the column is declared to hold
     * integers, so numeric predicates use the fast int64 span parser and
     * compare in double space (see hp_pred_matches) */
    bool hint_int;
    uint16_t or_group;   /* predicates sharing a group are ANDed; groups are ORed */
} hp_predicate_t;

#define HP_MAX_PREDICATES 16
#define HP_MAX_HINTS 256

/* One line of a --schema file: "name,type" where type is int or string.
 * Columns declared int get the fast numeric predicate path. */
typedef struct hp_schema_hint_s {
    char name[128];
    bool is_int;
} hp_schema_hint_t;

typedef struct hp_filter_s {
    hp_predicate_t preds[HP_MAX_PREDICATES];
    size_t count;
    /* Why hp_filter_init() returned HP_ERR_FILTER, and the byte offset in
     * the filter text where parsing stopped */
    const char *error;
    size_t error_at;
    hp_schema_hint_t hints[HP_MAX_HINTS];
    size_t hint_count;
} hp_filter_t;

typedef struct hp_projection_s {
    char col_names[HP_MAX_COLUMNS][128];
    int64_t col_indices[HP_MAX_COLUMNS];
    size_t count;
    bool active;
} hp_projection_t;

/* A column reference: a header name first, otherwise a 0-based position.
 * -1 when it is neither. */
int64_t hp_resolve_column(const hp_header_t *header, const char *ref);

hp_error_t hp_projection_init(hp_projection_t *proj, const char *spec);
hp_error_t hp_projection_bind(hp_projection_t *proj, const hp_header_t *header);
hp_error_t hp_project_row(const hp_projection_t *proj, const hp_row_t *src, hp_row_t *dst);
hp_error_t hp_project_header(const hp_projection_t *proj, const hp_header_t *src, hp_header_t *dst);

hp_error_t hp_filter_init(hp_filter_t *filter, const char *where_clause);

/* Loads declared column types from a schema file (lines of "name,type",
 * '#' starts a comment). Applies at hp_filter_bind time. Unknown types are
 * rejected with HP_ERR_INVALID_ARG; a missing file yields HP_ERR_IO. */
hp_error_t hp_filter_load_hints(hp_filter_t *filter, const char *path);

hp_error_t hp_filter_bind(hp_filter_t *filter, const hp_header_t *header);

/* General evaluator; hp_filter_matches() below is the entry point. */
bool hp_filter_eval(const hp_filter_t *filter, const hp_row_t *row);

HP_INLINE bool hp_is_digit(char c) { return (uint8_t)(c - '0') <= 9; }

/* Runs once per row, so the most common filter shape - a single equality
 * test - is decided inline without a call or a number parse.
 *
 * For a positive integer literal such as 500 the bytes can differ while the
 * values match ("0500", "5e2", "500.0"), so a mismatch alone proves nothing.
 * It does when the field starts with 1-9 and the first differing bytes are
 * both digits: the two numbers then differ in a significant digit, and no
 * fraction or exponent spelling can change that. For "status == 500" over
 * "200" or "304" that is a single byte compare. */
HP_INLINE bool hp_filter_matches(const hp_filter_t *filter, const hp_row_t *row) {
    if (filter->count == 0) return true;

    const hp_predicate_t *p = &filter->preds[0];
    if (filter->count == 1 && p->op == HP_OP_EQ && (!p->is_num || p->is_positive_canonical_int)) {
        if (p->col_index < 0 || (size_t)p->col_index >= row->field_count) return false;
        const hp_str_t *f = &row->fields[p->col_index];
        const size_t n = f->len < p->str_val_len ? f->len : p->str_val_len;
        size_t i = 0;
        while (i < n && f->ptr[i] == p->str_val[i]) i++;

        if (i == n && f->len == p->str_val_len) return true;
        if (!p->is_num) return false;
        if (i < n && hp_is_digit(f->ptr[i]) && hp_is_digit(p->str_val[i]) &&
            f->ptr[0] >= '1' && f->ptr[0] <= '9') {
            return false;
        }
    }
    return hp_filter_eval(filter, row);
}

#endif /* HYPERPIPE_HP_FILTER_H */
