#ifndef HYPERPIPE_HP_TIME_H
#define HYPERPIPE_HP_TIME_H

#include "hp_common.h"

/* Timestamps, as microseconds since 1970-01-01 00:00 UTC.
 *
 * Recognized in any field: ISO 8601 ("2024-03-05", "2024-03-05T12:34",
 * "2024-03-05 12:34:56.789+02:00"), the common log format of web servers
 * ("01/Jul/1995:00:00:01 -0400") and Unix time in seconds or milliseconds.
 * An offset is applied, so every time is in UTC; a time without one is
 * taken as UTC. Years run from 0 to 9999. */

typedef enum hp_time_unit_e {
    HP_TIME_YEAR,          /* "1995" */
    HP_TIME_MONTH,         /* "1995-07" */
    HP_TIME_DAY,           /* "1995-07-01" */
    HP_TIME_HOUR,          /* "1995-07-01 04:00" */
    HP_TIME_MINUTE         /* "1995-07-01 04:01" */
} hp_time_unit_t;

/* Longest text hp_time_format() writes */
#define HP_TIME_TEXT_MAX 16

bool hp_time_parse(const char *p, size_t len, int64_t *us);

/* ISO 8601 only: what filter literals may be, so that 2024 stays a number */
bool hp_time_parse_iso(const char *p, size_t len, int64_t *us);

/* The start of the unit that holds the time, as sortable text */
size_t hp_time_format(int64_t us, hp_time_unit_t unit, char *out);

/* "year", "month", "day", "hour" or "minute" */
bool hp_time_unit_from_name(const char *name, size_t len, hp_time_unit_t *unit);
const char *hp_time_unit_name(hp_time_unit_t unit);

#endif /* HYPERPIPE_HP_TIME_H */
