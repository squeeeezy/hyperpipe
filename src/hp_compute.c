#include "hyperpipe/hp_compute.h"
#include "hyperpipe/hp_filter.h"
#include "hyperpipe/hp_time.h"
#include "hp_number.h"
#include <ctype.h>
#include <math.h>

typedef enum expr_kind_e {
    EXPR_NUMBER,
    EXPR_TEXT,
    EXPR_COLUMN,           /* a field as it is; unresolved before binding */
    EXPR_PATH,             /* a value inside a field's JSON */
    EXPR_NEG,
    EXPR_ADD,
    EXPR_SUB,
    EXPR_MUL,
    EXPR_DIV,
    EXPR_MOD,
    EXPR_CALL
} expr_kind_t;

typedef enum fn_e {
    FN_LOWER,
    FN_UPPER,
    FN_TRIM,
    FN_LEN,
    FN_CONCAT,
    FN_COALESCE,
    FN_SUBSTR,
    FN_REPLACE,
    FN_SPLIT_PART,
    FN_ABS,
    FN_ROUND,
    FN_FLOOR,
    FN_CEIL,
    FN_TIME                /* year(), month(), day(), hour(), minute() */
} fn_t;

static const struct {
    const char *name;
    fn_t fn;
    uint8_t min_args, max_args;
} functions[] = {
    { "lower", FN_LOWER, 1, 1 },
    { "upper", FN_UPPER, 1, 1 },
    { "trim", FN_TRIM, 1, 1 },
    { "len", FN_LEN, 1, 1 },
    { "concat", FN_CONCAT, 1, HP_COMPUTE_MAX_ARGS },
    { "coalesce", FN_COALESCE, 1, HP_COMPUTE_MAX_ARGS },
    { "substr", FN_SUBSTR, 2, 3 },
    { "replace", FN_REPLACE, 3, 3 },
    { "split_part", FN_SPLIT_PART, 3, 3 },
    { "abs", FN_ABS, 1, 1 },
    { "round", FN_ROUND, 1, 2 },
    { "floor", FN_FLOOR, 1, 1 },
    { "ceil", FN_CEIL, 1, 1 },
};

#define MAX_DEPTH 64

static const int64_t pow10_i64[19] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
    10000000000, 100000000000, 1000000000000, 10000000000000, 100000000000000,
    1000000000000000, 10000000000000000, 100000000000000000, 1000000000000000000,
};

static bool is_name_byte(unsigned char ch) {
    return isalnum(ch) || ch == '_' || ch == '@' || ch == '$' || ch >= 0x80;
}

static const char *skip_space(const char *p) {
    while (isspace((unsigned char)*p)) p++;
    return p;
}

/* ---- parsing ------------------------------------------------------------------ */

typedef struct parser_s {
    hp_compute_t *c;
    const char *spec;      /* the whole --add text, for error offsets */
    const char *p;
    int depth;
} parser_t;

static int fail(parser_t *ps, const char *at, const char *why) {
    if (!ps->c->error) {
        ps->c->error = why;
        ps->c->error_at = (size_t)(at - ps->spec);
    }
    return -1;
}

static int alloc_node(hp_compute_t *c, expr_kind_t kind, uint32_t at) {
    if (c->node_count == HP_COMPUTE_MAX_NODES) return -1;
    hp_expr_t *e = &c->nodes[c->node_count];
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    e->col = -1;
    e->at = at;
    return (int)c->node_count++;
}

static int new_node(parser_t *ps, expr_kind_t kind, const char *at) {
    int n = alloc_node(ps->c, kind, (uint32_t)(at - ps->spec));
    return n < 0 ? fail(ps, at, "expression too long") : n;
}

/* Keeps a and then b, NUL-terminated, in the text pool */
static bool keep_text(hp_compute_t *c, const char *a, size_t a_len, const char *b, size_t b_len,
                      uint32_t *off) {
    if (c->text_len + a_len + b_len + 1 > HP_COMPUTE_TEXT) return false;
    char *t = c->text + c->text_len;
    memcpy(t, a, a_len);
    memcpy(t + a_len, b, b_len);
    t[a_len + b_len] = '\0';
    *off = (uint32_t)c->text_len;
    c->text_len += a_len + b_len + 1;
    return true;
}

static int parse_sum(parser_t *ps);

static int binary(parser_t *ps, expr_kind_t kind, int left, int right, const char *at) {
    int n = new_node(ps, kind, at);
    if (n < 0) return -1;
    hp_expr_t *e = &ps->c->nodes[n];
    e->argc = 2;
    e->arg[0] = (uint16_t)left;
    e->arg[1] = (uint16_t)right;
    return n;
}

static bool is_digit(char c) {
    return isdigit((unsigned char)c);
}

