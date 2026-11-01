#include "hyperpipe/hp_json_ser.h"
#include <ctype.h>

size_t hp_json_scan_escapes_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == '"' || c == '\\' || c < 0x20) {
            return i;
        }
        i++;
    }
    return len;
}

static hp_error_t hp_json_ser_grow(hp_json_ser_t *ser, size_t needed) {
    if (ser->pos + needed <= ser->capacity) return HP_OK;
    size_t new_cap = (ser->capacity == 0) ? 4096 : ser->capacity * 2;
    while (ser->pos + needed > new_cap) {
        new_cap *= 2;
    }
    char *new_buf = (char *)realloc(ser->out_buf, new_cap);
    if (!new_buf) return HP_ERR_NOMEM;
    ser->out_buf = new_buf;
    ser->capacity = new_cap;
    return HP_OK;
}

hp_error_t hp_json_ser_init(hp_json_ser_t *ser, size_t initial_capacity, hp_simd_mode_t simd) {
    (void)simd;
    if (!ser) return HP_ERR_INVALID_ARG;
    if (initial_capacity < 1024) initial_capacity = 1024;
    ser->out_buf = (char *)malloc(initial_capacity);
    if (!ser->out_buf) return HP_ERR_NOMEM;
    ser->capacity = initial_capacity;
    ser->pos = 0;
    ser->scan_escapes = hp_json_scan_escapes_scalar;
    return HP_OK;
}

void hp_json_ser_free(hp_json_ser_t *ser) {
    if (!ser) return;
    if (ser->out_buf) {
        free(ser->out_buf);
        ser->out_buf = NULL;
    }
    ser->capacity = 0;
    ser->pos = 0;
}

void hp_json_ser_reset(hp_json_ser_t *ser) {
    if (ser) ser->pos = 0;
}

hp_error_t hp_json_ser_write_string(hp_json_ser_t *ser, const char *str, size_t len) {
    if (!ser || !str) return HP_ERR_INVALID_ARG;

    /* Reserve space for opening and closing quote */
    hp_error_t err = hp_json_ser_grow(ser, len * 6 + 2);
    if (err != HP_OK) return err;

    ser->out_buf[ser->pos++] = '"';

    size_t offset = 0;
    while (offset < len) {
        size_t safe_span = ser->scan_escapes((const uint8_t *)str + offset, len - offset);
        if (safe_span > 0) {
            memcpy(ser->out_buf + ser->pos, str + offset, safe_span);
            ser->pos += safe_span;
            offset += safe_span;
        }

        if (offset < len) {
            uint8_t c = (uint8_t)str[offset];
            switch (c) {
                case '"':  ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = '"'; break;
                case '\\': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = '\\'; break;
                case '\b': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = 'b'; break;
                case '\f': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = 'f'; break;
                case '\n': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = 'n'; break;
                case '\r': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = 'r'; break;
                case '\t': ser->out_buf[ser->pos++] = '\\'; ser->out_buf[ser->pos++] = 't'; break;
                default: {
                    static const char hex[] = "0123456789abcdef";
                    ser->out_buf[ser->pos++] = '\\';
                    ser->out_buf[ser->pos++] = 'u';
                    ser->out_buf[ser->pos++] = '0';
                    ser->out_buf[ser->pos++] = '0';
                    ser->out_buf[ser->pos++] = hex[(c >> 4) & 0x0F];
                    ser->out_buf[ser->pos++] = hex[c & 0x0F];
                    break;
                }
            }
            offset++;
        }
    }

    ser->out_buf[ser->pos++] = '"';
    return HP_OK;
}

static bool is_valid_json_number(const char *s, size_t len) {
    if (len == 0) return false;
    size_t i = 0;
    if (s[0] == '-') i++;
    if (i >= len) return false;

    /* Fast path for unsigned and signed integers */
    size_t digits_start = i;
    while (i < len && (uint8_t)(s[i] - '0') <= 9) i++;
    if (i == len && i > digits_start) {
        if (s[digits_start] == '0' && (len - digits_start) > 1) return false;
        return true;
    }

    /* Reset and check full floating-point specification */
    i = (s[0] == '-') ? 1 : 0;
    if (s[i] == '0') {
        i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
        while (i < len && isdigit((unsigned char)s[i])) i++;
    } else {
        return false;
    }

    if (i < len && s[i] == '.') {
        i++;
        if (i >= len || !isdigit((unsigned char)s[i])) return false;
        while (i < len && isdigit((unsigned char)s[i])) i++;
    }

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= len || !isdigit((unsigned char)s[i])) return false;
        while (i < len && isdigit((unsigned char)s[i])) i++;
    }

    return i == len;
}

