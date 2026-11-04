/* JSON text: escapes, strings and their decoding, shared by the NDJSON
 * parser and paths into values. Internal to src/. */

#ifndef HYPERPIPE_SRC_HP_JSON_TEXT_H
#define HYPERPIPE_SRC_HP_JSON_TEXT_H

#include <stdbool.h>
#include <stddef.h>

/* Parses the 4 hex digits of a \uXXXX escape; -1 on malformed input. */
static inline int json_hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else return -1;
    }
    return v;
}

/* Encode codepoint as UTF-8 into dst (capacity avail). Returns bytes written,
 * 0 when the buffer is too small. */
static inline size_t json_utf8_encode(char *dst, size_t avail, unsigned cp) {
    size_t n = (cp <= 0x7F) ? 1 : (cp <= 0x7FF) ? 2 : (cp <= 0xFFFF) ? 3 : 4;
    if (n > avail) return 0;
    switch (n) {
        case 1: dst[0] = (char)cp; break;
        case 2: dst[0] = (char)(0xC0 | (cp >> 6));
                dst[1] = (char)(0x80 | (cp & 0x3F)); break;
        case 3: dst[0] = (char)(0xE0 | (cp >> 12));
                dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                dst[2] = (char)(0x80 | (cp & 0x3F)); break;
        default: dst[0] = (char)(0xF0 | (cp >> 18));
                 dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                 dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                 dst[3] = (char)(0x80 | (cp & 0x3F)); break;
    }
    return n;
}

/* JSON allows exactly four whitespace characters (RFC 8259 section 2). */
static inline bool json_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Length of the escape sequence starting at the backslash p[0]: 2, or 6
 * for \uXXXX; 0 when it is not one of the escapes RFC 8259 defines. */
static inline size_t json_escape_len(const char *p, size_t avail) {
    if (avail < 2) return 0;
    switch (p[1]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
            return 2;
        case 'u':
            return (avail >= 6 && json_hex4(p + 2) >= 0) ? 6 : 0;
        default:
            return 0;
    }
}

/* The character a two-byte escape stands for (\" \\ \/ map to themselves) */
static inline char json_unescape(char esc) {
    switch (esc) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'b': return '\b';
        case 'f': return '\f';
        default:  return esc;
    }
}

/* Length of the JSON string starting at the quote p[0], closing quote
 * included; 0 when it is unterminated or holds an invalid escape */
static inline size_t json_string_span(const char *p, size_t len) {
    size_t i = 1;
    while (i < len) {
        if (p[i] == '"') return i + 1;
        if (p[i] == '\\') {
            size_t esc = json_escape_len(p + i, len - i);
            if (esc == 0) return 0;
            i += esc;
        } else {
            i++;
        }
    }
    return 0;
}

/* Decodes the JSON string at p, which starts past its opening quote, into
 * out. Returns the bytes up to and including the closing quote, or 0 when
 * the string is unterminated or holds an invalid escape; *out_len is the
 * decoded length, never more than the bytes before the quote. */
static inline size_t json_decode_string(const char *p, size_t len, char *out, size_t *out_len) {
    size_t i = 0, w = 0;
    while (i < len && p[i] != '"') {
        if (p[i] != '\\') {
            out[w++] = p[i++];
            continue;
        }
        size_t esc = json_escape_len(p + i, len - i);
        if (esc == 0) return 0;
        if (esc == 2) {
            out[w++] = json_unescape(p[i + 1]);
            i += 2;
            continue;
        }

        /* \uXXXX: a surrogate pair combines into one code point, a lone
         * surrogate becomes U+FFFD */
        unsigned cp = (unsigned)json_hex4(p + i + 2);
        i += 6;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            int lo = (len - i >= 6 && p[i] == '\\' && p[i + 1] == 'u') ? json_hex4(p + i + 2) : -1;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (unsigned)(lo - 0xDC00);
                i += 6;
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFDu;
        }
        w += json_utf8_encode(out + w, 4, cp);
    }
    if (i >= len) return 0;
    *out_len = w;
    return i + 1;
}

#endif /* HYPERPIPE_SRC_HP_JSON_TEXT_H */