static int parse_number(parser_t *ps) {
    const char *p = ps->p, *start = p;
    while (is_digit(*p)) p++;
    if (*p == '.' && is_digit(p[1])) {
        for (p++; is_digit(*p); p++) {}
    }
    if (*p == 'e' || *p == 'E') {
        bool sign = p[1] == '+' || p[1] == '-';
        if (is_digit(p[1 + sign])) {
            for (p += 2 + sign; is_digit(*p); p++) {}
        }
    }
    ps->p = p;

    int n = new_node(ps, EXPR_NUMBER, start);
    if (n < 0) return -1;
    hp_expr_t *e = &ps->c->nodes[n];
    size_t len = (size_t)(p - start);
    if (!keep_text(ps->c, start, len, "", 0, &e->text)) {
        return fail(ps, start, "expression too long");
    }
    e->text_len = (uint32_t)len;
    e->exact = hp_parse_decimal_exact(start, len, &e->mant, &e->scale);
    if (!e->exact && !hp_parse_decimal_span(start, len, &e->f)) {
        return fail(ps, start, "invalid number");
    }
    return n;
}

/* 'text' or "text", taken as it is written */
static int parse_text(parser_t *ps) {
    const char *open = ps->p;
    const char *close = strchr(open + 1, *open);
    if (!close) return fail(ps, open, "unterminated quote");
    ps->p = close + 1;

    int n = new_node(ps, EXPR_TEXT, open);
    if (n < 0) return -1;
    hp_expr_t *e = &ps->c->nodes[n];
    e->text_len = (uint32_t)(close - open - 1);
    if (!keep_text(ps->c, open + 1, e->text_len, "", 0, &e->text)) {
        return fail(ps, open, "expression too long");
    }
    return n;
}

/* .key and [index] steps after a name */
static const char *scan_path(const char *p) {
    for (;;) {
        if (p[0] == '.' && is_name_byte((unsigned char)p[1])) {
            for (p++; is_name_byte((unsigned char)*p); p++) {}
            continue;
        }
        const char *q = p + 1;
        if (p[0] != '[') return p;
        if (*q == '-') q++;
        if (!is_digit(*q)) return p;
        while (is_digit(*q)) q++;
        if (*q != ']') return p;
        p = q + 1;
    }
}

static int parse_call(parser_t *ps, const char *name, size_t name_len, const char *paren) {
    int n = new_node(ps, EXPR_CALL, name);
    if (n < 0) return -1;
    hp_expr_t *e = &ps->c->nodes[n];

    size_t k = 0, count = sizeof(functions) / sizeof(functions[0]);
    while (k < count && !(strlen(functions[k].name) == name_len &&
                          memcmp(functions[k].name, name, name_len) == 0)) {
        k++;
    }
    hp_time_unit_t unit = HP_TIME_YEAR;
    uint8_t min_args = 1, max_args = 1;
    if (k < count) {
        e->fn = (uint8_t)functions[k].fn;
        min_args = functions[k].min_args;
        max_args = functions[k].max_args;
    } else if (hp_time_unit_from_name(name, name_len, &unit)) {
        e->fn = FN_TIME;
        e->unit = (uint8_t)unit;
    } else {
        return fail(ps, name, "unknown function");
    }

    ps->p = skip_space(paren + 1);
    if (*ps->p == ')') {
        ps->p++;
    } else {
        for (;;) {
            const char *at = skip_space(ps->p);
            if (e->argc == max_args) return fail(ps, at, "too many arguments");
            int arg = parse_sum(ps);
            if (arg < 0) return -1;
            e->arg[e->argc++] = (uint16_t)arg;
            ps->p = skip_space(ps->p);
            if (*ps->p == ')') break;
            if (*ps->p != ',') return fail(ps, ps->p, "expected ',' or ')'");
            ps->p++;
        }
        ps->p++;
    }
    if (e->argc < min_args) return fail(ps, name, "too few arguments");

    /* Text to find and its replacement are quoted, so the result has a
     * bound: each byte of the input at most becomes the replacement */
    if (e->fn == FN_REPLACE) {
        const hp_expr_t *from = &ps->c->nodes[e->arg[1]], *to = &ps->c->nodes[e->arg[2]];
        if (from->kind != EXPR_TEXT || from->text_len == 0 || to->kind != EXPR_TEXT) {
            return fail(ps, ps->spec + from->at, "replace takes quoted text to find and to "
                                                 "put in its place");
        }
    }
    return n;
}

/* A column, a path into one, or a function call. `quoted` names may hold
 * any byte but a backquote. */
static int parse_name(parser_t *ps) {
    const char *start = ps->p, *p = start, *root = start;
    size_t root_len = 0;
    if (*p == '`') {
        const char *close = strchr(p + 1, '`');
        if (!close) return fail(ps, p, "unterminated `");
        root = p + 1;
        root_len = (size_t)(close - root);
        if (root_len == 0) return fail(ps, p, "empty column name");
        p = close + 1;
    } else {
        while (is_name_byte((unsigned char)*p)) p++;
        const char *q = skip_space(p);
        if (*q == '(') return parse_call(ps, start, (size_t)(p - start), q);
    }
    const char *path = p;
    p = scan_path(p);
    ps->p = p;

    int n = new_node(ps, EXPR_COLUMN, start);
    if (n < 0) return -1;
    hp_expr_t *e = &ps->c->nodes[n];
    const char *name = root_len ? root : start;
    size_t name_len = root_len ? root_len : (size_t)(path - start);
    if (!keep_text(ps->c, name, name_len, path, (size_t)(p - path), &e->text)) {
        return fail(ps, start, "expression too long");
    }
    e->text_len = (uint32_t)(name_len + (size_t)(p - path));
    e->root_len = (uint32_t)root_len;
    return n;
}

