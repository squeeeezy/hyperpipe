#include "hyperpipe/hp_agg.h"
#include "hyperpipe/hp_filter.h"
#include "hp_distinct.h"
#include "hp_number.h"
#include "hp_quantile.h"

/* Exact sums are scaled integers. 128 bits hold any 18-digit value at any
 * of the 19 scales with room for 10^20 more additions; without a 128-bit
 * type, sums stay exact while they fit in 64. */
#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 hp_wide_t;
__extension__ typedef unsigned __int128 hp_uwide_t;
#else
typedef int64_t hp_wide_t;
typedef uint64_t hp_uwide_t;
#endif

/* A number from the input: mant * 10^-scale when exact, otherwise f */
typedef struct hp_agg_num_s {
    int64_t mant;
    int32_t scale;
    bool exact;
    double f;
} hp_agg_num_t;

struct hp_agg_acc_s {
    uint64_t n;            /* values folded in; rows for count */
    hp_wide_t sum;         /* exact sum, scaled by 10^scale */
    int32_t scale;
    bool inexact;          /* the sum left the exact range: fsum holds it */
    double fsum;
    hp_agg_num_t min, max;
    hp_quantile_t *quant;  /* the values of a quantile */
};

struct hp_agg_group_s {
    uint64_t hash;
    size_t key_off;
    size_t key_len;
};

/* A key is stored as, per column: a 4-byte length, a flags byte, the bytes */
#define KEY_PRESENT 1u
#define KEY_ESCAPES 2u
#define KEY_JSON    4u
#define KEY_FIELD_HEADER 5

static const int64_t pow10_i64[HP_DEC_MAX_DIGITS + 1] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
    10000000000, 100000000000, 1000000000000, 10000000000000, 100000000000000,
    1000000000000000, 10000000000000000, 100000000000000000, 1000000000000000000,
};

/* ---- parsing ---------------------------------------------------------------- */

static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static hp_error_t agg_error(hp_agg_t *agg, const char *text, const char *at, const char *why) {
    agg->error = why;
    agg->error_at = (size_t)(at - text);
    return HP_ERR_AGG;
}

static const struct {
    const char *name;
    hp_agg_fn_t fn;
    double q;
} agg_names[] = {
    { "count", HP_AGG_COUNT, 0 }, { "sum", HP_AGG_SUM, 0 }, { "avg", HP_AGG_AVG, 0 },
    { "min", HP_AGG_MIN, 0 },     { "max", HP_AGG_MAX, 0 },
    { "count_distinct", HP_AGG_COUNT_DISTINCT, 0 }, { "median", HP_AGG_QUANTILE, 0.5 },
};

/* column may be spec->column itself, so the name is built apart first */
static void set_name(hp_agg_spec_t *spec, const char *column) {
    char name[sizeof(spec->name)];
    if (column[0]) {
        snprintf(name, sizeof(name), "%s(%s)", spec->label, column);
    } else {
        snprintf(name, sizeof(name), "%s", spec->label);
    }
    memcpy(spec->name, name, sizeof(name));
}

/* A function name, or pN for the N-th percentile: p95, p99.9 */
static bool name_fn(const char *word, size_t len, hp_agg_spec_t *spec) {
    for (size_t k = 0; k < sizeof(agg_names) / sizeof(agg_names[0]); k++) {
        if (strlen(agg_names[k].name) == len && strncmp(word, agg_names[k].name, len) == 0) {
            spec->fn = agg_names[k].fn;
            spec->q = agg_names[k].q;
            return true;
        }
    }
    /* p99.9 is 999 / 1000 in one division, the double nearest 0.999, where
     * 99.9 / 100 would round twice */
    int64_t mant;
    int scale;
    if (len >= 2 && len < sizeof(spec->label) && word[0] == 'p' && isdigit((unsigned char)word[1]) &&
        hp_parse_decimal_exact(word + 1, len - 1, &mant, &scale) && scale <= 14 &&
        mant >= 0 && mant <= 100 * pow10_i64[scale]) {
        spec->fn = HP_AGG_QUANTILE;
        spec->q = (double)mant / (double)pow10_i64[scale + 2];
        return true;
    }
    return false;
}

