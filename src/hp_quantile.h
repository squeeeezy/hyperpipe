/* Quantiles of one group's values. Up to HP_QUANTILE_EXACT values are kept
 * and the quantile is exact, interpolated linearly between the two nearest
 * values as numpy and pandas do. Past that the values go into a sketch of
 * logarithmic buckets (DDSketch) whose answers are within 1% of a value
 * at the requested rank. Both forms depend only on the values, not on their
 * order, so merged parts give the single-threaded answer. Internal to src/. */

#ifndef HYPERPIPE_SRC_HP_QUANTILE_H
#define HYPERPIPE_SRC_HP_QUANTILE_H

#include "hyperpipe/hp_common.h"

#define HP_QUANTILE_EXACT 1024

typedef struct hp_quantile_s hp_quantile_t;

/* *q may be NULL: the state is created on the first value */
hp_error_t hp_quantile_add(hp_quantile_t **q, double v);
hp_error_t hp_quantile_merge(hp_quantile_t **dst, const hp_quantile_t *src);

/* The p-quantile, p in [0, 1]; false without values. Reorders the kept
 * values, which changes nothing else. */
bool hp_quantile_get(hp_quantile_t *q, double p, double *out);
void hp_quantile_free(hp_quantile_t *q);

#endif /* HYPERPIPE_SRC_HP_QUANTILE_H */