static int parse_unary(parser_t *ps) {
    const char *p = ps->p = skip_space(ps->p);
    if (++ps->depth > MAX_DEPTH) return fail(ps, p, "expression nested too deeply");

    int n;
    if (*p == '-') {
        ps->p++;
        int arg = parse_unary(ps);
        n = arg < 0 ? -1 : new_node(ps, EXPR_NEG, p);
        if (n >= 0) {
            ps->c->nodes[n].argc = 1;
            ps->c->nodes[n].arg[0] = (uint16_t)arg;
        }
    } else if (*p == '(') {
        ps->p++;
        n = parse_sum(ps);
        ps->p = skip_space(ps->p);
        if (n >= 0 && *ps->p != ')') n = fail(ps, ps->p, "expected ')'");
        ps->p++;
    } else if (is_digit(*p)) {
        n = parse_number(ps);
    } else if (*p == '\'' || *p == '"') {
        n = parse_text(ps);
    } else if (*p == '`' || is_name_byte((unsigned char)*p)) {
        n = parse_name(ps);
    } else {
        n = fail(ps, p, "expected a column, a number, quoted text or a function");
    }
    ps->depth--;
    return n;
}

static int parse_product(parser_t *ps) {
    int left = parse_unary(ps);
    while (left >= 0) {
        const char *at = ps->p = skip_space(ps->p);
        expr_kind_t kind = *at == '*' ? EXPR_MUL : *at == '/' ? EXPR_DIV : *at == '%' ? EXPR_MOD
                                                                              : EXPR_CALL;
        if (kind == EXPR_CALL) break;
        ps->p++;
        int right = parse_unary(ps);
        left = right < 0 ? -1 : binary(ps, kind, left, right, at);
    }
    return left;
}

static int parse_sum(parser_t *ps) {
    int left = parse_product(ps);
    while (left >= 0) {
        const char *at = ps->p = skip_space(ps->p);
        if (*at != '+' && *at != '-') break;
        ps->p++;
        int right = parse_product(ps);
        left = right < 0 ? -1 : binary(ps, *at == '+' ? EXPR_ADD : EXPR_SUB, left, right, at);
    }
    return left;
}

static hp_error_t init_error(parser_t *ps, const char *at, const char *why) {
    fail(ps, at, why);
    return HP_ERR_COMPUTE;
}

/* Columns are "name = expression", separated by commas */
hp_error_t hp_compute_init(hp_compute_t *c, const char *spec) {
    if (!c) return HP_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));
    if (!spec) return HP_OK;

    parser_t ps = { c, spec, spec, 0 };
    const char *p = skip_space(spec);
    while (*p) {
        const char *name = p;
        while (*p && *p != '=' && *p != ',') p++;
        const char *end = p;
        while (end > name && isspace((unsigned char)end[-1])) end--;
        size_t len = (size_t)(end - name);

        if (len == 0) return init_error(&ps, name, "expected a column name");
        if (*p != '=' || p[1] == '=') {
            return init_error(&ps, p, "expected '=' after the column name");
        }
        if (len >= sizeof(c->cols[0].name)) {
            return init_error(&ps, name, "column name longer than 127 bytes");
        }
        if (c->count == HP_COMPUTE_MAX_COLUMNS) {
            return init_error(&ps, name, "more than 64 columns");
        }

        ps.p = p + 1;
        int root = parse_sum(&ps);
        if (root < 0) return HP_ERR_COMPUTE;
        hp_computed_t *col = &c->cols[c->count++];
        memcpy(col->name, name, len);
        col->name[len] = '\0';
        col->root = (uint16_t)root;
        col->index = -1;

        p = skip_space(ps.p);
        if (*p && *p != ',') {
            return init_error(&ps, p, "expected an operator, or ',' and the next column");
        }
        if (*p) p = skip_space(p + 1);
    }
    return HP_OK;
}

void hp_compute_free(hp_compute_t *c) {
    if (!c) return;
    free(c->arena);
    c->arena = NULL;
    c->arena_cap = 0;
}

/* ---- binding -------------------------------------------------------------- */

static size_t add_sat(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }
static size_t mul_sat(size_t a, size_t b) { return (b && a > SIZE_MAX / b) ? SIZE_MAX : a * b; }

static hp_bound_t bound_add(hp_bound_t a, hp_bound_t b) {
    return (hp_bound_t){ add_sat(a.per_byte, b.per_byte), add_sat(a.fixed, b.fixed) };
}

static hp_bound_t bound_mul(hp_bound_t a, size_t k) {
    return (hp_bound_t){ mul_sat(a.per_byte, k), mul_sat(a.fixed, k) };
}

/* One step of a path at s: ".key", ".0" or "[-1]"; returns its length, 0
 * when s does not start with one */
