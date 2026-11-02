#ifndef HYPERPIPE_HP_KEYMAP_H
#define HYPERPIPE_HP_KEYMAP_H

#include "hp_common.h"

/* Open-addressing hash index over a running NDJSON header.
 *
 * Resolving a field's column by key used to walk the header with strcmp per
 * field per row: linear in the number of columns, which turns wide-row
 * NDJSON input into O(cols) work even when every key hits the first slot.
 * The keymap hashes each key once and probes a power-of-two slot table, so
 * lookup is O(1).
 *
 * Short keys (<= 8 bytes - the overwhelming majority in real data) get a
 * branch-free fast path: the slot stores the first 8 key bytes zero-padded
 * plus the length, so a hash hit is confirmed with two integer compares and
 * no memory walk. Longer keys fall back to a full verification against
 * header->names[idx], so arbitrary key lengths stay correct.
 *
 * The map indexes the header by position: header->names[idx] must stay
 * alive and unchanged for every indexed idx. Schema growth mid-stream is
 * supported through hp_keymap_insert(); rehashing on insert re-reads the
 * header, so there is no stale-copy problem.
 *
 * Not thread-safe: each streaming worker owns its own map over its own
 * header copy. */

typedef struct hp_keymap_s {
    uint64_t *keys8;    /* first 8 key bytes, zero-padded (short-key path) */
    uint32_t *lens;     /* key length per slot */
    uint32_t *hashes;   /* stored key hash per slot; 0 marks an empty slot */
    uint32_t *indices;  /* header column index per slot */
    size_t mask;        /* slot_count - 1 (slot_count is a power of two) */
    size_t count;       /* number of live entries */
} hp_keymap_t;

/* Builds the index over the current header contents. */
hp_error_t hp_keymap_build(hp_keymap_t *map, const hp_header_t *header);

/* Releases the slot table; the struct returns to the empty state. */
void hp_keymap_free(hp_keymap_t *map);

/* Looks up key (len bytes, NOT NUL-terminated) and returns its column index,
 * or -1 when the key is not indexed.
 *
 * padded_key hints that reading up to 8 bytes at `key` is safe even when
 * len < 8 (the NDJSON parser reads inside the SIMD-padded ring buffer).
 * When false, short keys are padded byte-by-byte - the safe choice for any
 * other caller. */
int64_t hp_keymap_find(const hp_keymap_t *map, const hp_header_t *header,
                       const char *key, size_t len, bool padded_key);

/* Adds one column to the index (used when the header grows mid-stream).
 * The map grows (rehashes) automatically past a 0.5 load factor. */
hp_error_t hp_keymap_insert(hp_keymap_t *map, const hp_header_t *header,
                            size_t index);

#endif /* HYPERPIPE_HP_KEYMAP_H */