/* "count", or "fn(column)": count(column) counts the non-empty values */
static hp_error_t parse_fns(hp_agg_t *agg, const char *text) {
    const char *p = skip_space(text);
    for (;;) {
        const char *word = p;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') p++;
        size_t word_len = (size_t)(p - word);
        if (agg->fn_count == HP_AGG_MAX_FNS) {
            return agg_error(agg, text, word, "more than 32 functions");
        }
        hp_agg_spec_t *spec = &agg->fns[agg->fn_count];
        if (!name_fn(word, word_len, spec)) {
            return agg_error(agg, text, word, "expected count, count_distinct, sum, avg, min, max, "
                                              "median or a percentile such as p95");
        }
        memcpy(spec->label, word, word_len);
        spec->label[word_len] = '\0';
        spec->col_index = -1;

        p = skip_space(p);
        if (*p == '(') {
            const char *col = skip_space(p + 1);
            const char *end = col;
            while (*end && *end != ')' && *end != ',') end++;
            if (*end != ')') return agg_error(agg, text, end, "expected ')'");
            p = end + 1;
            while (end > col && isspace((unsigned char)end[-1])) end--;
            size_t col_len = (size_t)(end - col);
            if (col_len == 0) return agg_error(agg, text, col, "expected a column name");
            if (col_len >= sizeof(spec->column)) {
                return agg_error(agg, text, col, "column name longer than 127 bytes");
            }
            memcpy(spec->column, col, col_len);
            spec->column[col_len] = '\0';
        } else if (spec->fn != HP_AGG_COUNT) {
            return agg_error(agg, text, p, "expected '(' and a column");
        }
        set_name(spec, spec->column);
        agg->fn_count++;

        p = skip_space(p);
        if (!*p) return HP_OK;
        if (*p != ',') return agg_error(agg, text, p, "expected ',' or the end");
        p = skip_space(p + 1);
    }
}

/* "column", or "hour(column)" and the other time units */
static hp_error_t parse_keys(hp_agg_t *agg, const char *text) {
    const char *p = text;
    for (;;) {
        p = skip_space(p);
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;

        int unit = -1;
        const char *paren = memchr(start, '(', (size_t)(end - start));
        hp_time_unit_t u;
        if (paren && end[-1] == ')' && hp_time_unit_from_name(start, (size_t)(paren - start), &u)) {
            unit = (int)u;
            start = skip_space(paren + 1);
            end--;
            while (end > start && isspace((unsigned char)end[-1])) end--;
            if (end == start) return HP_ERR_INVALID_ARG;
        }
        size_t len = (size_t)(end - start);
        if (len > 0) {
            if (agg->key_count == HP_AGG_MAX_KEYS || len >= sizeof(agg->keys[0])) {
                return HP_ERR_INVALID_ARG;
            }
            memcpy(agg->keys[agg->key_count], start, len);
            agg->keys[agg->key_count][len] = '\0';
            agg->key_unit[agg->key_count] = unit;
            agg->key_index[agg->key_count++] = -1;
        }
        if (!*p) return HP_OK;
        p++;
    }
}

hp_error_t hp_agg_init(hp_agg_t *agg, const char *group_by, const char *fns) {
    if (!agg) return HP_ERR_INVALID_ARG;
    memset(agg, 0, sizeof(*agg));

    hp_error_t err = HP_OK;
    if (group_by) err = parse_keys(agg, group_by);
    if (err == HP_OK && fns && *skip_space(fns)) err = parse_fns(agg, fns);
    if (err != HP_OK) return err;
    if (agg->fn_count == 0) {
        agg->fns[0].fn = HP_AGG_COUNT;
        agg->fns[0].col_index = -1;
        snprintf(agg->fns[0].label, sizeof(agg->fns[0].label), "count");
        set_name(&agg->fns[0], "");
        agg->fn_count = 1;
    }
    for (size_t i = 0; i < agg->fn_count; i++) {
        if (agg->fns[i].fn == HP_AGG_COUNT_DISTINCT && !agg->distinct) {
            agg->distinct = (hp_distinct_t *)calloc(agg->fn_count, sizeof(hp_distinct_t));
            if (!agg->distinct) return HP_ERR_NOMEM;
        }
    }
    return HP_OK;
}