static size_t parse_step(const char *s, size_t len, hp_json_step_t *step) {
    size_t i = 1;
    const char *digits = s + 1;
    if (s[0] == '.') {
        while (i < len && s[i] != '.' && s[i] != '[') i++;
        step->key = s + 1;
        step->key_len = i - 1;
        if (step->key_len == 0) return 0;
    } else if (s[0] == '[') {
        while (i < len && s[i] != ']') i++;
        if (i == len) return 0;
        step->key = NULL;
        step->key_len = 0;
    } else {
        return 0;
    }

    int64_t index;
    size_t n = (size_t)(s + i - digits);
    bool numeric = n > 0 && (is_digit(digits[0]) || (s[0] == '[' && digits[0] == '-'));
    step->index = numeric && hp_parse_int_span(digits, n, &index) ? index : HP_JSON_NO_INDEX;
    if (!step->key && step->index == HP_JSON_NO_INDEX) return 0;
    return s[0] == '[' ? i + 1 : i;
}

/* The steps after a column's name */
static bool parse_steps(hp_compute_t *c, const char *s, size_t len, hp_expr_t *e) {
    size_t first = c->step_count, i = 0;
    while (i < len) {
        hp_json_step_t *step = &c->steps[c->step_count];
        size_t n = c->step_count < HP_COMPUTE_MAX_STEPS ? parse_step(s + i, len - i, step) : 0;
        if (n == 0) {
            c->step_count = first;
            return false;
        }
        c->step_count++;
        i += n;
    }
    e->step = (uint16_t)first;
    e->step_count = (uint16_t)(c->step_count - first);
    return e->step_count > 0;
}

/* The column that len bytes of name refer to, by name or position */
static int64_t find_column(const hp_header_t *h, const char *name, size_t len) {
    char buf[1024];
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, name, len);
    buf[len] = '\0';
    return hp_resolve_column(h, buf);
}

/* Binds a reference: the column it names, or else a path into the column
 * that the longest leading part names, as 1.user.id reads into the second */
static bool resolve(hp_compute_t *c, hp_expr_t *e, const hp_header_t *h) {
    const char *text = c->text + e->text;
    if (e->root_len == 0) {
        e->col = hp_resolve_column(h, text);
        for (size_t cut = e->text_len; e->col < 0 && cut-- > 1;) {
            if (text[cut] != '.' && text[cut] != '[') continue;
            int64_t col = find_column(h, text, cut);
            if (col >= 0 && parse_steps(c, text + cut, e->text_len - cut, e)) {
                e->kind = EXPR_PATH;
                e->col = col;
            }
        }
        return e->col >= 0;
    }
    e->col = find_column(h, text, e->root_len);
    if (e->col < 0 || e->text_len == e->root_len) return e->col >= 0;
    e->kind = EXPR_PATH;
    return parse_steps(c, text + e->root_len, e->text_len - e->root_len, e);
}

/* The bound of field index's value, as the column at pos sees it */
static hp_bound_t field_bound(const hp_compute_t *c, int64_t index, size_t pos) {
    for (size_t j = pos; j-- > 0;) {
        if (c->cols[j].index == index) return c->cols[j].bound;
    }
    return (hp_bound_t){ 1, 0 };
}

/* Binds node n of the column at pos and bounds its value in *out; what it
 * writes to the arena goes into c->arena_need */
static bool bind_node(hp_compute_t *c, uint16_t n, const hp_header_t *h, size_t pos,
                      hp_bound_t *out, const char **missing) {
    hp_expr_t *e = &c->nodes[n];
    hp_bound_t args = { 0, 0 };
    hp_bound_t first = { 0, 0 };
    for (uint8_t a = 0; a < e->argc; a++) {
        hp_bound_t b;
        if (!bind_node(c, e->arg[a], h, pos, &b, missing)) return false;
        args = bound_add(args, b);
        if (a == 0) first = b;
    }

    const hp_bound_t number = { 0, HP_NUMBER_TEXT_MAX };
    hp_bound_t written = { 0, 0 };
    switch (e->kind) {
        case EXPR_NUMBER:
        case EXPR_TEXT:
            *out = (hp_bound_t){ 0, e->text_len };
            break;
        case EXPR_COLUMN:
        case EXPR_PATH:
            if (!resolve(c, e, h)) {
                *missing = c->text + e->text;
                return false;
            }
            *out = field_bound(c, e->col, pos);
            if (e->kind == EXPR_PATH) written = *out;   /* a decoded string */
            break;
        case EXPR_CALL:
            switch ((fn_t)e->fn) {
                case FN_LOWER:
                case FN_UPPER:
                    *out = written = first;
                    break;
                case FN_TRIM:
                case FN_SUBSTR:
                case FN_SPLIT_PART:
                case FN_COALESCE:
                    *out = e->fn == FN_COALESCE ? args : first;
                    break;
                case FN_CONCAT:
                    *out = written = args;
                    break;
                case FN_REPLACE:
                    *out = written = bound_mul(first, 1 + c->nodes[e->arg[2]].text_len);
                    break;
                case FN_TIME:
                    *out = written = (hp_bound_t){ 0, HP_TIME_TEXT_MAX };
                    break;
                default:
                    *out = written = number;
            }
            break;
        default:
            *out = written = number;
    }
    c->arena_need = bound_add(c->arena_need, written);
    return true;
}

