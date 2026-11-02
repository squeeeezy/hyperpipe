#include "hyperpipe/hp_filter.h"
#include <ctype.h>

/* Parses a signed decimal integer that spans the whole field. Values outside
 * int64 are rejected rather than wrapped, so they simply never match. */
static bool hp_parse_int_span(const char *ptr, size_t len, int64_t *out) {
    size_t k = 0;
    bool neg = false;
    if (len > 0 && (ptr[0] == '-' || ptr[0] == '+')) {
        neg = (ptr[0] == '-');
        k = 1;
    }
    const size_t digits = len - k;
    if (digits == 0 || digits > 19) return false;

    uint64_t v = 0;
    for (; k < len; k++) {
        uint8_t d = (uint8_t)(ptr[k] - '0');
        if (d > 9) return false;
        v = v * 10 + (uint64_t)d;
    }
    /* Up to 18 digits always fit; only a 19-digit value can exceed int64.
     * Below 10^19 the uint64 accumulator itself never wraps. */
    const uint64_t limit = neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    if (digits == 19 && v > limit) return false;
    *out = neg ? (int64_t)(0ull - v) : (int64_t)v;
    return true;
}

/* Parses a plain decimal number that spans the whole field: optional sign,
 * digits with an optional fraction, optional exponent. Unlike bare strtod it
 * rejects numeric prefixes ("200abc"), whitespace, hex, inf and nan. */
static bool hp_parse_decimal_span(const char *ptr, size_t len, double *out) {
    char buf[64];
    if (len == 0 || len >= sizeof(buf)) return false;

    size_t k = 0;
    size_t digits = 0;
    if (ptr[k] == '-' || ptr[k] == '+') k++;
    while (k < len && isdigit((unsigned char)ptr[k])) { k++; digits++; }
    if (k < len && ptr[k] == '.') {
        k++;
        while (k < len && isdigit((unsigned char)ptr[k])) { k++; digits++; }
    }
    if (digits == 0) return false;

    if (k < len && (ptr[k] == 'e' || ptr[k] == 'E')) {
        k++;
        if (k < len && (ptr[k] == '-' || ptr[k] == '+')) k++;
        size_t exp_digits = 0;
        while (k < len && isdigit((unsigned char)ptr[k])) { k++; exp_digits++; }
        if (exp_digits == 0) return false;
    }
    if (k != len) return false;

    memcpy(buf, ptr, len);
    buf[len] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

static int hp_sign_i64(int64_t a, int64_t b) { return (a > b) - (a < b); }
static int hp_sign_f64(double a, double b) { return (a > b) - (a < b); }

/* Applies a comparison operator to the sign of (field - literal). */
static bool hp_cmp_matches(hp_pred_op_t op, int cmp) {
    switch (op) {
        case HP_OP_EQ:  return cmp == 0;
        case HP_OP_NEQ: return cmp != 0;
        case HP_OP_LT:  return cmp < 0;
        case HP_OP_LTE: return cmp <= 0;
        case HP_OP_GT:  return cmp > 0;
        case HP_OP_GTE: return cmp >= 0;
        default:        return false;
    }
}

/* Resolves a column reference against the header. A matching name always
 * wins, so a column literally named "2019" is found by name; otherwise a
 * non-negative integer selects the column by position. */
static int64_t hp_resolve_column(const hp_header_t *header, const char *ref) {
    int64_t idx = hp_header_find_column(header, ref);
    if (idx >= 0) return idx;

    int64_t pos = 0;
    if (isdigit((unsigned char)ref[0]) && hp_parse_int_span(ref, strlen(ref), &pos)) {
        return pos;
    }
    return -1;
}

hp_error_t hp_projection_init(hp_projection_t *proj, const char *spec) {
    if (!proj) return HP_ERR_INVALID_ARG;
    memset(proj, 0, sizeof(*proj));
    if (!spec || *spec == '\0') return HP_OK;

    const char *p = spec;
    while (*p && proj->count < HP_MAX_COLUMNS) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
        size_t len = p - start;
        if (len >= sizeof(proj->col_names[0])) len = sizeof(proj->col_names[0]) - 1;

        memcpy(proj->col_names[proj->count], start, len);
        proj->col_names[proj->count][len] = '\0';
        proj->col_indices[proj->count] = -1;
        proj->count++;
    }

    proj->active = (proj->count > 0);
    return HP_OK;
}