void hp_agg_free(hp_agg_t *agg) {
    if (!agg) return;
    for (size_t i = 0; i < agg->fn_count; i++) {
        if (agg->fns[i].fn != HP_AGG_QUANTILE) continue;
        for (size_t g = 0; g < agg->group_count; g++) hp_quantile_free(agg->acc[g * agg->fn_count + i].quant);
    }
    for (size_t i = 0; agg->distinct && i < agg->fn_count; i++) hp_distinct_free(&agg->distinct[i]);
    free(agg->distinct);
    agg->distinct = NULL;
    free(agg->slots);
    free(agg->groups);
    free(agg->acc);
    free(agg->key_bytes);
    agg->slots = NULL;
    agg->groups = NULL;
    agg->acc = NULL;
    agg->key_bytes = NULL;
    agg->group_count = agg->group_cap = agg->key_len = agg->key_cap = 0;
}

hp_error_t hp_agg_bind(hp_agg_t *agg, const hp_header_t *header, const char **missing) {
    for (size_t k = 0; k < agg->key_count; k++) {
        int64_t idx = hp_resolve_column(header, agg->keys[k]);
        if (idx < 0) {
            *missing = agg->keys[k];
            return HP_ERR_COLUMN_NOT_FOUND;
        }
        agg->key_index[k] = idx;
        if ((size_t)idx < header->count) {
            snprintf(agg->keys[k], sizeof(agg->keys[k]), "%s", header->names[idx]);
        }
    }
    for (size_t i = 0; i < agg->fn_count; i++) {
        hp_agg_spec_t *spec = &agg->fns[i];
        if (!spec->column[0]) continue;
        int64_t idx = hp_resolve_column(header, spec->column);
        if (idx < 0) {
            *missing = spec->column;
            return HP_ERR_COLUMN_NOT_FOUND;
        }
        spec->col_index = idx;
        if ((size_t)idx < header->count) set_name(spec, header->names[idx]);
    }
    return HP_OK;
}

hp_error_t hp_agg_header(const hp_agg_t *agg, hp_header_t *out) {
    hp_header_free(out);
    for (size_t k = 0; k < agg->key_count; k++) {
        char name[160];
        if (agg->key_unit[k] >= 0) {
            snprintf(name, sizeof(name), "%s(%s)", hp_time_unit_name((hp_time_unit_t)agg->key_unit[k]),
                     agg->keys[k]);
        } else {
            snprintf(name, sizeof(name), "%s", agg->keys[k]);
        }
        if (!(out->names[out->count] = strdup(name))) return HP_ERR_NOMEM;
        out->count++;
    }
    for (size_t i = 0; i < agg->fn_count; i++) {
        if (!(out->names[out->count] = strdup(agg->fns[i].name))) return HP_ERR_NOMEM;
        out->count++;
    }
    return HP_OK;
}

/* ---- numbers ---------------------------------------------------------------- */

static bool scale_up(hp_wide_t *x, int digits) {
    return !__builtin_mul_overflow(*x, (hp_wide_t)pow10_i64[digits], x);
}

static double num_double(const hp_agg_num_t *v) {
    return v->exact ? (double)v->mant / (double)pow10_i64[v->scale] : v->f;
}

static bool parse_num(const hp_str_t *f, hp_agg_num_t *v) {
    int scale = 0;
    v->exact = hp_parse_decimal_exact(f->ptr, f->len, &v->mant, &scale);
    v->scale = scale;
    return v->exact || hp_parse_decimal_span(f->ptr, f->len, &v->f);
}

static int num_cmp(const hp_agg_num_t *a, const hp_agg_num_t *b) {
    if (a->exact && b->exact) {
        int s = a->scale > b->scale ? a->scale : b->scale;
        hp_wide_t x = a->mant, y = b->mant;
        if (scale_up(&x, s - a->scale) && scale_up(&y, s - b->scale)) return (x > y) - (x < y);
    }
    double x = num_double(a), y = num_double(b);
    return (x > y) - (x < y);
}

