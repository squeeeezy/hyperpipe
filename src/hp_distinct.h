/* The (group, value) pairs count_distinct has seen: one table for all the
 * groups of an aggregation, so a group costs nothing until it has values.
 * A slot keeps the value's hash apart from the group, so a merge moves a
 * pair to another group without hashing its bytes again. Internal to src/. */

#ifndef HYPERPIPE_SRC_HP_DISTINCT_H
#define HYPERPIPE_SRC_HP_DISTINCT_H

#include "hyperpipe/hp_common.h"

typedef struct hp_distinct_slot_s {
    uint64_t hash;         /* of the value alone */
    uint64_t ref;          /* offset of the value in bytes, plus 1; 0 when free */
    uint32_t len;
    uint32_t group;
} hp_distinct_slot_t;

typedef struct hp_distinct_s {
    hp_distinct_slot_t *slots;
    size_t mask, count;
    char *bytes;
    size_t len, cap;
} hp_distinct_t;

/* Adds the pair; *added tells whether it is new */
hp_error_t hp_distinct_add(hp_distinct_t *d, uint64_t hash, uint32_t group,
                           const char *value, size_t len, bool *added);
void hp_distinct_free(hp_distinct_t *d);

static inline const char *hp_distinct_value(const hp_distinct_t *d, const hp_distinct_slot_t *s) {
    return d->bytes + (s->ref - 1);
}

#endif /* HYPERPIPE_SRC_HP_DISTINCT_H */