hp_error_t hp_projection_bind(hp_projection_t *proj, const hp_header_t *header) {
    if (!proj || !proj->active) return HP_OK;
    if (!header) return HP_ERR_INVALID_ARG;

    for (size_t i = 0; i < proj->count; i++) {
        proj->col_indices[i] = hp_resolve_column(header, proj->col_names[i]);
        if (proj->col_indices[i] < 0) return HP_ERR_COLUMN_NOT_FOUND;
    }
    return HP_OK;
}

hp_error_t hp_project_row(const hp_projection_t *proj, const hp_row_t *src, hp_row_t *dst) {
    if (!proj || !proj->active) {
        if (dst != src) *dst = *src;
        return HP_OK;
    }
    if (!src || !dst) return HP_ERR_INVALID_ARG;

    dst->field_count = 0;
    dst->row_index = src->row_index;

    for (size_t i = 0; i < proj->count; i++) {
        int64_t col_idx = proj->col_indices[i];
        if (col_idx >= 0 && (size_t)col_idx < src->field_count) {
            dst->fields[dst->field_count++] = src->fields[col_idx];
        } else {
            dst->fields[dst->field_count].ptr = "";
            dst->fields[dst->field_count].len = 0;
            dst->fields[dst->field_count].has_escapes = false;
            dst->fields[dst->field_count].present = false;
            dst->field_count++;
        }
    }
    return HP_OK;
}

hp_error_t hp_project_header(const hp_projection_t *proj, const hp_header_t *src, hp_header_t *dst) {
    if (!proj || !proj->active) return HP_OK;
    if (!dst) return HP_ERR_INVALID_ARG;

    dst->count = 0;
    for (size_t i = 0; i < proj->count; i++) {
        int64_t col_idx = proj->col_indices[i];
        if (src && col_idx >= 0 && (size_t)col_idx < src->count) {
            dst->names[dst->count++] = strdup(src->names[col_idx]);
        } else {
            dst->names[dst->count++] = strdup(proj->col_names[i]);
        }
    }
    return HP_OK;
}