static double acc_sum_double(const hp_agg_acc_t *acc) {
    return acc->inexact ? acc->fsum : (double)acc->sum / (double)pow10_i64[acc->scale];
}

/* Adds x * 10^-scale to an exact sum; false when the result would not fit */
static bool add_exact(hp_agg_acc_t *acc, hp_wide_t x, int scale) {
    hp_wide_t sum = acc->sum;
    int s = acc->scale;
    if (scale > s) {
        if (!scale_up(&sum, scale - s)) return false;
        s = scale;
    } else if (!scale_up(&x, s - scale)) {
        return false;
    }
    if (__builtin_add_overflow(sum, x, &sum)) return false;
    acc->sum = sum;
    acc->scale = s;
    return true;
}

static void add_value(hp_agg_acc_t *acc, const hp_agg_num_t *v) {
    if (!acc->inexact) {
        if (v->exact && add_exact(acc, v->mant, v->scale)) return;
        acc->fsum = acc_sum_double(acc);
        acc->inexact = true;
    }
    acc->fsum += num_double(v);
}

static uint64_t hash_field(uint64_t h, const char *p, size_t n, bool present);

static hp_error_t fold(hp_agg_t *agg, size_t fn, size_t group, hp_agg_acc_t *acc, const hp_row_t *row) {
    const hp_agg_spec_t *spec = &agg->fns[fn];
    const hp_str_t *f = NULL;
    if (spec->col_index >= 0 && (size_t)spec->col_index < row->field_count &&
        row->fields[spec->col_index].present && row->fields[spec->col_index].len > 0) {
        f = &row->fields[spec->col_index];
    }
    if (spec->fn == HP_AGG_COUNT) {
        acc->n += (!spec->column[0] || f);
        return HP_OK;
    }
    if (spec->fn == HP_AGG_COUNT_DISTINCT) {
        if (!f) return HP_OK;
        bool added = false;
        hp_error_t err = hp_distinct_add(&agg->distinct[fn], hash_field(0, f->ptr, f->len, true),
                                         (uint32_t)group, f->ptr, f->len, &added);
        acc->n += added;
        return err;
    }
    if (spec->fn == HP_AGG_QUANTILE) {
        int64_t i;
        double d;
        if (!f || f->is_json) return HP_OK;
        if (hp_parse_int_span(f->ptr, f->len, &i)) {
            d = (double)i;
        } else if (!hp_parse_decimal_span(f->ptr, f->len, &d)) {
            return HP_OK;
        }
        acc->n++;
        return hp_quantile_add(&acc->quant, d);
    }

    hp_agg_num_t v;
    if (!f || f->is_json || !parse_num(f, &v)) return HP_OK;
    acc->n++;
    if (spec->fn == HP_AGG_SUM || spec->fn == HP_AGG_AVG) {
        add_value(acc, &v);
    } else if (spec->fn == HP_AGG_MIN) {
        if (acc->n == 1 || num_cmp(&v, &acc->min) < 0) acc->min = v;
    } else {
        if (acc->n == 1 || num_cmp(&v, &acc->max) > 0) acc->max = v;
    }
    return HP_OK;
}

/* Distinct values are merged by hp_agg_merge(), which knows the groups */
static hp_error_t merge_acc(hp_agg_acc_t *dst, const hp_agg_acc_t *src, hp_agg_fn_t fn) {
    if (src->n == 0 || fn == HP_AGG_COUNT_DISTINCT) return HP_OK;
    if (fn == HP_AGG_QUANTILE) {
        dst->n += src->n;
        return hp_quantile_merge(&dst->quant, src->quant);
    }
    if (dst->n == 0) {
        *dst = *src;
        return HP_OK;
    }
    dst->n += src->n;
    if (fn == HP_AGG_SUM || fn == HP_AGG_AVG) {
        if (!dst->inexact && !src->inexact && add_exact(dst, src->sum, src->scale)) return HP_OK;
        dst->fsum = acc_sum_double(dst) + acc_sum_double(src);
        dst->inexact = true;
    } else if (fn == HP_AGG_MIN) {
        if (num_cmp(&src->min, &dst->min) < 0) dst->min = src->min;
    } else if (fn == HP_AGG_MAX) {
        if (num_cmp(&src->max, &dst->max) > 0) dst->max = src->max;
    }
    return HP_OK;
}

