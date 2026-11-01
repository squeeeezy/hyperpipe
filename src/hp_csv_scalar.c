#include "hyperpipe/hp_csv_parser.h"
#include "hyperpipe/hp_cpuid.h"

size_t hp_csv_scan_special_scalar(const uint8_t *p, size_t len, uint8_t delim) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == delim || c == '"' || c == '\n' || c == '\r') {
            return i;
        }
        i++;
    }
    return len;
}

size_t hp_csv_scan_quote_scalar(const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (p[i] == '"') {
            return i;
        }
        i++;
    }
    return len;
}

void hp_csv_parser_reset(hp_csv_parser_t *parser) {
    if (!parser) return;
    parser->in_quotes = false;
    parser->quote_just_closed = false;
    parser->arena_pos = 0;
}

hp_error_t hp_csv_parse_row_scalar(hp_csv_parser_t *parser,
                                   const uint8_t *src,
                                   size_t len,
                                   size_t *consumed,
                                   hp_row_t *out_row,
                                   bool is_eof) {
    if (!parser || !src || !consumed || !out_row) return HP_ERR_INVALID_ARG;

    *consumed = 0;
    out_row->field_count = 0;
    parser->arena_pos = 0;

    size_t pos = 0;
    const uint8_t delim = parser->delim;

    while (pos < len && (src[pos] == '\r' || src[pos] == '\n')) {
        pos++;
    }
    if (pos >= len) {
        *consumed = pos;
        return HP_OK;
    }

    size_t field_start = pos;
    bool field_is_quoted = false;
    bool field_has_escapes = false;

    while (pos < len) {
        if (!field_is_quoted) {
            if (pos == field_start && src[pos] == '"') {
                field_is_quoted = true;
                pos++;
                field_start = pos;
                continue;
            }

            size_t skip = parser->scan_special(src + pos, len - pos, delim);
            pos += skip;

            if (pos >= len) {
                if (!is_eof) {
                    *consumed = 0;
                    return HP_OK;
                }
                break;
            }

            uint8_t c = src[pos];
            if (c == delim) {
                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
                out_row->fields[out_row->field_count].len = pos - field_start;
                out_row->fields[out_row->field_count].has_escapes = false;
                out_row->field_count++;

                pos++;
                field_start = pos;
                field_is_quoted = false;
                field_has_escapes = false;
            } else if (c == '\r' || c == '\n') {
                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
                out_row->fields[out_row->field_count].len = pos - field_start;
                out_row->fields[out_row->field_count].has_escapes = false;
                out_row->field_count++;

                if (c == '\r' && (pos + 1 < len) && src[pos + 1] == '\n') {
                    pos += 2;
                } else {
                    pos += 1;
                }
                *consumed = pos;
                return HP_OK;
            } else if (c == '"') {
                return HP_ERR_PARSE_QUOTE;
            }
        } else {
            size_t skip = parser->scan_quote(src + pos, len - pos);
            if (field_has_escapes) {
                if (parser->arena_pos + skip >= HP_MAX_ROW_BYTES) return HP_ERR_BUFFER_OVERFLOW;
                memcpy(parser->unescape_arena + parser->arena_pos, src + pos, skip);
                parser->arena_pos += skip;
            }
            pos += skip;

            if (pos >= len) {
                if (!is_eof) {
                    *consumed = 0;
                    return HP_OK;
                }
                return HP_ERR_PARSE_QUOTE;
            }

            if (pos + 1 < len && src[pos + 1] == '"') {
                if (!field_has_escapes) {
                    field_has_escapes = true;
                    size_t raw_len = pos - field_start;
                    if (parser->arena_pos + raw_len + 1 >= HP_MAX_ROW_BYTES) return HP_ERR_BUFFER_OVERFLOW;
                    memcpy(parser->unescape_arena + parser->arena_pos, src + field_start, raw_len);
                    parser->arena_pos += raw_len;
                }
                if (parser->arena_pos + 1 >= HP_MAX_ROW_BYTES) return HP_ERR_BUFFER_OVERFLOW;
                parser->unescape_arena[parser->arena_pos++] = '"';
                pos += 2;
            } else {
                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                if (field_has_escapes) {
                    size_t flen = parser->arena_pos;
                    out_row->fields[out_row->field_count].ptr = parser->unescape_arena;
                    out_row->fields[out_row->field_count].len = flen;
                    out_row->fields[out_row->field_count].has_escapes = true;
                } else {
                    out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
                    out_row->fields[out_row->field_count].len = pos - field_start;
                    out_row->fields[out_row->field_count].has_escapes = false;
                }
                out_row->field_count++;

                pos++;
                while (pos < len && src[pos] != delim && src[pos] != '\r' && src[pos] != '\n') {
                    pos++;
                }

                if (pos < len) {
                    if (src[pos] == delim) {
                        pos++;
                        field_start = pos;
                        field_is_quoted = false;
                        field_has_escapes = false;
                    } else if (src[pos] == '\r' || src[pos] == '\n') {
                        if (src[pos] == '\r' && (pos + 1 < len) && src[pos + 1] == '\n') {
                            pos += 2;
                        } else {
                            pos += 1;
                        }
                        *consumed = pos;
                        return HP_OK;
                    }
                } else {
                    if (is_eof) {
                        *consumed = pos;
                        return HP_OK;
                    }
                    *consumed = 0;
                    return HP_OK;
                }
            }
        }
    }

    if (field_start < len || out_row->field_count > 0) {
        if (field_is_quoted && !is_eof) {
            *consumed = 0;
            return HP_OK;
        }
        if (out_row->field_count < HP_MAX_COLUMNS) {
            out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
            out_row->fields[out_row->field_count].len = len - field_start;
            out_row->fields[out_row->field_count].has_escapes = false;
            out_row->field_count++;
        }
    }

    *consumed = pos;
    return HP_OK;
}

hp_error_t hp_csv_parse_row(hp_csv_parser_t *parser,
                            const uint8_t *src,
                            size_t len,
                            size_t *consumed,
                            hp_row_t *out_row,
                            bool is_eof) {
    if (g_hp_dispatch.csv_parse_row) {
        return g_hp_dispatch.csv_parse_row(parser, src, len, consumed, out_row, is_eof);
    }
    return hp_csv_parse_row_scalar(parser, src, len, consumed, out_row, is_eof);
}