static hp_error_t append_name(hp_header_t *header, const char *name, int64_t *index) {
    if (header->count == HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
    char *copy = strdup(name);
    if (!copy) return HP_ERR_NOMEM;
    *index = (int64_t)header->count;
    header->names[header->count++] = copy;
    return HP_OK;
}

hp_error_t hp_compute_bind(hp_compute_t *c, hp_header_t *header, const char **missing) {
    if (!c || !header) return HP_ERR_INVALID_ARG;
    c->width = header->count;
    for (size_t i = 0; i < c->count; i++) {
        hp_computed_t *col = &c->cols[i];
        if (!bind_node(c, col->root, header, i, &col->bound, missing)) {
            return HP_ERR_COLUMN_NOT_FOUND;
        }
        col->index = hp_header_find_column(header, col->name);
        if (col->index < 0) {
            hp_error_t err = append_name(header, col->name, &col->index);
            if (err != HP_OK) return err;
            c->appends = true;
        }
    }
    return HP_OK;
}

hp_error_t hp_compute_path(hp_compute_t *c, hp_header_t *header, const char *name) {
    size_t len = strlen(name);
    if (!strpbrk(name, ".[") || len >= sizeof(c->cols[0].name)) return HP_ERR_COLUMN_NOT_FOUND;
    if (c->count == HP_COMPUTE_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;

    /* A path is a reference on its own: kept only when it binds as one */
    const size_t nodes = c->node_count, text = c->text_len, steps = c->step_count;
    const hp_bound_t need = c->arena_need;
    hp_computed_t *col = &c->cols[c->count];
    int n = alloc_node(c, EXPR_COLUMN, 0);
    const char *unused = NULL;
    if (n >= 0 && keep_text(c, name, len, "", 0, &c->nodes[n].text)) {
        c->nodes[n].text_len = (uint32_t)len;
        if (bind_node(c, (uint16_t)n, header, c->count, &col->bound, &unused) &&
            c->nodes[n].kind == EXPR_PATH) {
            memcpy(col->name, name, len + 1);
            col->root = (uint16_t)n;
            col->hidden = true;
            c->count++;
            c->hidden++;
            c->appends = true;
            return append_name(header, name, &col->index);
        }
    }
    c->node_count = nodes;
    c->text_len = text;
    c->step_count = steps;
    c->arena_need = need;
    return n < 0 ? HP_ERR_BUFFER_OVERFLOW : HP_ERR_COLUMN_NOT_FOUND;
}

/* ---- evaluation ------------------------------------------------------------------ */

/* A value: text, or a number whose text is written only when it is needed */
typedef struct value_s {
    hp_str_t s;            /* ptr is NULL for a number not yet written */
    bool num;
    bool exact;            /* mant * 10^-scale, or f */
    int scale;
    int64_t mant;
    double f;
} value_t;

static value_t no_value(void) {
    return (value_t){ .s = { .ptr = "", .len = 0 } };
}

static value_t text_value(const char *p, size_t len) {
    return (value_t){ .s = { .ptr = p, .len = len, .has_escapes = true, .present = true } };
}

static value_t double_value(double f) {
    if (!isfinite(f)) return no_value();
    return (value_t){ .s = { .present = true }, .num = true, .f = f };
}

/* mant * 10^-scale, 0 <= scale <= 36, rounded once to the nearest double */
static double wide_to_double(hp_wide_t mant, int scale) {
    char text[HP_NUMBER_TEXT_MAX];
    text[hp_format_decimal(text, mant, scale)] = '\0';
    return strtod(text, NULL);
}

/* mant * 10^-scale exactly, or in double when it is too wide */
static value_t exact_value(hp_wide_t mant, int scale) {
    if (mant < INT64_MIN || mant > INT64_MAX) return double_value(wide_to_double(mant, scale));
    return (value_t){ .s = { .present = true }, .num = true, .exact = true, .scale = scale,
                      .mant = (int64_t)mant };
}

static double to_double(const value_t *v) {
    return v->exact ? (double)v->mant / (double)pow10_i64[v->scale] : v->f;
}

static bool as_number(value_t *v) {
    if (v->num) return true;
    if (!v->s.present || v->s.is_json) return false;
    if (hp_parse_decimal_exact(v->s.ptr, v->s.len, &v->mant, &v->scale)) {
        v->exact = true;
    } else if (!hp_parse_decimal_span(v->s.ptr, v->s.len, &v->f)) {
        return false;
    }
    v->num = true;
    return true;
}

/* A whole number from v, rounded toward zero */
static bool as_integer(value_t *v, int64_t *out) {
    if (!as_number(v)) return false;
    if (v->exact) {
        *out = v->mant / pow10_i64[v->scale];
        return true;
    }
    if (!(v->f > -9e18 && v->f < 9e18)) return false;
    *out = (int64_t)v->f;
    return true;
}

static char *arena_at(hp_compute_t *c) {
    return c->arena + c->arena_len;
}

/* v's text; a number is written when it is first needed */
static hp_str_t text_of(hp_compute_t *c, value_t *v) {
    if (v->s.ptr) return v->s;
    char *out = arena_at(c);
    size_t n;
    if (v->exact) {
        n = hp_format_decimal(out, v->mant, v->scale);
    } else {
        n = hp_format_double(out, v->f);
    }
    c->arena_len += n;
    v->s.ptr = out;
    v->s.len = n;
    return v->s;
}

static value_t arith(expr_kind_t kind, value_t a, value_t b) {
    if (!as_number(&a) || !as_number(&b)) return no_value();
    if (kind != EXPR_DIV && a.exact && b.exact && sizeof(hp_wide_t) > sizeof(int64_t)) {
        int s = a.scale > b.scale ? a.scale : b.scale;
        hp_wide_t x = (hp_wide_t)a.mant * pow10_i64[s - a.scale];
        hp_wide_t y = (hp_wide_t)b.mant * pow10_i64[s - b.scale];
        if (kind == EXPR_ADD) return exact_value(x + y, s);
        if (kind == EXPR_SUB) return exact_value(x - y, s);
        if (kind == EXPR_MOD) return y == 0 ? no_value() : exact_value(x % y, s);

        hp_wide_t m = (hp_wide_t)a.mant * b.mant;
        int scale = a.scale + b.scale;
        while (scale > HP_DEC_MAX_DIGITS && m % 10 == 0) {
            m /= 10;
            scale--;
        }
        return scale <= HP_DEC_MAX_DIGITS ? exact_value(m, scale)
                                          : double_value(wide_to_double(m, scale));
    }
    double x = to_double(&a), y = to_double(&b);
    switch (kind) {
        case EXPR_ADD: return double_value(x + y);
        case EXPR_SUB: return double_value(x - y);
        case EXPR_MUL: return double_value(x * y);
        case EXPR_DIV: return y == 0 ? no_value() : double_value(x / y);
        default:       return y == 0 ? no_value() : double_value(fmod(x, y));
    }
}

/* round() to digits after the point, half away from zero; floor() and
 * ceil() to whole numbers */
static value_t round_value(value_t v, int64_t digits, fn_t fn) {
    if (!as_number(&v)) return no_value();
    if (digits > HP_DEC_MAX_DIGITS) return v;
    if (digits < -HP_DEC_MAX_DIGITS) digits = -HP_DEC_MAX_DIGITS;

    if (v.exact && sizeof(hp_wide_t) > sizeof(int64_t)) {
        int drop = v.scale - (int)digits;
        if (drop <= 0) return v;
        hp_wide_t p = 1;
        for (int i = 0; i < drop; i++) p *= 10;
        hp_wide_t q = v.mant / p, r = v.mant % p;
        if (fn == FN_ROUND && 2 * (r < 0 ? -r : r) >= p) q += v.mant < 0 ? -1 : 1;
        if (fn == FN_FLOOR && r < 0) q--;
        if (fn == FN_CEIL && r > 0) q++;
        return digits >= 0 ? exact_value(q, (int)digits) : exact_value(q * pow10_i64[-digits], 0);
    }

    double p = pow(10.0, (double)digits), x = to_double(&v) * p;
    x = fn == FN_FLOOR ? floor(x) : fn == FN_CEIL ? ceil(x) : round(x);
    return double_value(x / p);
}

/* In a block where letters come in pairs, the other case of cp */
static unsigned pair_case(unsigned cp, bool upper, bool upper_is_even) {
    bool is_upper = ((cp & 1) == 0) == upper_is_even;
    if (is_upper == upper) return cp;
    return is_upper ? cp + 1 : cp - 1;
}

/* The case of a Latin, Greek or Cyrillic letter; anything else, and the
 * letters whose other case is not the same length in UTF-8, as it is */
static unsigned letter_case(unsigned cp, bool upper) {
    if (cp >= 0xC0 && cp <= 0xFE && cp != 0xD7 && cp != 0xF7 && cp != 0xDF) {
        return upper ? (cp >= 0xE0 ? cp - 0x20 : cp) : (cp <= 0xDE ? cp + 0x20 : cp);
    }
    if (cp == 0xFF || cp == 0x178) return upper ? 0x178 : 0xFF;
    if ((cp >= 0x100 && cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) ||
        (cp >= 0x14A && cp <= 0x177) || (cp >= 0x460 && cp <= 0x481) ||
        (cp >= 0x48A && cp <= 0x4BF)) {
        return pair_case(cp, upper, true);
    }
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
        return pair_case(cp, upper, false);
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return upper ? cp : cp + 0x20;
    if (cp >= 0x3B1 && cp <= 0x3C9) return upper ? (cp == 0x3C2 ? 0x3A3 : cp - 0x20) : cp;
    if (cp >= 0x400 && cp <= 0x40F) return upper ? cp : cp + 0x50;
    if (cp >= 0x410 && cp <= 0x42F) return upper ? cp : cp + 0x20;
    if (cp >= 0x430 && cp <= 0x44F) return upper ? cp - 0x20 : cp;
    if (cp >= 0x450 && cp <= 0x45F) return upper ? cp - 0x50 : cp;
    return cp;
}

/* lower() and upper(): the same length as the text, in UTF-8 */
static void change_case(char *out, const char *p, size_t len, bool upper) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)p[i];
        if (b < 0x80) {
            out[i] = (char)(upper ? toupper(b) : tolower(b));
        } else if ((b & 0xE0) == 0xC0 && i + 1 < len && ((uint8_t)p[i + 1] & 0xC0) == 0x80) {
            unsigned cp = letter_case(((b & 0x1Fu) << 6) | ((uint8_t)p[i + 1] & 0x3Fu), upper);
            out[i] = (char)(0xC0 | (cp >> 6));
            out[++i] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[i] = (char)b;
        }
    }
}