/* ---- the table -------------------------------------------------------------- */

#define HASH_K 0x9E3779B97F4A7C15ull

HP_INLINE uint64_t hash_mix(uint64_t h, uint64_t w) {
    h = (h ^ w) * HASH_K;
    return h ^ (h >> 31);
}

static uint64_t hash_field(uint64_t h, const char *p, size_t n, bool present) {
    h = hash_mix(h, ((uint64_t)n << 1) | present);
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = hash_mix(h, w);
    }
    if (n > 0) {
        uint64_t w = 0;
        for (size_t i = 0; i < n; i++) w |= (uint64_t)(uint8_t)p[i] << (8 * i);
        h = hash_mix(h, w);
    }
    return h;
}

/* The row's value for each key column: the field, or for hour(col) and the
 * like the bucket of its time. NULL for a missing value and for one that
 * is not a time. */
static void row_keys(hp_agg_t *agg, const hp_row_t *row, const hp_str_t **vals, hp_str_t *buckets) {
    for (size_t k = 0; k < agg->key_count; k++) {
        size_t idx = (size_t)agg->key_index[k];
        const hp_str_t *f = idx < row->field_count && row->fields[idx].present ? &row->fields[idx] : NULL;
        int64_t us;
        if (f && agg->key_unit[k] >= 0) {
            if (!f->is_json && hp_time_parse(f->ptr, f->len, &us)) {
                size_t len = hp_time_format(us, (hp_time_unit_t)agg->key_unit[k], agg->key_text[k]);
                buckets[k] = (hp_str_t){ .ptr = agg->key_text[k], .len = len, .present = true };
                f = &buckets[k];
            } else {
                f = NULL;
            }
        }
        vals[k] = f;
    }
}

static uint64_t row_hash(const hp_agg_t *agg, const hp_str_t *const *vals) {
    uint64_t h = 0;
    for (size_t k = 0; k < agg->key_count; k++) {
        const hp_str_t *f = vals[k];
        h = f ? hash_field(h, f->ptr, f->len, true) : hash_field(h, "", 0, false);
    }
    return h;
}

static uint32_t read_len(const char *p) {
    uint32_t len;
    memcpy(&len, p, 4);
    return len;
}

static bool row_matches(const hp_agg_t *agg, const char *key, const hp_str_t *const *vals) {
    for (size_t k = 0; k < agg->key_count; k++) {
        const hp_str_t *f = vals[k];
        uint32_t len = read_len(key);
        bool present = (key[4] & KEY_PRESENT) != 0;
        if (present != (f != NULL)) return false;
        if (f && (len != f->len || memcmp(key + KEY_FIELD_HEADER, f->ptr, len) != 0)) return false;
        key += KEY_FIELD_HEADER + len;
    }
    return true;
}

/* Equal keys from two tables: the flags may differ, the values may not */
static bool keys_match(const hp_agg_t *agg, const char *a, const char *b) {
    for (size_t k = 0; k < agg->key_count; k++) {
        uint32_t len = read_len(a);
        if (len != read_len(b) || ((a[4] ^ b[4]) & KEY_PRESENT) ||
            memcmp(a + KEY_FIELD_HEADER, b + KEY_FIELD_HEADER, len) != 0) {
            return false;
        }
        a += KEY_FIELD_HEADER + len;
        b += KEY_FIELD_HEADER + len;
    }
    return true;
}

static hp_error_t reserve_keys(hp_agg_t *agg, size_t more) {
    if (agg->key_len + more <= agg->key_cap) return HP_OK;
    size_t cap = agg->key_cap ? agg->key_cap : 4096;
    while (cap < agg->key_len + more) cap *= 2;
    char *bytes = (char *)realloc(agg->key_bytes, cap);
    if (!bytes) return HP_ERR_NOMEM;
    agg->key_bytes = bytes;
    agg->key_cap = cap;
    return HP_OK;
}

