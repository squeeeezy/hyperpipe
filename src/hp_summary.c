#include "hyperpipe/hp_summary.h"
#include "hyperpipe/hp_json_ser.h"

void hp_summary_init(hp_summary_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

/* Bounded lexicographic compare between a field span and a stored copy:
 * memcmp over the common prefix, length breaks the tie (same rule the
 * string predicates in hp_filter.c use). */
static int span_cmp(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(a, b, min_len);
    if (cmp != 0) return cmp;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

/* Converts an already validated JSON integer of at most 18 characters, so
 * it always fits int64. Converting the result to double rounds exactly as
 * strtod would, without the copy and the locale-aware parse. */
static int64_t span_to_int64(const char *s, size_t len) {
    size_t i = (s[0] == '-') ? 1 : 0;
    int64_t v = 0;
    for (; i < len; i++) v = v * 10 + (s[i] - '0');
    return s[0] == '-' ? -v : v;
}

void hp_summary_observe(hp_summary_t *s, const hp_header_t *header,
                        const hp_row_t *row) {
    if (!s || !row) return;
    s->rows++;

    size_t n = row->field_count;
    if (n > HP_MAX_COLUMNS) n = HP_MAX_COLUMNS;
    if (n > s->col_count) s->col_count = n;

    for (size_t i = 0; i < n; i++) {
        hp_summary_col_t *col = &s->cols[i];

        /* Capture the column name once; the running header may still be
         * growing on early rows of schema-discovery streams. */
        if (!col->named && header && i < header->count && header->names[i]) {
            size_t nl = strlen(header->names[i]);
            if (nl >= sizeof(col->name)) nl = sizeof(col->name) - 1;
            memcpy(col->name, header->names[i], nl);
            col->name[nl] = '\0';
            col->named = true;
        }

        const hp_str_t *f = &row->fields[i];
        if (!f->present) {
            col->absent++;
            continue;
        }
        col->present++;
        if (f->len == 0) {
            col->empty++;
            continue;
        }

        /* String-typed values (quoted sources) never count as numbers:
         * type fidelity outranks convenience, matching the filter engine. */
        if (!f->has_escapes && hp_json_is_number(f->ptr, f->len)) {
            bool is_int = true;
            for (size_t k = 0; k < f->len; k++) {
                char c = f->ptr[k];
                if (c == '.' || c == 'e' || c == 'E') {
                    is_int = false;
                    break;
                }
            }

            double v = 0.0;
            if (is_int && f->len <= 18) {
                v = (double)span_to_int64(f->ptr, f->len);
            } else {
                hp_json_number_as_double(f->ptr, f->len, &v);
            }
            col->num_count++;
            if (!col->has_numeric) {
                col->min_num = v;
                col->max_num = v;
                col->has_numeric = true;
            } else {
                if (v < col->min_num) col->min_num = v;
                if (v > col->max_num) col->max_num = v;
            }
            col->sum += v;
            if (is_int) col->int_count++;
        } else {
            size_t copy_len = f->len < sizeof(col->min_str) - 1
                                  ? f->len : sizeof(col->min_str) - 1;
            if (!col->has_string) {
                memcpy(col->min_str, f->ptr, copy_len);
                col->min_str[copy_len] = '\0';
                col->min_str_len = copy_len;
                memcpy(col->max_str, f->ptr, copy_len);
                col->max_str[copy_len] = '\0';
                col->max_str_len = copy_len;
                col->has_string = true;
            } else {
                if (span_cmp(f->ptr, f->len, col->min_str, col->min_str_len) < 0) {
                    memcpy(col->min_str, f->ptr, copy_len);
                    col->min_str[copy_len] = '\0';
                    col->min_str_len = copy_len;
                }
                if (span_cmp(f->ptr, f->len, col->max_str, col->max_str_len) > 0) {
                    memcpy(col->max_str, f->ptr, copy_len);
                    col->max_str[copy_len] = '\0';
                    col->max_str_len = copy_len;
                }
            }
        }
    }
}

static void summary_print_value(FILE *out, const hp_summary_col_t *col,
                                bool numeric, bool as_int, bool is_min) {
    if (numeric) {
        double v = is_min ? col->min_num : col->max_num;
        if (as_int) {
            fprintf(out, "%lld", (long long)v);
        } else {
            fprintf(out, "%.10g", v);
        }
    } else if (is_min && col->has_string) {
        fwrite(col->min_str, 1, col->min_str_len, out);
    } else if (col->has_string) {
        fwrite(col->max_str, 1, col->max_str_len, out);
    } else {
        fputc('-', out);
    }
}

void hp_summary_print(const hp_summary_t *s, FILE *out) {
    if (!s || !out) return;

    fprintf(out, "%-24s %-7s %12s %9s %9s  %-18s %-18s %14s\n",
            "column", "type", "present", "empty", "absent", "min", "max", "mean");

    for (size_t i = 0; i < s->col_count; i++) {
        const hp_summary_col_t *col = &s->cols[i];

        const char *type = "-";
        uint64_t values = col->present - col->empty;
        if (col->num_count > 0 && col->num_count == values) {
            type = (col->int_count == col->num_count) ? "int" : "number";
        } else if (col->num_count > 0 && values > col->num_count) {
            type = "mixed";
        } else if (values > 0) {
            type = "string";
        }
        bool numeric = col->has_numeric;
        bool as_int = numeric && (col->int_count == col->num_count);

        fprintf(out, "%-24s %-7s %12llu %9llu %9llu  ",
                col->named ? col->name : "col_?",
                type,
                (unsigned long long)col->present,
                (unsigned long long)col->empty,
                (unsigned long long)col->absent);
        summary_print_value(out, col, numeric, as_int, true);
        fprintf(out, "  ");
        summary_print_value(out, col, numeric, as_int, false);
        fprintf(out, "  ");
        if (col->num_count > 0) {
            fprintf(out, "%14.10g", col->sum / (double)col->num_count);
        } else {
            fprintf(out, "%14s", "-");
        }
        fputc('\n', out);
    }

    fprintf(out, "rows: %llu\n", (unsigned long long)s->rows);
}
