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
    }
    return HP_OK;
}

HP_HOT bool hp_filter_matches(const hp_filter_t *filter, const hp_row_t *row) {
    if (!filter || filter->count == 0) return true;
    if (!row) return false;

    for (size_t i = 0; i < filter->count; i++) {
        const hp_predicate_t *pred = &filter->preds[i];
        if (pred->col_index < 0 || (size_t)pred->col_index >= row->field_count) {
            return false;
        }

        const hp_str_t *f = &row->fields[pred->col_index];
        const char *ptr = f->ptr;
        size_t len = f->len;

        if (pred->is_int) {
            if (pred->op == HP_OP_EQ) {
                if (len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) {
                    continue;
                }
            } else if (pred->op == HP_OP_NEQ) {
                if (len == pred->str_val_len && memcmp(ptr, pred->str_val, len) == 0) {
                    return false;
                }
            }

            if (len == 0 || len > 19) return false;
            size_t k = 0;
            bool neg = false;
            if (ptr[0] == '-') { neg = true; k++; }
            else if (ptr[0] == '+') { k++; }
            if (k >= len) return false;

            uint64_t v = 0;
            bool valid = true;
            for (; k < len; k++) {
                uint8_t d = (uint8_t)(ptr[k] - '0');
                if (d > 9) { valid = false; break; }
                v = v * 10 + d;
            }
            if (!valid) return false;
            int64_t f_int = neg ? -(int64_t)v : (int64_t)v;

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
    }

    return true;
}
