#include "hyperpipe/hp_time.h"
#include <ctype.h>

#define US_PER_SEC  1000000LL
#define US_PER_DAY  (86400LL * US_PER_SEC)
#define MAX_UNIX_S  253402300799LL      /* 9999-12-31 23:59:59 */

/* Days since 1970-01-01 of a date in the proleptic Gregorian calendar,
 * and back (Howard Hinnant's days_from_civil and civil_from_days) */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static unsigned days_in_month(int64_t y, unsigned m) {
    static const unsigned days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

/* Exactly n digits at p */
static bool read_digits(const char *p, size_t n, unsigned *v) {
    unsigned x = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned d = (unsigned)(uint8_t)p[i] - '0';
        if (d > 9) return false;
        x = x * 10 + d;
    }
    *v = x;
    return true;
}

typedef struct hp_civil_s {
    unsigned y, mo, d, h, mi, s, us;
    int offset_s;          /* east of UTC */
} hp_civil_t;

static bool to_unix_us(const hp_civil_t *c, int64_t *us) {
    if (c->y > 9999 || c->mo < 1 || c->mo > 12 || c->d < 1 || c->d > days_in_month(c->y, c->mo) ||
        c->h > 23 || c->mi > 59 || c->s > 60) {
        return false;
    }
    int64_t secs = days_from_civil(c->y, c->mo, c->d) * 86400 +
                   (int64_t)c->h * 3600 + (int64_t)c->mi * 60 + c->s - c->offset_s;
    /* An offset may not carry the time out of years 0 to 9999 */
    if (secs < days_from_civil(0, 1, 1) * 86400 || secs > MAX_UNIX_S) return false;
    *us = secs * US_PER_SEC + c->us;
    return true;
}

/* "Z", "+02:00", "+0200" or "+02", which must end the text */
static bool read_offset(const char *p, size_t len, int *offset_s) {
    *offset_s = 0;
    if (len == 0) return true;
    if (len == 1 && (p[0] == 'Z' || p[0] == 'z')) return true;
    if (p[0] != '+' && p[0] != '-') return false;
    unsigned h = 0, m = 0;
    bool ok = (len == 3 && read_digits(p + 1, 2, &h)) ||
              (len == 5 && read_digits(p + 1, 2, &h) && read_digits(p + 3, 2, &m)) ||
              (len == 6 && p[3] == ':' && read_digits(p + 1, 2, &h) && read_digits(p + 4, 2, &m));
    if (!ok || h > 23 || m > 59) return false;
    *offset_s = (int)(h * 3600 + m * 60) * (p[0] == '-' ? -1 : 1);
    return true;
}

bool hp_time_parse_iso(const char *p, size_t len, int64_t *us) {
    hp_civil_t c = { 0 };
    if (len < 10 || p[4] != '-' || p[7] != '-' || !read_digits(p, 4, &c.y) ||
        !read_digits(p + 5, 2, &c.mo) || !read_digits(p + 8, 2, &c.d)) {
        return false;
    }
    size_t i = 10;
    if (i < len && (p[i] == 'T' || p[i] == 't' || p[i] == ' ')) {
        if (len < i + 6 || p[i + 3] != ':' || !read_digits(p + i + 1, 2, &c.h) ||
            !read_digits(p + i + 4, 2, &c.mi)) {
            return false;
        }
        i += 6;
        if (i < len && p[i] == ':') {
            if (len < i + 3 || !read_digits(p + i + 1, 2, &c.s)) return false;
            i += 3;
            if (i < len && (p[i] == '.' || p[i] == ',')) {
                size_t digits = 0;
                unsigned scale = 100000;
                for (i++; i < len && (unsigned)(uint8_t)p[i] - '0' <= 9; i++, digits++) {
                    c.us += (unsigned)(p[i] - '0') * scale;
                    scale /= 10;
                }
                if (digits == 0) return false;
            }
        }
    }
    return read_offset(p + i, len - i, &c.offset_s) && to_unix_us(&c, us);
}

/* "01/Jul/1995:00:00:01 -0400", the offset optional */
static bool parse_clf(const char *p, size_t len, int64_t *us) {
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    hp_civil_t c = { 0 };
    if (len < 20 || p[2] != '/' || p[6] != '/' || p[11] != ':' || p[14] != ':' || p[17] != ':' ||
        !read_digits(p, 2, &c.d) || !read_digits(p + 7, 4, &c.y) || !read_digits(p + 12, 2, &c.h) ||
        !read_digits(p + 15, 2, &c.mi) || !read_digits(p + 18, 2, &c.s)) {
        return false;
    }
    char name[3] = { (char)tolower((uint8_t)p[3]), (char)tolower((uint8_t)p[4]), (char)tolower((uint8_t)p[5]) };
    for (unsigned k = 0; k < 12 && !c.mo; k++) {
        if (memcmp(months + 3 * k, name, 3) == 0) c.mo = k + 1;
    }
    if (!c.mo) return false;
    size_t i = 20;
    if (i < len && p[i] == ' ') i++;
    return read_offset(p + i, len - i, &c.offset_s) && to_unix_us(&c, us);
}

/* Seconds, or milliseconds from 12 digits on */
static bool parse_unix(const char *p, size_t len, int64_t *us) {
    if (len == 0 || len > 14) return false;
    int64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned d = (unsigned)(uint8_t)p[i] - '0';
        if (d > 9) return false;
        v = v * 10 + d;
    }
    if (len >= 12) {
        if (v / 1000 > MAX_UNIX_S) return false;
        *us = v * 1000;
    } else {
        if (v > MAX_UNIX_S) return false;
        *us = v * US_PER_SEC;
    }
    return true;
}

bool hp_time_parse(const char *p, size_t len, int64_t *us) {
    if (len >= 10 && p[4] == '-') return hp_time_parse_iso(p, len, us);
    if (len >= 20 && p[2] == '/') return parse_clf(p, len, us);
    return parse_unix(p, len, us);
}

static char *put2(char *w, unsigned v) {
    w[0] = (char)('0' + v / 10);
    w[1] = (char)('0' + v % 10);
    return w + 2;
}

size_t hp_time_format(int64_t us, hp_time_unit_t unit, char *out) {
    int64_t days = us / US_PER_DAY, rem = us % US_PER_DAY;
    if (rem < 0) {
        days--;
        rem += US_PER_DAY;
    }
    int64_t y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    unsigned secs = (unsigned)(rem / US_PER_SEC);

    char *w = out;
    w = put2(w, (unsigned)(y / 100));
    w = put2(w, (unsigned)(y % 100));
    if (unit >= HP_TIME_MONTH) {
        *w++ = '-';
        w = put2(w, m);
    }
    if (unit >= HP_TIME_DAY) {
        *w++ = '-';
        w = put2(w, d);
    }
    if (unit >= HP_TIME_HOUR) {
        *w++ = ' ';
        w = put2(w, secs / 3600);
        *w++ = ':';
        w = put2(w, unit == HP_TIME_MINUTE ? secs / 60 % 60 : 0);
    }
    return (size_t)(w - out);
}

static const char *const unit_names[] = { "year", "month", "day", "hour", "minute" };

bool hp_time_unit_from_name(const char *name, size_t len, hp_time_unit_t *unit) {
    for (int u = 0; u < 5; u++) {
        if (strlen(unit_names[u]) == len && memcmp(unit_names[u], name, len) == 0) {
            *unit = (hp_time_unit_t)u;
            return true;
        }
    }
    return false;
}

const char *hp_time_unit_name(hp_time_unit_t unit) {
    return unit_names[unit];
}
