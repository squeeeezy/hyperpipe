/* Paths into JSON values: meta.user.id, tags[-1] (hp_json_lookup) */

#include "hyperpipe/hp_json_ser.h"
#include "hp_json_text.h"

static size_t json_skip_space(const char *p, size_t i, size_t len) {
    while (i < len && json_is_space(p[i])) i++;
    return i;
}

/* Length of the value at p, following strings and nesting without checking
 * the grammar: the parser has checked an NDJSON value already, and text
 * from elsewhere only needs to stay within len. 0 when it runs past len. */
static size_t json_skip(const char *p, size_t len) {
    size_t i = 0, depth = 0;
    while (i < len) {
        char c = p[i];
        if (c == '"') {
            for (i++; i < len && p[i] != '"'; i++) i += p[i] == '\\';
            if (i++ >= len) return 0;
        } else if (c == '{' || c == '[') {
            depth++;
            i++;
        } else if (c == '}' || c == ']' || c == ',' || json_is_space(c)) {
            if (depth == 0) return i;      /* the end of a literal */
            depth -= (c == '}' || c == ']');
            i++;
        } else {
            i++;
        }
        if (depth == 0 && (c == '"' || c == '}' || c == ']')) return i;
    }
    return 0;
}

/* Whether the raw key between quotes reads as key. An escape decodes to at
 * least one byte per six, so a long raw key cannot match a short one. */
static bool json_key_is(const char *raw, size_t raw_len, const char *key, size_t key_len) {
    if (!memchr(raw, '\\', raw_len)) return raw_len == key_len && memcmp(raw, key, key_len) == 0;
    char buf[6 * 128];
    size_t n = 0;
    if (raw_len >= sizeof(buf) || json_decode_string(raw, raw_len + 1, buf, &n) == 0) return false;
    return n == key_len && memcmp(buf, key, key_len) == 0;
}

/* The member of the object at p named key: *at and *span locate its value */
static bool json_member(const char *p, size_t len, const char *key, size_t key_len,
                        size_t *at, size_t *span) {
    size_t i = json_skip_space(p, 1, len);
    while (i < len && p[i] == '"') {
        size_t k = json_skip(p + i, len - i);
        if (k == 0) return false;
        bool match = json_key_is(p + i + 1, k - 2, key, key_len);
        i = json_skip_space(p, i + k, len);
        if (i >= len || p[i] != ':') return false;
        i = json_skip_space(p, i + 1, len);
        size_t v = json_skip(p + i, len - i);
        if (v == 0) return false;
        if (match) {
            *at = i;
            *span = v;
            return true;
        }
        i = json_skip_space(p, i + v, len);
        if (i >= len || p[i] != ',') return false;
        i = json_skip_space(p, i + 1, len);
    }
    return false;
}

/* The element of the array at p at index, counted from 0, when *span
 * comes back nonzero; returns the number of elements passed, all of them
 * when index is out of range */
static int64_t json_element(const char *p, size_t len, int64_t index, size_t *at, size_t *span) {
    size_t i = json_skip_space(p, 1, len);
    if (i < len && p[i] == ']') return 0;
    for (int64_t k = 0;; k++) {
        size_t v = json_skip(p + i, len - i);
        if (v == 0) return -1;
        if (k == index) {
            *at = i;
            *span = v;
            return k;
        }
        i = json_skip_space(p, i + v, len);
        if (i >= len || p[i] != ',') return k + 1;
        i = json_skip_space(p, i + 1, len);
    }
}

bool hp_json_lookup(const char *json, size_t len, const hp_json_step_t *steps, size_t count,
                    hp_str_t *out, char *arena) {
    const char *p = json;
    size_t i = json_skip_space(p, 0, len);
    p += i;
    len -= i;
    for (size_t s = 0; s < count; s++) {
        const hp_json_step_t *step = &steps[s];
        size_t at = 0, span = 0;
        if (len > 0 && p[0] == '{' && step->key) {
            if (!json_member(p, len, step->key, step->key_len, &at, &span)) return false;
        } else if (len > 0 && p[0] == '[' && step->index != HP_JSON_NO_INDEX) {
            int64_t index = step->index;
            if (index < 0) index += json_element(p, len, INT64_MAX, &at, &span);
            if (index >= 0) json_element(p, len, index, &at, &span);
            if (span == 0) return false;
        } else {
            return false;
        }
        p += at;
        len = span;
    }

    /* The value the last step found, as json_skip() measured it: a string
     * ends with its closing quote. null is no value, as a missing key is. */
    if (count == 0 || len == 0 || (len == 4 && memcmp(p, "null", 4) == 0)) return false;
    *out = (hp_str_t){ .ptr = p, .len = len, .present = true };
    if (p[0] == '"') {
        out->ptr = p + 1;
        out->len = len - 2;
        out->has_escapes = true;
        if (memchr(out->ptr, '\\', out->len)) {
            if (json_decode_string(p + 1, len - 1, arena, &out->len) == 0) return false;
            out->ptr = arena;
        }
    } else if (p[0] == '{' || p[0] == '[') {
        out->is_json = true;
    }
    return true;
}