/* Code points of UTF-8 text: the bytes that do not continue one */
static size_t code_points(const char *p, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) n += ((uint8_t)p[i] & 0xC0) != 0x80;
    return n;
}

/* The byte offset of code point k, counted from 0 */
static size_t point_offset(const char *p, size_t len, size_t k) {
    size_t i = 0;
    for (; i < len; i++) {
        if (((uint8_t)p[i] & 0xC0) != 0x80 && k-- == 0) break;
    }
    return i;
}

/* substr(s, start, count): code points from start, counted from 1, or from
 * the end when negative */
static value_t substr_value(hp_compute_t *c, value_t *s, value_t *start, value_t *count) {
    int64_t from, n = INT64_MAX;
    if (!s->s.present && !s->num) return no_value();
    if (!as_integer(start, &from) || (count && !as_integer(count, &n))) return no_value();
    hp_str_t t = text_of(c, s);
    int64_t points = (int64_t)code_points(t.ptr, t.len);
    if (from < 0) from += points + 1;
    if (from < 1) {
        n = n == INT64_MAX ? n : n + from - 1;
        from = 1;
    }
    if (n <= 0 || from > points) return text_value(t.ptr, 0);
    int64_t to = n > points - from + 1 ? points + 1 : from + n;
    size_t a = point_offset(t.ptr, t.len, (size_t)(from - 1));
    size_t b = point_offset(t.ptr, t.len, (size_t)(to - 1));
    return text_value(t.ptr + a, b - a);
}