/* Appends the row's key to key_bytes, without claiming it yet */
static hp_error_t encode_row_key(hp_agg_t *agg, const hp_str_t *const *vals, size_t *key_len) {
    size_t need = 0;
    for (size_t k = 0; k < agg->key_count; k++) {
        need += KEY_FIELD_HEADER + (vals[k] ? vals[k]->len : 0);
    }
    if (reserve_keys(agg, need) != HP_OK) return HP_ERR_NOMEM;

    char *w = agg->key_bytes + agg->key_len;
    for (size_t k = 0; k < agg->key_count; k++) {
        const hp_str_t *f = vals[k];
        uint32_t len = f ? (uint32_t)f->len : 0;
        memcpy(w, &len, 4);
        w[4] = (char)(f ? KEY_PRESENT | (f->has_escapes ? KEY_ESCAPES : 0) |
                          (f->is_json ? KEY_JSON : 0)
                        : 0);
        if (len) memcpy(w + KEY_FIELD_HEADER, f->ptr, len);
        w += KEY_FIELD_HEADER + len;
    }
    *key_len = need;
    return HP_OK;
}

static hp_error_t grow_slots(hp_agg_t *agg) {
    size_t cap = agg->slots ? (agg->slot_mask + 1) * 2 : 1024;
    uint32_t *slots = (uint32_t *)calloc(cap, sizeof(*slots));
    if (!slots) return HP_ERR_NOMEM;
    for (size_t g = 0; g < agg->group_count; g++) {
        size_t i = agg->groups[g].hash & (cap - 1);
        while (slots[i]) i = (i + 1) & (cap - 1);
        slots[i] = (uint32_t)g + 1;
    }
    free(agg->slots);
    agg->slots = slots;
    agg->slot_mask = cap - 1;
    return HP_OK;
}

/* Adds a group whose key was just encoded at the end of key_bytes */
static hp_error_t add_group(hp_agg_t *agg, uint64_t hash, size_t key_len, size_t *g) {
    if (agg->group_count == UINT32_MAX - 1) return HP_ERR_NOMEM;
    if (agg->group_count == agg->group_cap) {
        size_t cap = agg->group_cap ? agg->group_cap * 2 : 256;
        hp_agg_group_t *groups = (hp_agg_group_t *)realloc(agg->groups, cap * sizeof(*groups));
        if (!groups) return HP_ERR_NOMEM;
        agg->groups = groups;
        hp_agg_acc_t *acc = (hp_agg_acc_t *)realloc(agg->acc, cap * agg->fn_count * sizeof(*acc));
        if (!acc) return HP_ERR_NOMEM;
        agg->acc = acc;
        agg->group_cap = cap;
    }
    *g = agg->group_count++;
    agg->groups[*g] = (hp_agg_group_t){ .hash = hash, .key_off = agg->key_len, .key_len = key_len };
    agg->key_len += key_len;
    memset(&agg->acc[*g * agg->fn_count], 0, agg->fn_count * sizeof(hp_agg_acc_t));

    /* At most half full, so probes stay short */
    if (2 * agg->group_count > agg->slot_mask + 1) return grow_slots(agg);
    size_t i = hash & agg->slot_mask;
    while (agg->slots[i]) i = (i + 1) & agg->slot_mask;
    agg->slots[i] = (uint32_t)*g + 1;
    return HP_OK;
}

static hp_error_t find_row_group(hp_agg_t *agg, const hp_row_t *row, size_t *g) {
    if (!agg->slots && grow_slots(agg) != HP_OK) return HP_ERR_NOMEM;
    const hp_str_t *vals[HP_AGG_MAX_KEYS];
    hp_str_t buckets[HP_AGG_MAX_KEYS];
    row_keys(agg, row, vals, buckets);
    uint64_t hash = row_hash(agg, vals);
    for (size_t i = hash & agg->slot_mask; agg->slots[i]; i = (i + 1) & agg->slot_mask) {
        const hp_agg_group_t *grp = &agg->groups[agg->slots[i] - 1];
        if (grp->hash == hash && row_matches(agg, agg->key_bytes + grp->key_off, vals)) {
            *g = agg->slots[i] - 1;
            return HP_OK;
        }
    }
    size_t key_len = 0;
    hp_error_t err = encode_row_key(agg, vals, &key_len);
    if (err != HP_OK) return err;
    return add_group(agg, hash, key_len, g);
}