hp_error_t hp_filter_init(hp_filter_t *filter, const char *where_clause) {
    if (!filter) return HP_ERR_INVALID_ARG;
    memset(filter, 0, sizeof(*filter));
    if (!where_clause || *where_clause == '\0') return HP_OK;

    const char *p = where_clause;
    uint16_t cur_group = 0;
    while (*p && filter->count < HP_MAX_PREDICATES) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *col_start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '!' && *p != '<' && *p != '>') p++;
        size_t col_len = p - col_start;
        if (col_len == 0) break;

        hp_predicate_t *pred = &filter->preds[filter->count];
        if (col_len >= sizeof(pred->col_name)) col_len = sizeof(pred->col_name) - 1;
        memcpy(pred->col_name, col_start, col_len);
        pred->col_name[col_len] = '\0';
        pred->col_index = -1;
        pred->or_group = cur_group;

        while (*p && isspace((unsigned char)*p)) p++;

        if (strncmp(p, "==", 2) == 0) {
            pred->op = HP_OP_EQ;
            p += 2;
        } else if (strncmp(p, "!=", 2) == 0) {
            pred->op = HP_OP_NEQ;
            p += 2;
        } else if (strncmp(p, "<=", 2) == 0) {
            pred->op = HP_OP_LTE;
            p += 2;
        } else if (strncmp(p, ">=", 2) == 0) {
            pred->op = HP_OP_GTE;
            p += 2;
        } else if (strncmp(p, "contains", 8) == 0 && (p[8] == '\0' || isspace((unsigned char)p[8]))) {
            pred->op = HP_OP_CONTAINS;
            p += 8;
        } else if (strncmp(p, "startswith", 10) == 0 && (p[10] == '\0' || isspace((unsigned char)p[10]))) {
            pred->op = HP_OP_STARTSWITH;
            p += 10;
        } else if (strncmp(p, "endswith", 8) == 0 && (p[8] == '\0' || isspace((unsigned char)p[8]))) {
            pred->op = HP_OP_ENDSWITH;
            p += 8;
        } else if (*p == '<') {
            pred->op = HP_OP_LT;
            p += 1;
        } else if (*p == '>') {
            pred->op = HP_OP_GT;
            p += 1;
        } else if (*p == '=') {
            pred->op = HP_OP_EQ;
            p += 1;
        } else {
            return HP_ERR_INVALID_ARG;
        }

        while (*p && isspace((unsigned char)*p)) p++;

        const char *val_start = p;
        if (*p == '"') {
            p++;
            val_start = p;
            while (*p && *p != '"') p++;
            size_t val_len = p - val_start;
            if (val_len >= sizeof(pred->str_val)) val_len = sizeof(pred->str_val) - 1;
            memcpy(pred->str_val, val_start, val_len);
            pred->str_val[val_len] = '\0';
            pred->str_val_len = val_len;
            if (*p == '"') p++;
            pred->is_num = false;
            pred->is_int = false;
        } else if (pred->op == HP_OP_CONTAINS || pred->op == HP_OP_STARTSWITH || pred->op == HP_OP_ENDSWITH) {
            /* String operators always compare text, even against numeric-looking literals */
            while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
            size_t val_len = p - val_start;
            if (val_len >= sizeof(pred->str_val)) val_len = sizeof(pred->str_val) - 1;
            memcpy(pred->str_val, val_start, val_len);
            pred->str_val[val_len] = '\0';
            pred->str_val_len = val_len;
            pred->is_num = false;
            pred->is_int = false;
        } else {
            while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
            size_t val_len = p - val_start;
            if (val_len >= sizeof(pred->str_val)) val_len = sizeof(pred->str_val) - 1;
            memcpy(pred->str_val, val_start, val_len);
            pred->str_val[val_len] = '\0';
            pred->str_val_len = val_len;

            /* An unquoted literal that reads as a number compares numerically */
            pred->is_int = hp_parse_int_span(pred->str_val, val_len, &pred->int_val);
            pred->is_positive_canonical_int =
                pred->is_int && pred->str_val[0] >= '1' && pred->str_val[0] <= '9';
            if (pred->is_int) {
                pred->is_num = true;
                pred->num_val = (double)pred->int_val;
            } else {
                pred->is_num = hp_parse_decimal_span(pred->str_val, val_len, &pred->num_val);
            }
        }

        filter->count++;
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        /* Accept explicit conjunction keywords: 'and' / 'AND' / '&&' keep the
         * current OR-group (AND-semantics inside a group); 'or' / '||' starts
         * a new one. A row passes when AT LEAST ONE group fully matches. */
        if (p[0] == '&' && p[1] == '&') {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
        } else if ((p[0] | 32) == 'a' && (p[1] | 32) == 'n' && (p[2] | 32) == 'd' &&
                   (p[3] == '\0' || isspace((unsigned char)p[3]))) {
            p += 3;
            while (*p && isspace((unsigned char)*p)) p++;
        } else if (p[0] == '|' && p[1] == '|') {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
            cur_group++;
        } else if ((p[0] | 32) == 'o' && (p[1] | 32) == 'r' &&
                   (p[2] == '\0' || isspace((unsigned char)p[2]))) {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
            cur_group++;
        }
    }

    return HP_OK;
}

hp_error_t hp_filter_bind(hp_filter_t *filter, const hp_header_t *header) {
    if (!filter || filter->count == 0) return HP_OK;
    if (!header) return HP_ERR_INVALID_ARG;

    for (size_t i = 0; i < filter->count; i++) {
        hp_predicate_t *pred = &filter->preds[i];
        pred->col_index = hp_resolve_column(header, pred->col_name);
        if (pred->col_index < 0) return HP_ERR_COLUMN_NOT_FOUND;

        /* Apply declared types (--schema) by resolved column name */
        if (pred->col_index >= 0 && (size_t)pred->col_index < header->count) {
            const char *resolved = header->names[pred->col_index];
            if (resolved) {
                for (size_t h = 0; h < filter->hint_count; h++) {
                    if (strcmp(filter->hints[h].name, resolved) == 0) {
                        pred->hint_int = filter->hints[h].is_int;
                        break;
                    }
                }
            }
        }
    }
    return HP_OK;
}

