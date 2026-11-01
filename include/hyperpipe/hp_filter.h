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
    HP_OP_GTE   /* >= */
} hp_pred_op_t;

typedef struct hp_predicate_s {
    char col_name[128];
    int64_t col_index;
    hp_pred_op_t op;
    char str_val[256];
    double num_val;
    bool is_num;
} hp_predicate_t;

#define HP_MAX_PREDICATES 16

typedef struct hp_filter_s {
    hp_predicate_t preds[HP_MAX_PREDICATES];
    size_t count;
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
hp_error_t hp_filter_bind(hp_filter_t *filter, const hp_header_t *header);
bool hp_filter_matches(const hp_filter_t *filter, const hp_row_t *row);

#endif /* HYPERPIPE_HP_FILTER_H */