hp_error_t hp_agg_observe(hp_agg_t *agg, const hp_row_t *row) {
    size_t g = 0;
    if (agg->key_count > 0) {
        hp_error_t err = find_row_group(agg, row, &g);
        if (err != HP_OK) return err;
    } else if (agg->group_count == 0) {
        if (!agg->slots && grow_slots(agg) != HP_OK) return HP_ERR_NOMEM;
        hp_error_t err = add_group(agg, 0, 0, &g);
        if (err != HP_OK) return err;
    }
    hp_agg_acc_t *acc = &agg->acc[g * agg->fn_count];
    for (size_t i = 0; i < agg->fn_count; i++) {
        hp_error_t err = fold(agg, i, g, &acc[i], row);
        if (err != HP_OK) return err;
    }
    return HP_OK;
}

hp_error_t hp_agg_merge(hp_agg_t *dst, const hp_agg_t *src) {
    if (!dst->slots && grow_slots(dst) != HP_OK) return HP_ERR_NOMEM;
    uint32_t *to = (uint32_t *)malloc((src->group_count ? src->group_count : 1) * sizeof(uint32_t));
    if (!to) return HP_ERR_NOMEM;
    hp_error_t err = HP_OK;
    for (size_t sg = 0; err == HP_OK && sg < src->group_count; sg++) {
        const hp_agg_group_t *from = &src->groups[sg];
        const char *key = src->key_bytes + from->key_off;

        size_t g = SIZE_MAX;
        for (size_t i = from->hash & dst->slot_mask; dst->slots[i]; i = (i + 1) & dst->slot_mask) {
            const hp_agg_group_t *grp = &dst->groups[dst->slots[i] - 1];
            if (grp->hash == from->hash && keys_match(dst, dst->key_bytes + grp->key_off, key)) {
                g = dst->slots[i] - 1;
                break;
            }
        }
        if (g == SIZE_MAX) {
            err = reserve_keys(dst, from->key_len);
            if (err != HP_OK) break;
            if (from->key_len) memcpy(dst->key_bytes + dst->key_len, key, from->key_len);
            if ((err = add_group(dst, from->hash, from->key_len, &g)) != HP_OK) break;
        }
        to[sg] = (uint32_t)g;
        for (size_t i = 0; err == HP_OK && i < dst->fn_count; i++) {
            err = merge_acc(&dst->acc[g * dst->fn_count + i], &src->acc[sg * src->fn_count + i],
                            dst->fns[i].fn);
        }
    }

    /* Each distinct pair of src joins its group in dst, counted when new */
    for (size_t i = 0; err == HP_OK && src->distinct && i < dst->fn_count; i++) {
        const hp_distinct_t *d = &src->distinct[i];
        for (size_t s = 0; err == HP_OK && d->slots && s <= d->mask; s++) {
            const hp_distinct_slot_t *slot = &d->slots[s];
            if (!slot->ref) continue;
            bool added = false;
            uint32_t g = to[slot->group];
            err = hp_distinct_add(&dst->distinct[i], slot->hash, g, hp_distinct_value(d, slot),
                                  slot->len, &added);
            dst->acc[g * dst->fn_count + i].n += added;
        }
    }
    free(to);
    return err;
}

/* ---- output ----------------------------------------------------------------- */

size_t hp_agg_rows(const hp_agg_t *agg) {
    return (agg->key_count == 0 && agg->group_count == 0) ? 1 : agg->group_count;
}

static size_t format_u64(char *out, uint64_t v) {
    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++) out[i] = digits[n - 1 - i];
    return n;
}