hp_error_t hp_filter_load_hints(hp_filter_t *filter, const char *path) {
    if (!filter || !path) return HP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "r");
    if (!f) return HP_ERR_IO;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *comma = strchr(p, ',');
        if (!comma) {
            fclose(f);
            return HP_ERR_INVALID_ARG;
        }
        *comma = '\0';
        const char *type = comma + 1;

        /* Trim the name on both sides; the type is a single token */
        size_t nlen = strlen(p);
        while (nlen > 0 && (p[nlen - 1] == ' ' || p[nlen - 1] == '\t')) nlen--;
        while (*type == ' ' || *type == '\t') type++;
        size_t tlen = strlen(type);
        while (tlen > 0 && (type[tlen - 1] == '\n' || type[tlen - 1] == '\r' ||
                            type[tlen - 1] == ' ' || type[tlen - 1] == '\t')) {
            tlen--;
        }

        bool is_int;
        if (tlen == 3 && memcmp(type, "int", 3) == 0) {
            is_int = true;
        } else if (tlen == 6 && memcmp(type, "string", 6) == 0) {
            is_int = false;
        } else {
            fclose(f);
            return HP_ERR_INVALID_ARG;
        }

        if (nlen == 0 || nlen >= sizeof(filter->hints[0].name)) {
            fclose(f);
            return HP_ERR_INVALID_ARG;
        }
        if (filter->hint_count >= HP_MAX_HINTS) {
            fclose(f);
            return HP_ERR_BUFFER_OVERFLOW;
        }

        hp_schema_hint_t *h = &filter->hints[filter->hint_count++];
        memcpy(h->name, p, nlen);
        h->name[nlen] = '\0';
        h->is_int = is_int;
    }

    fclose(f);
    return HP_OK;
}

static bool hp_mem_contains(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (needle_len > hay_len) return false;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static bool hp_pred_matches(const hp_predicate_t *pred, const hp_row_t *row) {
    if (pred->col_index < 0 || (size_t)pred->col_index >= row->field_count) {
        return false;
    }

    const hp_str_t *f = &row->fields[pred->col_index];
    const char *ptr = f->ptr;
    size_t len = f->len;

    if (pred->is_num) {
        /* Same bytes as the literal: equal without parsing */
        if ((pred->op == HP_OP_EQ || pred->op == HP_OP_NEQ) &&
            len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) {
            return pred->op == HP_OP_EQ;
        }

        /* A numeric literal only matches numeric fields. Integers compare
         * exactly in int64; anything else ("5000.0", "5e3") in double. */
        int64_t i_val = 0;
        if (pred->is_int && hp_parse_int_span(ptr, len, &i_val)) {
            return hp_cmp_matches(pred->op, hp_sign_i64(i_val, pred->int_val));
        }
        /* A column declared int (--schema) never needs the slower decimal
         * parse: non-integer cells are a contract violation and never match. */
        if (pred->hint_int) {
            if (!hp_parse_int_span(ptr, len, &i_val)) return false;
            return hp_cmp_matches(pred->op, hp_sign_f64((double)i_val, pred->num_val));
        }
        double d_val = 0.0;
        if (!hp_parse_decimal_span(ptr, len, &d_val)) return false;
        return hp_cmp_matches(pred->op, hp_sign_f64(d_val, pred->num_val));
    }

    switch (pred->op) {
        case HP_OP_CONTAINS:
            return hp_mem_contains(ptr, len, pred->str_val, pred->str_val_len);
        case HP_OP_STARTSWITH:
            return len >= pred->str_val_len &&
                   memcmp(ptr, pred->str_val, pred->str_val_len) == 0;
        case HP_OP_ENDSWITH:
            return len >= pred->str_val_len &&
                   memcmp(ptr + len - pred->str_val_len, pred->str_val, pred->str_val_len) == 0;
        default: {
            size_t min_len = len < pred->str_val_len ? len : pred->str_val_len;
            int cmp = memcmp(ptr, pred->str_val, min_len);
            if (cmp == 0) cmp = (len > pred->str_val_len) - (len < pred->str_val_len);
            return hp_cmp_matches(pred->op, cmp);
        }
    }
}

HP_HOT bool hp_filter_eval(const hp_filter_t *filter, const hp_row_t *row) {

    /* Predicates are stored grouped: within a group all must match (AND),
     * across groups any fully-matching group passes (OR). */
    size_t i = 0;
    while (i < filter->count) {
        uint16_t group = filter->preds[i].or_group;
        bool group_ok = true;
        for (; i < filter->count && filter->preds[i].or_group == group; i++) {
            if (group_ok && !hp_pred_matches(&filter->preds[i], row)) group_ok = false;
        }
        if (group_ok) return true;
    }
    return false;
}