hp_error_t hp_json_ser_row(hp_json_ser_t *ser,
                           const hp_header_t *header,
                           const hp_row_t *row,
                           const char **out_line,
                           size_t *out_line_len) {
    if (!ser || !row) return HP_ERR_INVALID_ARG;
    hp_json_ser_reset(ser);

    /* Pre-calculate upper bound size to grow buffer once per row */
    size_t est_size = 32;
    size_t count = row->field_count;
    for (size_t i = 0; i < count; i++) {
        est_size += row->fields[i].len * 2 + 32;
    }
    hp_error_t err = hp_json_ser_grow(ser, est_size);
    if (err != HP_OK) return err;

    ser->out_buf[ser->pos++] = '{';

    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            ser->out_buf[ser->pos++] = ',';
        }

        /* Write key */
        const char *key = (header && i < header->count) ? header->names[i] : NULL;
        char default_key[32];
        if (!key) {
            snprintf(default_key, sizeof(default_key), "col_%zu", i);
            key = default_key;
        }

        /* Fast key emit */
        ser->out_buf[ser->pos++] = '"';
        size_t key_len = strlen(key);
        memcpy(ser->out_buf + ser->pos, key, key_len);
        ser->pos += key_len;
        ser->out_buf[ser->pos++] = '"';
        ser->out_buf[ser->pos++] = ':';

        /* Write value */
        const hp_str_t *f = &row->fields[i];
        if (!f->has_escapes && is_valid_json_number(f->ptr, f->len)) {
            memcpy(ser->out_buf + ser->pos, f->ptr, f->len);
            ser->pos += f->len;
        } else if (!f->has_escapes && f->len == 4 && memcmp(f->ptr, "true", 4) == 0) {
            memcpy(ser->out_buf + ser->pos, "true", 4);
            ser->pos += 4;
        } else if (!f->has_escapes && f->len == 5 && memcmp(f->ptr, "false", 5) == 0) {
            memcpy(ser->out_buf + ser->pos, "false", 5);
            ser->pos += 5;
        } else if (!f->has_escapes && f->len == 4 && memcmp(f->ptr, "null", 4) == 0) {
            memcpy(ser->out_buf + ser->pos, "null", 4);
            ser->pos += 4;
        } else {
            err = hp_json_ser_write_string(ser, f->ptr, f->len);
            if (err != HP_OK) return err;
        }
    }

    ser->out_buf[ser->pos++] = '}';
    ser->out_buf[ser->pos++] = '\n';

    if (out_line) *out_line = ser->out_buf;
    if (out_line_len) *out_line_len = ser->pos;
    return HP_OK;
}

hp_error_t hp_json_parse_line(const char *line,
                              size_t len,
                              hp_header_t *header,
                              hp_row_t *out_row,
                              char *value_arena,
                              size_t arena_size) {
    if (!line || !out_row) return HP_ERR_INVALID_ARG;
    out_row->field_count = 0;

    size_t pos = 0;
    while (pos < len && isspace((unsigned char)line[pos])) pos++;
    if (pos >= len || line[pos] != '{') return HP_ERR_PARSE_QUOTE;
    pos++;

    size_t arena_pos = 0;

    while (pos < len) {
        while (pos < len && isspace((unsigned char)line[pos])) pos++;
        if (pos < len && line[pos] == '}') break;

        /* Parse key */
        if (pos >= len || line[pos] != '"') return HP_ERR_PARSE_QUOTE;
        pos++;
        size_t key_start = pos;
        while (pos < len && line[pos] != '"') {
            if (line[pos] == '\\' && pos + 1 < len) pos += 2;
            else pos++;
        }
        if (pos >= len) return HP_ERR_PARSE_QUOTE;
        size_t key_len = pos - key_start;
        char key_buf[256];
        if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
        memcpy(key_buf, line + key_start, key_len);
        key_buf[key_len] = '\0';
        pos++; /* skip closing quote */

        while (pos < len && isspace((unsigned char)line[pos])) pos++;
        if (pos >= len || line[pos] != ':') return HP_ERR_PARSE_QUOTE;
        pos++; /* skip ':' */
        while (pos < len && isspace((unsigned char)line[pos])) pos++;

        /* Parse value */
        size_t val_start_arena = arena_pos;
        if (pos < len && line[pos] == '"') {
            /* String value */
            pos++;
            while (pos < len && line[pos] != '"') {
                if (line[pos] == '\\' && pos + 1 < len) {
                    char esc = line[pos + 1];
                    char unescaped = esc;
                    if (esc == 'n') unescaped = '\n';
                    else if (esc == 'r') unescaped = '\r';
                    else if (esc == 't') unescaped = '\t';
                    else if (esc == '"') unescaped = '"';
                    else if (esc == '\\') unescaped = '\\';
                    if (arena_pos + 1 < arena_size) value_arena[arena_pos++] = unescaped;
                    pos += 2;
                } else {
                    if (arena_pos + 1 < arena_size) value_arena[arena_pos++] = line[pos];
                    pos++;
                }
            }
            if (pos < len && line[pos] == '"') pos++;
        } else {
            /* Literal/number value */
            size_t lit_start = pos;
            while (pos < len && line[pos] != ',' && line[pos] != '}' && !isspace((unsigned char)line[pos])) {
                pos++;
            }
            size_t lit_len = pos - lit_start;
            if (arena_pos + lit_len < arena_size) {
                memcpy(value_arena + arena_pos, line + lit_start, lit_len);
                arena_pos += lit_len;
            }
        }

        /* Register field */
        if (out_row->field_count < HP_MAX_COLUMNS) {
            size_t idx = out_row->field_count++;
            out_row->fields[idx].ptr = value_arena + val_start_arena;
            out_row->fields[idx].len = arena_pos - val_start_arena;
            out_row->fields[idx].has_escapes = false;

            /* Register header column if header provided and new */
            if (header && idx >= header->count && header->count < HP_MAX_COLUMNS) {
                header->names[header->count++] = hp_strdup(key_buf);
            }
        }

        while (pos < len && isspace((unsigned char)line[pos])) pos++;
        if (pos < len && line[pos] == ',') {
            pos++;
        }
    }

    return HP_OK;
}
