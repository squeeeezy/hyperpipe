#include "hp_quantile.h"
#include <math.h>

/* Bucket i holds the values in (gamma^(i-1), gamma^i], gamma = 1.01 / 0.99:
 * the bucket's middle, 2 gamma^i / (gamma + 1), is within 1% of each. */
#define ALPHA     0.01
#define MAX_BINS  4096

typedef struct bins_s {
    int32_t lo;            /* the index of counts[0] */
    uint32_t cap;
    int32_t min, max;      /* the lowest and highest index with a count */
    uint64_t *counts;
} bins_t;

struct hp_quantile_s {
    uint64_t count;
    bool sketch;
    double *values;        /* the values themselves, until there are too many */
    uint32_t nvalues, cap;
    uint64_t zeros;
    bins_t pos, neg;       /* positive values, and negative ones by magnitude */
};

#define GAMMA     ((1 + ALPHA) / (1 - ALPHA))
#define LN_GAMMA  0.020000666706669435  /* log(GAMMA) */

static int32_t bin_of(double magnitude) {
    return (int32_t)ceil(log(magnitude) / LN_GAMMA);
}

static double bin_value(int32_t i) {
    return 2 * exp(i * LN_GAMMA) / (GAMMA + 1);
}

/* Adds c to bin i. A store keeps MAX_BINS bins up to its highest; lower
 * ones fold into the lowest kept. The cut only rises, so the result depends
 * on the values alone, not on the order they came in. */
static hp_error_t bins_add(bins_t *b, int32_t i, uint64_t c) {
    if (!b->counts) {
        if (!(b->counts = (uint64_t *)calloc(64, sizeof(uint64_t)))) return HP_ERR_NOMEM;
        b->cap = 64;
        b->lo = i - 32;
        b->min = b->max = i;
        b->counts[i - b->lo] = c;
        return HP_OK;
    }
    const int32_t max = i > b->max ? i : b->max;
    const int32_t floor = max - (MAX_BINS - 1);

    /* Bins below the cut leave the array before it moves */
    uint64_t folded = 0;
    int32_t old_min = b->min;
    if (old_min < floor) {
        int32_t end = b->max < floor ? b->max + 1 : floor;
        for (int32_t k = old_min; k < end; k++) {
            folded += b->counts[k - b->lo];
            b->counts[k - b->lo] = 0;
        }
        old_min = floor;
    }
    if (i < floor) i = floor;
    const int32_t min = i < old_min ? i : old_min;

    if (min < b->lo || max >= b->lo + (int32_t)b->cap) {
        int32_t span = max - min + 1, pad = span / 2 + 16;
        uint32_t cap = (uint32_t)(span + 2 * pad);
        int32_t lo = min - pad;
        uint64_t *counts = (uint64_t *)calloc(cap, sizeof(uint64_t));
        if (!counts) return HP_ERR_NOMEM;
        for (int32_t k = old_min; k <= b->max; k++) counts[k - lo] = b->counts[k - b->lo];
        free(b->counts);
        b->counts = counts;
        b->cap = cap;
        b->lo = lo;
    }
    if (folded) b->counts[floor - b->lo] += folded;
    b->counts[i - b->lo] += c;
    b->min = min;
    b->max = max;
    return HP_OK;
}

static hp_error_t sketch_add(hp_quantile_t *q, double v, uint64_t c) {
    if (v > 0) return bins_add(&q->pos, bin_of(v), c);
    if (v < 0) return bins_add(&q->neg, bin_of(-v), c);
    q->zeros += c;
    return HP_OK;
}

/* Moves the kept values into the sketch */
static hp_error_t to_sketch(hp_quantile_t *q) {
    hp_error_t err = HP_OK;
    for (uint32_t k = 0; err == HP_OK && k < q->nvalues; k++) err = sketch_add(q, q->values[k], 1);
    free(q->values);
    q->values = NULL;
    q->nvalues = q->cap = 0;
    q->sketch = true;
    return err;
}

