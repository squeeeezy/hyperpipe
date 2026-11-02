#include "hyperpipe/hp_filter.h"
#include <ctype.h>

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
        char *endptr = NULL;
        long idx = strtol(proj->col_names[i], &endptr, 10);
        if (endptr != proj->col_names[i] && *endptr == '\0') {
            proj->col_indices[i] = idx;
        } else {
            proj->col_indices[i] = hp_header_find_column(header, proj->col_names[i]);
            if (proj->col_indices[i] < 0) {
                return HP_ERR_COLUMN_NOT_FOUND;
            }
        }
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

            bool is_int = false;
            if (val_len > 0) {
                size_t k = 0;
                if (pred->str_val[0] == '-' || pred->str_val[0] == '+') k++;
                if (k < val_len) {
                    is_int = true;
                    for (; k < val_len; k++) {
                        if (!isdigit((unsigned char)pred->str_val[k])) {
                            is_int = false;
                            break;
                        }
                    }
                }
            }

            if (is_int) {
                pred->is_int = true;
                pred->is_num = true;
                char *endptr = NULL;
                pred->int_val = strtoll(pred->str_val, &endptr, 10);
                pred->num_val = (double)pred->int_val;
            } else {
                pred->is_int = false;
                char *endptr = NULL;
                pred->num_val = strtod(pred->str_val, &endptr);
                if (endptr != pred->str_val && *endptr == '\0') {
                    pred->is_num = true;
                } else {
                    pred->is_num = false;
                }
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
        char *endptr = NULL;
        long idx = strtol(pred->col_name, &endptr, 10);
        if (endptr != pred->col_name && *endptr == '\0') {
            pred->col_index = idx;
        } else {
            pred->col_index = hp_header_find_column(header, pred->col_name);
            if (pred->col_index < 0) {
                return HP_ERR_COLUMN_NOT_FOUND;
            }
        }

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

/* Parses a bounded decimal integer span with a sign. Rejects any value that
 * does not fit int64 exactly - silent wraparound on 19-digit overflow would
 * corrupt comparisons, so out-of-range values simply never match. */
static bool hp_parse_int_span(const char *ptr, size_t len, int64_t *out) {
    if (len == 0 || len > 19) return false;
    size_t k = 0;
    bool neg = false;
    if (ptr[0] == '-') {
        neg = true;
        k++;
    } else if (ptr[0] == '+') {
        k++;
    }
    if (k >= len) return false;

    uint64_t v = 0;
    const uint64_t limit = neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    for (; k < len; k++) {
        uint8_t d = (uint8_t)(ptr[k] - '0');
        if (d > 9) return false;
        if (v > (limit - (uint64_t)d) / 10) return false;
        v = v * 10 + (uint64_t)d;
    }
    *out = neg ? (int64_t)(0ull - v) : (int64_t)v;
    return true;
}

static bool hp_pred_matches(const hp_predicate_t *pred, const hp_row_t *row) {
    if (pred->col_index < 0 || (size_t)pred->col_index >= row->field_count) {
        return false;
    }

    const hp_str_t *f = &row->fields[pred->col_index];
    const char *ptr = f->ptr;
    size_t len = f->len;

    if (pred->is_int) {
        if (pred->op == HP_OP_EQ) {
            if (len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) {
                return true;
            }
        } else if (pred->op == HP_OP_NEQ) {
            if (len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) {
                return false;
            }
        }

        int64_t f_int = 0;
        if (!hp_parse_int_span(ptr, len, &f_int)) return false;

        switch (pred->op) {
            case HP_OP_EQ:  if (f_int != pred->int_val) return false; break;
            case HP_OP_NEQ: if (f_int == pred->int_val) return false; break;
            case HP_OP_LT:  if (f_int >= pred->int_val) return false; break;
            case HP_OP_LTE: if (f_int >  pred->int_val) return false; break;
            case HP_OP_GT:  if (f_int <= pred->int_val) return false; break;
            case HP_OP_GTE: if (f_int <  pred->int_val) return false; break;
            default: return false;
        }
    } else if (pred->is_num) {
        /* Declared-int column (--schema): the int64 span parser replaces the
         * locale-bound strtod on the hot path. The comparison itself stays
         * in double space, so fractional literals (e.g. bytes > 5000.5)
         * behave identically against int columns. */
        if (pred->hint_int) {
            int64_t v = 0;
            if (!hp_parse_int_span(ptr, len, &v)) return false;
            double f_val = (double)v;

            switch (pred->op) {
                case HP_OP_EQ:  if (!(f_val == pred->num_val)) return false; break;
                case HP_OP_NEQ: if (!(f_val != pred->num_val)) return false; break;
                case HP_OP_LT:  if (!(f_val <  pred->num_val)) return false; break;
                case HP_OP_LTE: if (!(f_val <= pred->num_val)) return false; break;
                case HP_OP_GT:  if (!(f_val >  pred->num_val)) return false; break;
                case HP_OP_GTE: if (!(f_val >= pred->num_val)) return false; break;
                default: return false;
            }
            return true;
        }

        char buf[64];
        size_t blen = len < 63 ? len : 63;
        memcpy(buf, ptr, blen);
        buf[blen] = '\0';
        char *endptr = NULL;
        double f_val = strtod(buf, &endptr);
        if (endptr == buf) return false;

        switch (pred->op) {
            case HP_OP_EQ:  if (!(f_val == pred->num_val)) return false; break;
            case HP_OP_NEQ: if (!(f_val != pred->num_val)) return false; break;
            case HP_OP_LT:  if (!(f_val <  pred->num_val)) return false; break;
            case HP_OP_LTE: if (!(f_val <= pred->num_val)) return false; break;
            case HP_OP_GT:  if (!(f_val >  pred->num_val)) return false; break;
            case HP_OP_GTE: if (!(f_val >= pred->num_val)) return false; break;
            default: return false;
        }
    } else {
        if (pred->op == HP_OP_EQ) {
            if (len != pred->str_val_len || memcmp(ptr, pred->str_val, len) != 0) return false;
        } else if (pred->op == HP_OP_NEQ) {
            if (len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) return false;
        } else if (pred->op == HP_OP_CONTAINS) {
            if (!hp_mem_contains(ptr, len, pred->str_val, pred->str_val_len)) return false;
        } else if (pred->op == HP_OP_STARTSWITH) {
            if (len < pred->str_val_len || memcmp(ptr, pred->str_val, pred->str_val_len) != 0) return false;
        } else if (pred->op == HP_OP_ENDSWITH) {
            if (len < pred->str_val_len ||
                memcmp(ptr + len - pred->str_val_len, pred->str_val, pred->str_val_len) != 0) return false;
        } else {
            size_t min_len = len < pred->str_val_len ? len : pred->str_val_len;
            int cmp = memcmp(ptr, pred->str_val, min_len);
            if (cmp == 0) {
                if (len < pred->str_val_len) cmp = -1;
                else if (len > pred->str_val_len) cmp = 1;
            }
            switch (pred->op) {
                case HP_OP_LT:  if (cmp >= 0) return false; break;
                case HP_OP_LTE: if (cmp >  0) return false; break;
                case HP_OP_GT:  if (cmp <= 0) return false; break;
                case HP_OP_GTE: if (cmp <  0) return false; break;
                default: return false;
            }
        }
    }

    return true;
}

HP_HOT bool hp_filter_matches(const hp_filter_t *filter, const hp_row_t *row) {
    if (!filter || filter->count == 0) return true;
    if (!row) return false;

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
