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
    hp_schema_hint_t hints[HP_MAX_HINTS];
    size_t hint_count;
} hp_filter_t;

typedef struct hp_projection_s {
    char col_names[HP_MAX_COLUMNS][128];
    int64_t col_indices[HP_MAX_COLUMNS];
    size_t count;
    bool active;
} hp_projection_t;

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
bool hp_filter_matches(const hp_filter_t *filter, const hp_row_t *row);

#endif /* HYPERPIPE_HP_FILTER_H */
