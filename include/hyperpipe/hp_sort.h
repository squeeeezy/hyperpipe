#ifndef HYPERPIPE_HP_SORT_H
#define HYPERPIPE_HP_SORT_H

#include "hp_common.h"
#include <stdio.h>

/* --sort: rows are written in the order of their sort columns.
 *
 * A row is kept as its output line plus a key that compares with memcmp:
 * per column a type tag (numbers, then text, then empty values, in either
 * direction), then the number in an order-preserving encoding or the text
 * with its zero bytes escaped, inverted for a descending column. Equal keys
 * keep their input order: the sort is stable and runs merge in input order.
 * With a row limit the heap is not stable, so its keys end with the row's
 * position.
 *
 * Rows collect in memory up to a budget; then they are sorted into a run,
 * written to a temporary file, and all runs are merged at the end. With a
 * row limit only that many rows are kept, in a bounded heap, so --head
 * after --sort costs memory for N rows whatever the input.
 *
 * Parallel workers sort their chunks into runs with positions that follow
 * the input order; the coordinator absorbs the runs and the merge gives the
 * single-threaded result. */

#define HP_SORT_MAX_KEYS 16
#define HP_SORT_DEFAULT_BUDGET ((size_t)512 << 20)

typedef struct hp_sort_key_s {
    char name[128];
    int64_t col_index;
    bool desc;
} hp_sort_key_t;

/* The key's first 16 bytes are kept in the entry, so most comparisons never
 * touch the record: keys of numbers alone fit there whole */
typedef struct hp_sort_entry_s {
    uint64_t prefix[2];    /* big-endian, zero-padded */
    uint8_t *rec;          /* key length, line length, key, line */
} hp_sort_entry_t;

typedef struct hp_sort_block_s hp_sort_block_t;
typedef struct hp_sort_run_s hp_sort_run_t;

typedef struct hp_sorter_s {
    hp_sort_key_t keys[HP_SORT_MAX_KEYS];
    size_t key_count;
    uint64_t limit;        /* keep this many rows; 0 keeps all */
    size_t budget;         /* bytes of rows held before they become a run */
    uint64_t seq;          /* the next row's position */
    bool long_keys;        /* a key is longer than an entry's prefix */
    int threads;           /* threads hp_sorter_write() may merge with */

    hp_sort_entry_t *entries;  /* rows not yet in a run; a heap with a limit */
    size_t count, cap;
    size_t held;           /* bytes those rows take */
    hp_sort_block_t *blocks;   /* where their records live, without a limit */
    uint8_t *key;          /* scratch for one key */
    size_t key_cap;

    hp_sort_run_t *runs;
    size_t run_count, run_cap;
    size_t run_bytes;      /* bytes held by the runs in memory */
} hp_sorter_t;

/* Comma-separated columns, each optionally prefixed by '-' for descending.
 * HP_ERR_INVALID_ARG for an empty spec, more than 16 columns or a name over
 * 127 bytes. budget 0 takes the default. */
hp_error_t hp_sorter_init(hp_sorter_t *s, const char *spec, uint64_t limit, size_t budget);
void hp_sorter_free(hp_sorter_t *s);

/* Resolves the columns against the header. On HP_ERR_COLUMN_NOT_FOUND,
 * *missing names the column. */
hp_error_t hp_sorter_bind(hp_sorter_t *s, const hp_header_t *header, const char **missing);

/* Takes one output line, keyed by the row it was written from */
hp_error_t hp_sorter_add(hp_sorter_t *s, const hp_row_t *row, const void *line, size_t len);

/* Sorts the rows taken so far into a run. Parallel workers call it so the
 * sorting happens on their threads. */
hp_error_t hp_sorter_finish(hp_sorter_t *s);

/* Moves the runs of src into dst, src's after dst's; runs held in memory
 * past dst's budget are written to disk. src is left empty. */
hp_error_t hp_sorter_absorb(hp_sorter_t *dst, hp_sorter_t *src);

/* Writes every line in order, at most limit of them, and returns how many */
hp_error_t hp_sorter_write(hp_sorter_t *s, void (*emit)(void *ctx, const void *data, size_t len),
                           void *ctx, uint64_t *written);

#endif /* HYPERPIPE_HP_SORT_H */
