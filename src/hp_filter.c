#include "hyperpipe/hp_filter.h"
#include "hyperpipe/hp_time.h"
#include "hp_number.h"
#include <ctype.h>

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
int64_t hp_resolve_column(const hp_header_t *header, const char *ref) {
    int64_t idx = hp_header_find_column(header, ref);
    if (idx >= 0) return idx;

    int64_t pos = 0;
    if (isdigit((unsigned char)ref[0]) && hp_parse_int_span(ref, strlen(ref), &pos)) {
        return pos;
    }
    return -1;
}

static const char *skip_space(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Comma-separated column names; spaces around a name are dropped, spaces
 * inside it are kept */
hp_error_t hp_projection_init(hp_projection_t *proj, const char *spec) {
    if (!proj) return HP_ERR_INVALID_ARG;
    memset(proj, 0, sizeof(*proj));
    if (!spec) return HP_OK;

    const char *p = spec;
    for (;;) {
        p = skip_space(p);
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t len = (size_t)(end - start);

        if (len > 0) {
            if (proj->count == HP_MAX_COLUMNS || len >= sizeof(proj->col_names[0])) {
                return HP_ERR_INVALID_ARG;
            }
            memcpy(proj->col_names[proj->count], start, len);
            proj->col_names[proj->count][len] = '\0';
            proj->col_indices[proj->count] = -1;
            proj->count++;
        }
        if (!*p) break;
        p++;
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
            dst->fields[dst->field_count].is_json = false;
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

/* A word such as "and" or "contains", followed by a space or the end */
static bool at_word(const char *p, const char *word, bool any_case) {
    size_t n = strlen(word);
    for (size_t i = 0; i < n; i++) {
        char c = any_case ? (char)tolower((unsigned char)p[i]) : p[i];
        if (c != word[i]) return false;
    }
    return p[n] == '\0' || isspace((unsigned char)p[n]);
}

static hp_error_t filter_error(hp_filter_t *filter, const char *clause, const char *at,
                               const char *why) {
    filter->error = why;
    filter->error_at = (size_t)(at - clause);
    return HP_ERR_FILTER;
}

static const struct {
    const char *text;
    hp_pred_op_t op;
    bool word;
} filter_ops[] = {
    { "==", HP_OP_EQ, false },   { "!=", HP_OP_NEQ, false },
    { "<=", HP_OP_LTE, false },  { ">=", HP_OP_GTE, false },
    { "<", HP_OP_LT, false },    { ">", HP_OP_GT, false },
    { "=", HP_OP_EQ, false },
    { "contains", HP_OP_CONTAINS, true },
    { "startswith", HP_OP_STARTSWITH, true },
    { "endswith", HP_OP_ENDSWITH, true },
};

/* One "column operator value" condition at *pp */
static hp_error_t parse_predicate(hp_filter_t *filter, const char *clause, const char **pp,
                                  hp_predicate_t *pred) {
    const char *p = *pp;

    const char *name = p;
    while (*p && !isspace((unsigned char)*p) && !strchr("=!<>", *p)) p++;
    size_t name_len = (size_t)(p - name);
    if (name_len == 0) return filter_error(filter, clause, name, "expected a column name");
    if (name_len >= sizeof(pred->col_name)) {
        return filter_error(filter, clause, name, "column name longer than 127 bytes");
    }
    memcpy(pred->col_name, name, name_len);
    pred->col_name[name_len] = '\0';

    p = skip_space(p);
    size_t k = 0, n_ops = sizeof(filter_ops) / sizeof(filter_ops[0]);
    while (k < n_ops && !(filter_ops[k].word ? at_word(p, filter_ops[k].text, false)
                                             : strncmp(p, filter_ops[k].text, strlen(filter_ops[k].text)) == 0)) {
        k++;
    }
    if (k == n_ops) {
        return filter_error(filter, clause, p, "expected an operator: ==, !=, <, <=, >, >=, "
                                               "contains, startswith or endswith");
    }
    pred->op = filter_ops[k].op;
    p = skip_space(p + strlen(filter_ops[k].text));

    const char *value = p;
    bool quoted = (*p == '"');
    if (quoted) {
        value = ++p;
        while (*p && *p != '"') p++;
        if (!*p) return filter_error(filter, clause, value - 1, "unterminated quote");
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        if (p == value) return filter_error(filter, clause, value, "expected a value");
    }
    size_t value_len = (size_t)(p - value);
    if (value_len >= sizeof(pred->str_val)) {
        return filter_error(filter, clause, value, "value longer than 255 bytes");
    }
    memcpy(pred->str_val, value, value_len);
    pred->str_val[value_len] = '\0';
    pred->str_val_len = value_len;
    if (quoted) p++;

    /* An unquoted literal that reads as a number compares numerically;
     * string operators always compare text */
    bool text_op = pred->op == HP_OP_CONTAINS || pred->op == HP_OP_STARTSWITH ||
                   pred->op == HP_OP_ENDSWITH;
    if (!quoted && !text_op && hp_time_parse_iso(pred->str_val, value_len, &pred->time_val)) {
        pred->is_time = true;
    } else if (!quoted && !text_op) {
        pred->is_int = hp_parse_int_span(pred->str_val, value_len, &pred->int_val);
        pred->is_positive_canonical_int =
            pred->is_int && pred->str_val[0] >= '1' && pred->str_val[0] <= '9';
        if (pred->is_int) {
            pred->is_num = true;
            pred->num_val = (double)pred->int_val;
        } else {
            pred->is_num = hp_parse_decimal_span(pred->str_val, value_len, &pred->num_val);
        }
    }
    pred->eq_by_bytes = pred->op == HP_OP_EQ && !pred->is_time &&
                        (!pred->is_num || pred->is_positive_canonical_int);

    *pp = p;
    return HP_OK;
}

/* Conditions joined by "and" (also "&&" or ",") form a group; "or" (also
 * "||") starts the next group, and a row passes when any group matches. */
hp_error_t hp_filter_init(hp_filter_t *filter, const char *where_clause) {
    if (!filter) return HP_ERR_INVALID_ARG;
    memset(filter, 0, sizeof(*filter));
    if (!where_clause) return HP_OK;

    const char *p = skip_space(where_clause);
    if (!*p) return HP_OK;

    uint16_t group = 0;
    for (;;) {
        if (filter->count == HP_MAX_PREDICATES) {
            return filter_error(filter, where_clause, p, "more than 16 conditions");
        }
        hp_predicate_t *pred = &filter->preds[filter->count];
        pred->col_index = -1;
        pred->or_group = group;
        hp_error_t err = parse_predicate(filter, where_clause, &p, pred);
        if (err != HP_OK) return err;
        filter->count++;

        p = skip_space(p);
        if (!*p) return HP_OK;
        if (*p == ',') {
            p++;
        } else if (p[0] == '&' && p[1] == '&') {
            p += 2;
        } else if (at_word(p, "and", true)) {
            p += 3;
        } else if (p[0] == '|' && p[1] == '|') {
            p += 2;
            group++;
        } else if (at_word(p, "or", true)) {
            p += 2;
            group++;
        } else {
            return filter_error(filter, where_clause, p, "expected 'and', 'or' or the end "
                                                         "of the filter; quote values with spaces");
        }
        p = skip_space(p);
    }
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

    /* A time literal only matches fields that are times */
    if (pred->is_time) {
        int64_t us;
        if (f->is_json || !hp_time_parse(ptr, len, &us)) return false;
        return hp_cmp_matches(pred->op, hp_sign_i64(us, pred->time_val));
    }

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
