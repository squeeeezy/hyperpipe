/* Timestamps: every recognized form, offsets, the calendar's edges, time
 * buckets in --group-by and time literals in filters. */

#include "hyperpipe/hp_time.h"
#include "pipe.h"

static int64_t at(int y, int mo, int d, int h, int mi, int s) {
    /* 1970-01-01 plus whole days, computed the slow way */
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int year = 1970; year < y; year++) {
        days += 365 + ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
    }
    for (int m = 1; m < mo; m++) {
        days += mdays[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    }
    days += d - 1;
    return ((days * 24 + h) * 60 + mi) * 60 * 1000000LL + s * 1000000LL;
}

static bool parses_to(const char *text, int64_t want) {
    int64_t us = -1;
    bool ok = hp_time_parse(text, strlen(text), &us) && us == want;
    if (!ok) fprintf(stderr, "# '%s' -> %lld, want %lld\n", text, (long long)us, (long long)want);
    return ok;
}

static bool rejects(const char *text) {
    int64_t us;
    return !hp_time_parse(text, strlen(text), &us);
}

static void test_forms(void) {
    CHECK(parses_to("2024-03-05", at(2024, 3, 5, 0, 0, 0)));
    CHECK(parses_to("2024-03-05T12:34", at(2024, 3, 5, 12, 34, 0)));
    CHECK(parses_to("2024-03-05 12:34:56", at(2024, 3, 5, 12, 34, 56)));
    CHECK(parses_to("2024-03-05T12:34:56.25Z", at(2024, 3, 5, 12, 34, 56) + 250000));
    CHECK(parses_to("2024-03-05T12:34:56,000001", at(2024, 3, 5, 12, 34, 56) + 1));
    CHECK(parses_to("2024-03-05T12:00:00+02:00", at(2024, 3, 5, 10, 0, 0)));
    CHECK(parses_to("2024-03-05T12:00:00-0530", at(2024, 3, 5, 17, 30, 0)));
    CHECK(parses_to("2024-03-05T12:00+01", at(2024, 3, 5, 11, 0, 0)));
    CHECK(parses_to("01/Jul/1995:00:00:01 -0400", at(1995, 7, 1, 4, 0, 1)));
    CHECK(parses_to("31/dec/1999:23:59:59", at(1999, 12, 31, 23, 59, 59)));
    CHECK(parses_to("0", 0));
    CHECK(parses_to("1709679000", at(2024, 3, 5, 22, 50, 0)));
    CHECK(parses_to("1709679000123", at(2024, 3, 5, 22, 50, 0) + 123000));
}

static void test_calendar(void) {
    CHECK(parses_to("2000-02-29", at(2000, 2, 29, 0, 0, 0)));
    CHECK(parses_to("1969-12-31T23:59:59", -1000000));
    CHECK(parses_to("1600-03-01", at(1970, 1, 1, 0, 0, 0) - 135080LL * 86400 * 1000000));
    CHECK(rejects("1900-02-29"));
    CHECK(rejects("2023-02-29"));
    CHECK(rejects("2024-04-31"));
    CHECK(rejects("2024-13-01"));
    CHECK(rejects("2024-03-05T24:00"));
    CHECK(rejects("2024-03-05T12:60"));
    CHECK(rejects("0000-01-01T00:00+01:00"));
    CHECK(rejects("2024-03-05T12:00Zx"));
    CHECK(rejects("2024-03-05T12:00:00."));
    CHECK(rejects("2024-3-5"));
    CHECK(rejects("01/Jux/1995:00:00:01"));
    CHECK(rejects("12.5"));
    CHECK(rejects(""));

    char out[HP_TIME_TEXT_MAX];
    int64_t t = at(1995, 7, 1, 4, 5, 6);
    CHECK(hp_time_format(t, HP_TIME_YEAR, out) == 4 && memcmp(out, "1995", 4) == 0);
    CHECK(hp_time_format(t, HP_TIME_MONTH, out) == 7 && memcmp(out, "1995-07", 7) == 0);
    CHECK(hp_time_format(t, HP_TIME_DAY, out) == 10 && memcmp(out, "1995-07-01", 10) == 0);
    CHECK(hp_time_format(t, HP_TIME_HOUR, out) == 16 && memcmp(out, "1995-07-01 04:00", 16) == 0);
    CHECK(hp_time_format(t, HP_TIME_MINUTE, out) == 16 && memcmp(out, "1995-07-01 04:05", 16) == 0);
    CHECK(hp_time_format(-1, HP_TIME_MINUTE, out) == 16 && memcmp(out, "1969-12-31 23:59", 16) == 0);
}

static const char *const events =
    "t,v\n"
    "2024-03-05T23:30:00+02:00,1\n"
    "2024-03-05 21:10,2\n"
    "1709679000,3\n"
    "nope,4\n"
    "05/Mar/2024:22:00:00 +0000,5\n";

static void test_buckets(void) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV,
        .group_by_spec = "hour(t)", .agg_spec = "count, sum(v)",
    };
    pipe_result_t r = pipe_run_str(&cfg, events);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "hour(t),count,sum(v)\n"
                        "2024-03-05 21:00,2,3\n"
                        "2024-03-05 22:00,2,8\n"
                        ",1,4\n") == 0);
    pipe_free(&r);

    cfg.group_by_spec = "day( t ), month(t)";
    cfg.agg_spec = "count";
    r = pipe_run_str(&cfg, events);
    CHECK(r.err == HP_OK && strcmp(r.out, "day(t),month(t),count\n2024-03-05,2024-03,4\n,,1\n") == 0);
    pipe_free(&r);

    /* Buckets are sortable text */
    cfg.group_by_spec = "minute(t)";
    cfg.sort_spec = "-minute(t)";
    r = pipe_run_str(&cfg, events);
    CHECK(r.err == HP_OK);
    CHECK(strcmp(r.out, "minute(t),count\n2024-03-05 22:50,1\n2024-03-05 22:00,1\n"
                        "2024-03-05 21:30,1\n2024-03-05 21:10,1\n,1\n") == 0);
    pipe_free(&r);
}

static uint64_t count_where(const char *where) {
    hp_transcoder_config_t cfg = {
        .in_format = HP_FORMAT_CSV, .out_format = HP_FORMAT_CSV, .where_spec = where,
    };
    pipe_result_t r = pipe_run_str(&cfg, events);
    CHECK(r.err == HP_OK);
    uint64_t n = r.stats.rows_emitted;
    pipe_free(&r);
    return n;
}

static void test_filters(void) {
    CHECK(count_where("t >= 2024-03-05T21:20") == 3);
    CHECK(count_where("t < 2024-03-05T21:20Z") == 1);
    CHECK(count_where("t == 2024-03-05T22:00") == 1);
    CHECK(count_where("t != 2024-03-05T22:00") == 3);   /* "nope" is not a time */
    CHECK(count_where("t > 2024-03-05 and t < 2024-03-06") == 4);
    /* Quoted, the literal is text again */
    CHECK(count_where("t == \"nope\"") == 1);
    /* A year alone is a number, not a time */
    CHECK(count_where("v < 2024") == 5);
}

int main(void) {
    TAP_RUN(test_forms);
    TAP_RUN(test_calendar);
    TAP_RUN(test_buckets);
    TAP_RUN(test_filters);
    return tap_done();
}