static value_t replace_value(hp_compute_t *c, value_t *s, const hp_expr_t *from,
                             const hp_expr_t *to) {
    if (!s->s.present && !s->num) return no_value();
    hp_str_t t = text_of(c, s);
    const char *f = c->text + from->text, *r = c->text + to->text;
    char *out = arena_at(c);
    size_t w = 0;
    for (size_t i = 0; i < t.len;) {
        if (t.len - i >= from->text_len && memcmp(t.ptr + i, f, from->text_len) == 0) {
            memcpy(out + w, r, to->text_len);
            w += to->text_len;
            i += from->text_len;
        } else {
            out[w++] = t.ptr[i++];
        }
    }
    c->arena_len += w;
    return text_value(out, w);
}

static bool starts_with(const char *p, size_t len, hp_str_t prefix) {
    return prefix.len > 0 && len >= prefix.len && memcmp(p, prefix.ptr, prefix.len) == 0;
}

/* split_part(s, sep, n): the nth part, counted from 1, empty past the last */
static value_t split_value(hp_compute_t *c, value_t *s, value_t *sep, value_t *index) {
    int64_t n;
    if ((!s->s.present && !s->num) || !as_integer(index, &n) || n < 1) return no_value();
    hp_str_t t = text_of(c, s), d = text_of(c, sep);
    size_t begin = 0;
    for (int64_t part = 1;; part++) {
        size_t end = begin;
        while (end < t.len && !starts_with(t.ptr + end, t.len - end, d)) end++;
        if (part == n) return text_value(t.ptr + begin, end - begin);
        if (end == t.len) return text_value(t.ptr, 0);
        begin = end + d.len;
    }
}

static value_t eval(hp_compute_t *c, const hp_expr_t *e, const hp_row_t *row);