/* v * 10^-scale as decimal text: "-12.50", "0.05" */
static size_t format_decimal(char *out, hp_wide_t v, int scale) {
    char digits[48];
    size_t n = 0, w = 0;
    hp_uwide_t u = v < 0 ? (hp_uwide_t)0 - (hp_uwide_t)v : (hp_uwide_t)v;
    do {
        digits[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u);
    while (n <= (size_t)scale) digits[n++] = '0';

    if (v < 0) out[w++] = '-';
    for (size_t i = n; i-- > 0;) {
        out[w++] = digits[i];
        if (i == (size_t)scale && scale > 0) out[w++] = '.';
    }
    return w;
}

/* The shortest text that reads back as the same double */
static size_t format_double(char *out, double v) {
    int n = 0;
    for (int prec = 15; prec <= 17; prec++) {
        n = snprintf(out, HP_AGG_VALUE_MAX, "%.*g", prec, v);
        if (strtod(out, NULL) == v) break;
    }
    return (size_t)n;
}

static size_t format_num(char *out, const hp_agg_num_t *v) {
    return v->exact ? format_decimal(out, v->mant, v->scale) : format_double(out, v->f);
}

/* Writes one aggregate value; returns 0 when it has none (no numbers) */
static size_t format_acc(char *out, const hp_agg_acc_t *acc, const hp_agg_spec_t *spec) {
    const hp_agg_fn_t fn = spec->fn;
    if (fn == HP_AGG_COUNT || fn == HP_AGG_COUNT_DISTINCT) return format_u64(out, acc->n);
    if (acc->n == 0) return 0;
    double v;
    switch (fn) {
        case HP_AGG_QUANTILE:
            return hp_quantile_get(acc->quant, spec->q, &v) ? format_double(out, v) : 0;
        case HP_AGG_SUM:
            return acc->inexact ? format_double(out, acc->fsum)
                                : format_decimal(out, acc->sum, acc->scale);
        case HP_AGG_AVG: {
            if (acc->inexact) return format_double(out, acc->fsum / (double)acc->n);
            /* sum / (n * 10^scale) from the exact sum, so the result does
             * not depend on the order rows were folded in. Below 2^53 both
             * are exact doubles and one division rounds correctly. */
            const hp_wide_t exact53 = (hp_wide_t)1 << 53;
            hp_wide_t den = (hp_wide_t)acc->n;
            if (!scale_up(&den, acc->scale)) {
                return format_double(out, acc_sum_double(acc) / (double)acc->n);
            }
            if (acc->sum <= exact53 && acc->sum >= -exact53 && den <= exact53) {
                return format_double(out, (double)acc->sum / (double)den);
            }
            hp_wide_t q = acc->sum / den, r = acc->sum % den;
            return format_double(out, (double)q + (double)r / (double)den);
        }
        case HP_AGG_MIN: return format_num(out, &acc->min);
        case HP_AGG_MAX: return format_num(out, &acc->max);
        default: return 0;
    }
}

void hp_agg_row(const hp_agg_t *agg, size_t i, hp_row_t *out, char *scratch) {
    static const hp_agg_acc_t no_rows;
    size_t w = 0;

    if (i < agg->group_count) {
        const char *key = agg->key_bytes + agg->groups[i].key_off;
        for (size_t k = 0; k < agg->key_count; k++) {
            uint32_t len = read_len(key);
            uint8_t flags = (uint8_t)key[4];
            out->fields[w++] = (hp_str_t){
                .ptr = key + KEY_FIELD_HEADER,
                .len = len,
                .has_escapes = (flags & KEY_ESCAPES) != 0,
                .is_json = (flags & KEY_JSON) != 0,
                .present = (flags & KEY_PRESENT) != 0,
            };
            key += KEY_FIELD_HEADER + len;
        }
    }
    for (size_t j = 0; j < agg->fn_count; j++) {
        const hp_agg_acc_t *acc = i < agg->group_count ? &agg->acc[i * agg->fn_count + j] : &no_rows;
        char *text = scratch + j * HP_AGG_VALUE_MAX;
        size_t len = format_acc(text, acc, &agg->fns[j]);
        out->fields[w++] = (hp_str_t){ .ptr = text, .len = len, .present = len > 0 };
    }
    out->field_count = w;
    out->row_index = i;
}
