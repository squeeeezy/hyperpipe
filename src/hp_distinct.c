#include "hp_distinct.h"

/* The slot of a pair depends on both the value and the group */
static inline size_t slot_of(uint64_t hash, uint32_t group, size_t mask) {
    uint64_t h = (hash ^ ((uint64_t)group * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
    return (size_t)(h ^ (h >> 29)) & mask;
}

static hp_error_t grow(hp_distinct_t *d) {
    size_t cap = d->slots ? (d->mask + 1) * 2 : 1024;
    hp_distinct_slot_t *slots = (hp_distinct_slot_t *)calloc(cap, sizeof(*slots));
    if (!slots) return HP_ERR_NOMEM;
    for (size_t i = 0; d->slots && i <= d->mask; i++) {
        const hp_distinct_slot_t *s = &d->slots[i];
        if (!s->ref) continue;
        size_t j = slot_of(s->hash, s->group, cap - 1);
        while (slots[j].ref) j = (j + 1) & (cap - 1);
        slots[j] = *s;
    }
    free(d->slots);
    d->slots = slots;
    d->mask = cap - 1;
    return HP_OK;
}

hp_error_t hp_distinct_add(hp_distinct_t *d, uint64_t hash, uint32_t group,
                           const char *value, size_t len, bool *added) {
    *added = false;
    if (!d->slots && grow(d) != HP_OK) return HP_ERR_NOMEM;
    size_t i = slot_of(hash, group, d->mask);
    for (; d->slots[i].ref; i = (i + 1) & d->mask) {
        const hp_distinct_slot_t *s = &d->slots[i];
        if (s->hash == hash && s->group == group && s->len == len &&
            memcmp(hp_distinct_value(d, s), value, len) == 0) {
            return HP_OK;
        }
    }

    if (d->len + len > d->cap) {
        size_t cap = d->cap ? d->cap : 4096;
        while (cap < d->len + len) cap *= 2;
        char *bytes = (char *)realloc(d->bytes, cap);
        if (!bytes) return HP_ERR_NOMEM;
        d->bytes = bytes;
        d->cap = cap;
    }
    if (len) memcpy(d->bytes + d->len, value, len);
    d->slots[i] = (hp_distinct_slot_t){ .hash = hash, .ref = d->len + 1, .len = (uint32_t)len, .group = group };
    d->len += len;
    *added = true;

    /* At most half full */
    return 2 * ++d->count > d->mask + 1 ? grow(d) : HP_OK;
}

void hp_distinct_free(hp_distinct_t *d) {
    free(d->slots);
    free(d->bytes);
    memset(d, 0, sizeof(*d));
}
