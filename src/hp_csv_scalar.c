#include "hyperpipe/hp_csv_parser.h"

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

hp_error_t hp_csv_parse_row(hp_csv_parser_t *parser,
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

    /* Skip blank lines (e.g. extra \r\n) at beginning of record if any */
    while (pos < len && (src[pos] == '\r' || src[pos] == '\n')) {
        pos++;
    }
    if (pos >= len) {
        *consumed = pos;
        return HP_OK; /* No more records or empty buffer */
    }

    size_t field_start = pos;
    bool field_is_quoted = false;
    bool field_has_escapes = false;
    size_t arena_field_start = parser->arena_pos;

    while (pos < len) {
        if (!field_is_quoted) {
            /* Check if field starts with a quote */
            if (pos == field_start && src[pos] == '"') {
                field_is_quoted = true;
                pos++;
                field_start = pos;
                arena_field_start = parser->arena_pos;
                continue;
            }

            /* Fast scan for special chars using vectorized or scalar scanner */
            size_t skip = parser->scan_special(src + pos, len - pos, delim);
            pos += skip;

            if (pos >= len) {
                if (!is_eof) {
                    /* Incomplete unquoted field at end of chunk -> request refill */
                    *consumed = 0;
                    return HP_OK;
                }
                /* At EOF, unquoted field finishes at EOF */
                break;
            }

            uint8_t c = src[pos];
            if (c == delim) {
                /* Field ends */
                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
                out_row->fields[out_row->field_count].len = pos - field_start;
                out_row->fields[out_row->field_count].has_escapes = false;
                out_row->field_count++;

                pos++; /* skip delimiter */
                field_start = pos;
                field_is_quoted = false;
                field_has_escapes = false;
            } else if (c == '\r' || c == '\n') {
                /* End of row reached */
                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
                out_row->fields[out_row->field_count].len = pos - field_start;
                out_row->fields[out_row->field_count].has_escapes = false;
                out_row->field_count++;

                /* Skip CRLF or LF */
                if (c == '\r' && (pos + 1 < len) && src[pos + 1] == '\n') {
                    pos += 2;
                } else {
                    pos += 1;
                }
                *consumed = pos;
                return HP_OK;
            } else if (c == '"') {
                /* Quote in middle of unquoted field -> RFC 4180 strict parse error */
                return HP_ERR_PARSE_QUOTE;
            }
        } else {
            /* Inside quoted field: scan for next quote */
            size_t skip = parser->scan_quote(src + pos, len - pos);
            if (field_has_escapes) {
                /* Copy unescaped chunk to arena */
                if (parser->arena_pos + skip >= HP_MAX_ROW_BYTES) return HP_ERR_BUFFER_OVERFLOW;
                memcpy(parser->unescape_arena + parser->arena_pos, src + pos, skip);
                parser->arena_pos += skip;
            }
            pos += skip;

            if (pos >= len) {
                if (!is_eof) {
                    /* Incomplete quoted field -> request refill */
                    *consumed = 0;
                    return HP_OK;
                }
                /* EOF reached while inside quotes -> syntax error */
                return HP_ERR_PARSE_QUOTE;
            }

            /* src[pos] is a quote */
            if (pos + 1 < len && src[pos + 1] == '"') {
                /* Escaped quote '""' */
                if (!field_has_escapes) {
                    /* First escape encountered: copy accumulated raw prefix into arena */
                    field_has_escapes = true;
                    size_t raw_len = pos - field_start;
                    if (parser->arena_pos + raw_len + 1 >= HP_MAX_ROW_BYTES) return HP_ERR_BUFFER_OVERFLOW;
                    memcpy(parser->unescape_arena + parser->arena_pos, src + field_start, raw_len);
                    parser->arena_pos += raw_len;
                }
                /* Append single quote */
                parser->unescape_arena[parser->arena_pos++] = '"';
                pos += 2; /* skip both quotes */
            } else {
                /* Closing quote */
                size_t field_len;
                const char *field_ptr;
                if (field_has_escapes) {
                    field_len = parser->arena_pos - arena_field_start;
                    field_ptr = parser->unescape_arena + arena_field_start;
                } else {
                    field_len = pos - field_start;
                    field_ptr = (const char *)(src + field_start);
                }

                if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
                out_row->fields[out_row->field_count].ptr = field_ptr;
                out_row->fields[out_row->field_count].len = field_len;
                out_row->fields[out_row->field_count].has_escapes = field_has_escapes;
                out_row->field_count++;

                pos++; /* skip closing quote */

                /* Must be followed by delimiter, newline, or EOF */
                if (pos < len) {
                    if (src[pos] == delim) {
                        pos++;
                        field_start = pos;
                        field_is_quoted = false;
                        field_has_escapes = false;
                        arena_field_start = parser->arena_pos;
                    } else if (src[pos] == '\r' || src[pos] == '\n') {
                        if (src[pos] == '\r' && pos + 1 < len && src[pos + 1] == '\n') {
                            pos += 2;
                        } else {
                            pos += 1;
                        }
                        *consumed = pos;
                        return HP_OK;
                    } else {
                        /* Illegal character immediately after closing quote */
                        return HP_ERR_PARSE_QUOTE;
                    }
                } else {
                    /* EOF reached right after closing quote */
                    if (is_eof) {
                        *consumed = pos;
                        return HP_OK;
                    } else {
                        *consumed = 0;
                        return HP_OK;
                    }
                }
            }
        }
    }

    /* Trailing record at EOF without ending newline */
    if (is_eof && (pos > field_start || field_is_quoted || out_row->field_count > 0)) {
        if (!field_is_quoted) {
            if (out_row->field_count >= HP_MAX_COLUMNS) return HP_ERR_BUFFER_OVERFLOW;
            out_row->fields[out_row->field_count].ptr = (const char *)(src + field_start);
            out_row->fields[out_row->field_count].len = pos - field_start;
            out_row->fields[out_row->field_count].has_escapes = false;
            out_row->field_count++;
        }
        *consumed = pos;
        return HP_OK;
    }

    *consumed = pos;
    return HP_OK;
}
