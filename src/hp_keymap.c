#include "hyperpipe/hp_keymap.h"

/* Zero-pads the first 8 bytes of key into a little-endian word. The padded
 * variant reads a full 8 bytes with memcpy and is only legal when the
 * caller guarantees 8 readable bytes (the ring buffer's SIMD padding);
 * the byte-wise variant is always safe. */
HP_INLINE uint64_t hp_key_first8(const char *key, size_t len, bool padded) {
    uint64_t v = 0;
    if (padded && len < 8) {
        memcpy(&v, key, 8);
        /* clear the bytes beyond len: with a little-endian load the bytes
         * past len are the high ones */
        v &= (len == 0) ? 0ull : (~0ull >> (64 - 8 * len));
        return v;
    }
    size_t n = len < 8 ? len : 8;
    if (n == 8) {
        memcpy(&v, key, 8);
    } else {
        for (size_t i = 0; i < n; i++) {
            v |= (uint64_t)(uint8_t)key[i] << (8 * i);
        }
    }
    return v;
}

/* Hash for short keys: one multiply-high off the packed word. Cheaper than
 * a byte-loop FNV and well-distributed for the short identifiers that
 * dominate real NDJSON. */
HP_INLINE uint32_t hp_key_hash8(uint64_t key8, size_t len) {
    uint64_t h = (key8 ^ ((uint64_t)len << 56)) * 0x9E3779B97F4A7C15ull;
    uint32_t h32 = (uint32_t)(h >> 32);
    return h32 ? h32 : 1u;
}

/* FNV-1a over a byte span, used for keys longer than 8 bytes. The raw key
 * bytes are hashed exactly as they appear in the line - escape sequences
 * inside quoted keys are not decoded, matching the header names captured
 * raw from earlier lines. */
static uint32_t hp_key_hash_long(const char *key, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

HP_INLINE uint32_t hp_key_hash(const char *key, size_t len, bool padded) {
    if (len <= 8) {
        return hp_key_hash8(hp_key_first8(key, len, padded), len);
    }
    return hp_key_hash_long(key, len);
}

static hp_error_t hp_keymap_reserve(hp_keymap_t *map, size_t min_slots) {
    size_t slots = 16;
    while (slots < min_slots) slots *= 2;

    uint64_t *keys8 = (uint64_t *)malloc(slots * sizeof(uint64_t));
    uint32_t *lens = (uint32_t *)malloc(slots * sizeof(uint32_t));
    uint32_t *hashes = (uint32_t *)calloc(slots, sizeof(uint32_t));
    uint32_t *indices = (uint32_t *)malloc(slots * sizeof(uint32_t));
    if (!keys8 || !lens || !hashes || !indices) {
        free(keys8);
        free(lens);
        free(hashes);
        free(indices);
        return HP_ERR_NOMEM;
    }

    free(map->keys8);
    free(map->lens);
    free(map->hashes);
    free(map->indices);
    map->keys8 = keys8;
    map->lens = lens;
    map->hashes = hashes;
    map->indices = indices;
    map->mask = slots - 1;
    map->count = 0;
    return HP_OK;
}

static void hp_keymap_put(hp_keymap_t *map, const char *name, uint32_t index) {
    size_t len = strlen(name);
    uint32_t h = hp_key_hash(name, len, false);
    size_t slot = h & map->mask;
    while (map->hashes[slot] != 0) {
        slot = (slot + 1) & map->mask;
    }
    map->hashes[slot] = h;
    map->lens[slot] = (uint32_t)len;
    map->keys8[slot] = hp_key_first8(name, len, false);
    map->indices[slot] = index;
    map->count++;
}

hp_error_t hp_keymap_build(hp_keymap_t *map, const hp_header_t *header) {
    if (!map || !header) return HP_ERR_INVALID_ARG;

    /* One entry per column, kept at or below a 0.5 load factor. */
    hp_error_t err = hp_keymap_reserve(map, (header->count + 1) * 2);
    if (err != HP_OK) return err;

    for (size_t i = 0; i < header->count; i++) {
        const char *name = header->names[i] ? header->names[i] : "";
        hp_keymap_put(map, name, (uint32_t)i);
    }
    return HP_OK;
}

void hp_keymap_free(hp_keymap_t *map) {
    if (!map) return;
    free(map->keys8);
    free(map->lens);
    free(map->hashes);
    free(map->indices);
    map->keys8 = NULL;
    map->lens = NULL;
    map->hashes = NULL;
    map->indices = NULL;
    map->mask = 0;
    map->count = 0;
}

int64_t hp_keymap_find(const hp_keymap_t *map, const hp_header_t *header,
                       const char *key, size_t len, bool padded_key) {
    if (!map || !map->hashes || !header) return -1;

    uint32_t h = hp_key_hash(key, len, padded_key);
    size_t slot = h & map->mask;

    while (map->hashes[slot] != 0) {
        if (map->hashes[slot] == h && map->lens[slot] == len) {
            if (len <= 8) {
                /* Branch-free confirmation: packed bytes + length match
                 * without touching header memory */
                if (map->keys8[slot] == hp_key_first8(key, len, padded_key)) {
                    return (int64_t)map->indices[slot];
                }
            } else {
                size_t idx = map->indices[slot];
                const char *name = (idx < header->count && header->names[idx])
                                       ? header->names[idx] : NULL;
                if (name && memcmp(name, key, len) == 0) {
                    return (int64_t)idx;
                }
            }
        }
        slot = (slot + 1) & map->mask;
    }
    return -1;
}

hp_error_t hp_keymap_insert(hp_keymap_t *map, const hp_header_t *header,
                            size_t index) {
    if (!map || !header || index >= HP_MAX_COLUMNS) return HP_ERR_INVALID_ARG;

    /* Grow before the load factor passes 0.5. Rehashing walks the header
     * itself, so the table can never hold a stale view of the columns. */
    size_t slots = map->mask + 1;
    if (slots == 0 || (map->count + 1) * 2 > slots) {
        hp_error_t err = hp_keymap_build(map, header);
        if (err != HP_OK) return err;
        if (index < header->count) return HP_OK; /* rebuild already indexed it */
    }

    const char *name = header->names[index] ? header->names[index] : "";
    hp_keymap_put(map, name, (uint32_t)index);
    return HP_OK;
}