static hp_error_t keep_value(hp_quantile_t *q, double v) {
    if (q->nvalues == q->cap) {
        uint32_t cap = q->cap ? q->cap * 2 : 8;
        double *values = (double *)realloc(q->values, cap * sizeof(double));
        if (!values) return HP_ERR_NOMEM;
        q->values = values;
        q->cap = cap;
    }
    q->values[q->nvalues++] = v;
    return HP_OK;
}

hp_error_t hp_quantile_add(hp_quantile_t **qp, double v) {
    if (!isfinite(v)) return HP_OK;
    if (!*qp && !(*qp = (hp_quantile_t *)calloc(1, sizeof(hp_quantile_t)))) return HP_ERR_NOMEM;
    hp_quantile_t *q = *qp;
    q->count++;
    if (q->sketch) return sketch_add(q, v, 1);
    if (q->nvalues < HP_QUANTILE_EXACT) return keep_value(q, v);
    hp_error_t err = to_sketch(q);
    return err != HP_OK ? err : sketch_add(q, v, 1);
}

hp_error_t hp_quantile_merge(hp_quantile_t **dstp, const hp_quantile_t *src) {
    if (!src || src->count == 0) return HP_OK;
    if (!*dstp && !(*dstp = (hp_quantile_t *)calloc(1, sizeof(hp_quantile_t)))) return HP_ERR_NOMEM;
    hp_quantile_t *dst = *dstp;
    hp_error_t err = HP_OK;

    if (!dst->sketch && !src->sketch && dst->nvalues + src->nvalues <= HP_QUANTILE_EXACT) {
        for (uint32_t k = 0; err == HP_OK && k < src->nvalues; k++) err = keep_value(dst, src->values[k]);
        dst->count += src->count;
        return err;
    }
    if (!dst->sketch && (err = to_sketch(dst)) != HP_OK) return err;
    for (uint32_t k = 0; err == HP_OK && k < src->nvalues; k++) err = sketch_add(dst, src->values[k], 1);
    for (int32_t i = src->pos.min; err == HP_OK && src->pos.counts && i <= src->pos.max; i++) {
        uint64_t c = src->pos.counts[i - src->pos.lo];
        if (c) err = bins_add(&dst->pos, i, c);
    }
    for (int32_t i = src->neg.min; err == HP_OK && src->neg.counts && i <= src->neg.max; i++) {
        uint64_t c = src->neg.counts[i - src->neg.lo];
        if (c) err = bins_add(&dst->neg, i, c);
    }
    dst->zeros += src->zeros;
    dst->count += src->count;
    return err;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

bool hp_quantile_get(hp_quantile_t *q, double p, double *out) {
    if (!q || q->count == 0) return false;
    double rank = p * (double)(q->count - 1);

    if (!q->sketch) {
        qsort(q->values, q->nvalues, sizeof(double), cmp_double);
        size_t lo = (size_t)rank;
        double t = rank - (double)lo;
        if (lo + 1 >= q->nvalues) {
            *out = q->values[lo];
            return true;
        }
        /* numpy's lerp, from the nearer end, so the results match it and
         * pandas to the last bit: the median of 0.1 and 1.5 is 0.8 */
        double a = q->values[lo], b = q->values[lo + 1], d = b - a;
        *out = t >= 0.5 ? b - d * (1 - t) : a + d * t;
        return true;
    }

    /* The bucket holding the value of that rank, from the most negative up */
    uint64_t want = (uint64_t)rank, seen = 0;
    for (int32_t i = q->neg.max; q->neg.counts && i >= q->neg.min; i--) {
        seen += q->neg.counts[i - q->neg.lo];
        if (seen > want) {
            *out = -bin_value(i);
            return true;
        }
    }
    seen += q->zeros;
    if (seen > want) {
        *out = 0;
        return true;
    }
    for (int32_t i = q->pos.min; q->pos.counts && i <= q->pos.max; i++) {
        seen += q->pos.counts[i - q->pos.lo];
        if (seen > want) {
            *out = bin_value(i);
            return true;
        }
    }
    *out = bin_value(q->pos.counts ? q->pos.max : 0);
    return true;
}

void hp_quantile_free(hp_quantile_t *q) {
    if (!q) return;
    free(q->values);
    free(q->pos.counts);
    free(q->neg.counts);
    free(q);
}