static value_t call(hp_compute_t *c, const hp_expr_t *e, const hp_row_t *row) {
    value_t v[HP_COMPUTE_MAX_ARGS];
    for (uint8_t a = 0; a < e->argc; a++) v[a] = eval(c, &c->nodes[e->arg[a]], row);
    const bool has = v[0].s.present || v[0].num;

    switch ((fn_t)e->fn) {
        case FN_LOWER:
        case FN_UPPER: {
            if (!has) return no_value();
            hp_str_t t = text_of(c, &v[0]);
            char *out = arena_at(c);
            change_case(out, t.ptr, t.len, e->fn == FN_UPPER);
            c->arena_len += t.len;
            return text_value(out, t.len);
        }
        case FN_TRIM: {
            if (!has) return no_value();
            hp_str_t t = text_of(c, &v[0]);
            size_t a = 0, b = t.len;
            while (a < b && isspace((unsigned char)t.ptr[a])) a++;
            while (b > a && isspace((unsigned char)t.ptr[b - 1])) b--;
            return text_value(t.ptr + a, b - a);
        }
        case FN_LEN: {
            if (!has) return no_value();
            hp_str_t t = text_of(c, &v[0]);
            return exact_value((hp_wide_t)code_points(t.ptr, t.len), 0);
        }
        case FN_CONCAT: {
            /* Written after the arguments, which may themselves be in the
             * arena */
            hp_str_t t[HP_COMPUTE_MAX_ARGS];
            for (uint8_t a = 0; a < e->argc; a++) t[a] = text_of(c, &v[a]);
            char *out = arena_at(c);
            size_t w = 0;
            for (uint8_t a = 0; a < e->argc; a++) {
                memcpy(out + w, t[a].ptr, t[a].len);
                w += t[a].len;
            }
            c->arena_len += w;
            return text_value(out, w);
        }
        case FN_COALESCE:
            for (uint8_t a = 0; a < e->argc; a++) {
                if (v[a].num || v[a].s.len > 0) return v[a];
            }
            return no_value();
        case FN_SUBSTR:
            return substr_value(c, &v[0], &v[1], e->argc > 2 ? &v[2] : NULL);
        case FN_REPLACE:
            return replace_value(c, &v[0], &c->nodes[e->arg[1]], &c->nodes[e->arg[2]]);
        case FN_SPLIT_PART:
            return split_value(c, &v[0], &v[1], &v[2]);
        case FN_ABS:
            if (!as_number(&v[0])) return no_value();
            if (!v[0].exact || v[0].mant == INT64_MIN) return double_value(fabs(to_double(&v[0])));
            return exact_value(v[0].mant < 0 ? -(hp_wide_t)v[0].mant : v[0].mant, v[0].scale);
        case FN_ROUND: {
            int64_t digits = 0;
            if (e->argc > 1 && !as_integer(&v[1], &digits)) return no_value();
            return round_value(v[0], digits, FN_ROUND);
        }
        case FN_FLOOR:
        case FN_CEIL:
            return round_value(v[0], 0, (fn_t)e->fn);
        case FN_TIME: {
            int64_t us;
            if (!has) return no_value();
            hp_str_t t = text_of(c, &v[0]);
            if (t.is_json || !hp_time_parse(t.ptr, t.len, &us)) return no_value();
            char *out = arena_at(c);
            size_t n = hp_time_format(us, (hp_time_unit_t)e->unit, out);
            c->arena_len += n;
            return text_value(out, n);
        }
    }
    return no_value();
}

static value_t eval(hp_compute_t *c, const hp_expr_t *e, const hp_row_t *row) {
    switch ((expr_kind_t)e->kind) {
        case EXPR_NUMBER: {
            value_t v = e->exact ? exact_value(e->mant, e->scale) : double_value(e->f);
            v.s.ptr = c->text + e->text;
            v.s.len = e->text_len;
            return v;
        }
        case EXPR_TEXT:
            return text_value(c->text + e->text, e->text_len);
        case EXPR_COLUMN:
        case EXPR_PATH: {
            if ((size_t)e->col >= row->field_count || !row->fields[e->col].present) {
                return no_value();
            }
            value_t v = { .s = row->fields[e->col] };
            if (e->kind == EXPR_COLUMN) return v;
            char *arena = arena_at(c);
            if (!hp_json_lookup(v.s.ptr, v.s.len, &c->steps[e->step], e->step_count, &v.s, arena)) {
                return no_value();
            }
            if (v.s.ptr == arena) c->arena_len += v.s.len;
            return v;
        }
        case EXPR_NEG: {
            value_t v = eval(c, &c->nodes[e->arg[0]], row);
            if (!as_number(&v)) return no_value();
            if (v.exact && v.mant != INT64_MIN) return exact_value(-(hp_wide_t)v.mant, v.scale);
            return double_value(-to_double(&v));
        }
        case EXPR_CALL:
            return call(c, e, row);
        default: {
            value_t a = eval(c, &c->nodes[e->arg[0]], row);
            value_t b = eval(c, &c->nodes[e->arg[1]], row);
            return arith((expr_kind_t)e->kind, a, b);
        }
    }
}

hp_error_t hp_compute_begin(hp_compute_t *c, hp_row_t *row, size_t raw_len, bool cut) {
    if (cut && c->appends && row->field_count > c->width) row->field_count = c->width;
    size_t need = add_sat(mul_sat(c->arena_need.per_byte, raw_len), c->arena_need.fixed);
    if (need > c->arena_cap || !c->arena) {
        if (need > SIZE_MAX / 2) return HP_ERR_NOMEM;
        size_t cap = need + need / 2 + 64;
        char *arena = (char *)realloc(c->arena, cap);
        if (!arena) return HP_ERR_NOMEM;
        c->arena = arena;
        c->arena_cap = cap;
    }
    c->arena_len = 0;
    return HP_OK;
}

HP_HOT size_t hp_compute_row(hp_compute_t *c, hp_row_t *row, size_t from, size_t to) {
    size_t bytes = 0;
    for (size_t i = from; i < to; i++) {
        const hp_computed_t *col = &c->cols[i];
        value_t v = eval(c, &c->nodes[col->root], row);
        hp_str_t f = text_of(c, &v);
        if (!f.present) f = (hp_str_t){ .ptr = "", .len = 0 };
        f.has_escapes |= f.is_json;   /* JSON text may hold the delimiter */

        const size_t index = (size_t)col->index;
        for (size_t k = row->field_count; k < index; k++) row->fields[k] = (hp_str_t){ .ptr = "" };
        if (index >= row->field_count) row->field_count = index + 1;
        row->fields[index] = f;
        bytes += f.len;
    }
    return bytes;
}
