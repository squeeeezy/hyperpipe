#ifndef HYPERPIPE_HP_COMPUTE_H
#define HYPERPIPE_HP_COMPUTE_H

#include "hp_common.h"
#include "hp_json_ser.h"

/* --add and paths into JSON: columns computed from each row.
 *
 * "kb = bytes / 1024, who = lower(user.name)" adds the columns kb and who
 * after the input's. Each may use the columns before it, and one named like
 * an input column replaces it. A reference that names no column but starts
 * with one and goes on with .key or [index], such as meta.user.id or
 * tags[-1], reads into that column's JSON value. Named in a filter,
 * --select, --group-by, --agg or --sort, such a path becomes a hidden
 * column, written out only where it is selected.
 *
 * The values are computed into the row once it is parsed, so everything
 * downstream sees them as input columns. Arithmetic is exact on decimals of
 * up to 18 digits, as sums are (hp_agg.h); division, and anything wider, is
 * in double. Where a number is needed and the value is not one, the result
 * is empty, as it is for an empty or missing field. */

#define HP_COMPUTE_MAX_COLUMNS 64
#define HP_COMPUTE_MAX_NODES 512
#define HP_COMPUTE_MAX_STEPS 256
#define HP_COMPUTE_MAX_ARGS 8
#define HP_COMPUTE_TEXT 16384

/* The most bytes a value takes: so many per byte of the record, plus some */
typedef struct hp_bound_s {
    size_t per_byte;
    size_t fixed;
} hp_bound_t;

/* One node of an expression */
typedef struct hp_expr_s {
    uint8_t kind;          /* the kinds and functions are in hp_compute.c */
    uint8_t fn;
    uint8_t unit;          /* the hp_time_unit_t of hour() and the like */
    uint8_t argc;
    uint16_t arg[HP_COMPUTE_MAX_ARGS];
    uint32_t text;         /* a name or literal, in hp_compute_t.text */
    uint32_t text_len;
    uint32_t root_len;     /* a `quoted` name: the bytes that name the column */
    uint32_t at;           /* where it starts in the --add text */
    uint16_t step;         /* a path's steps */
    uint16_t step_count;
    int64_t col;           /* the field a reference reads, once bound */
    bool exact;            /* a number literal: mant * 10^-scale, or f */
    int scale;
    int64_t mant;
    double f;
} hp_expr_t;

typedef struct hp_computed_s {
    char name[128];
    uint16_t root;
    int64_t index;         /* the field it fills */
    bool hidden;           /* a path some option named, not an --add column */
    hp_bound_t bound;      /* of its value */
} hp_computed_t;

typedef struct hp_compute_s {
    hp_computed_t cols[HP_COMPUTE_MAX_COLUMNS];
    size_t count;
    size_t hidden;         /* how many of them are hidden */
    size_t before_filter;  /* the filter needs columns [0, before_filter) */
    size_t width;          /* the input's columns */
    bool appends;          /* a column goes after the input's */

    hp_expr_t nodes[HP_COMPUTE_MAX_NODES];
    size_t node_count;
    hp_json_step_t steps[HP_COMPUTE_MAX_STEPS];
    size_t step_count;
    char text[HP_COMPUTE_TEXT];
    size_t text_len;

    /* Text the current row's values are written to */
    char *arena;
    size_t arena_cap;
    size_t arena_len;
    hp_bound_t arena_need;

    /* Why hp_compute_init() failed, and the offset in the --add text */
    const char *error;
    size_t error_at;
} hp_compute_t;

/* Parses the --add text, which may be NULL. HP_ERR_COMPUTE, with error and
 * error_at, when it is invalid. */
hp_error_t hp_compute_init(hp_compute_t *c, const char *spec);
void hp_compute_free(hp_compute_t *c);

/* Resolves the --add columns' references and appends their new columns to
 * the header. On HP_ERR_COLUMN_NOT_FOUND, *missing names the reference. */
hp_error_t hp_compute_bind(hp_compute_t *c, hp_header_t *header, const char **missing);

/* Adds name as a hidden column when it is a path into a column of the
 * header, and appends it to the header; HP_ERR_COLUMN_NOT_FOUND when it is
 * not one. */
hp_error_t hp_compute_path(hp_compute_t *c, hp_header_t *header, const char *name);

/* Starts a row parsed from raw_len bytes. With cut, a row longer than the
 * input's header loses the extra fields, since the new columns go there. */
hp_error_t hp_compute_begin(hp_compute_t *c, hp_row_t *row, size_t raw_len, bool cut);

/* Computes columns [from, to) into the row and returns the bytes of their
 * values */
size_t hp_compute_row(hp_compute_t *c, hp_row_t *row, size_t from, size_t to);

#endif /* HYPERPIPE_HP_COMPUTE_H */
