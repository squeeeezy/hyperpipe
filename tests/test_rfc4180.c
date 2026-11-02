#include "hyperpipe/hp_csv_parser.h"
#include "tap.h"
#include <stdio.h>
#include <string.h>

static void test_simple_csv(void) {
    hp_csv_parser_t parser;
    memset(&parser, 0, sizeof(parser)); /* zero state machine + arena cursor */
    parser.delim = ',';
    parser.quote = '"';
    parser.scan_special = hp_csv_scan_special_scalar;
    parser.scan_quote = hp_csv_scan_quote_scalar;

    const char *csv = "name,age,city\nAlice,30,New York\nBob,25,London\n";
    size_t len = strlen(csv);
    size_t offset = 0;
    hp_row_t row;
    size_t consumed = 0;

    /* Header */
    hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)csv + offset, len - offset, &consumed, &row, false);
    CHECK(err == HP_OK);
    CHECK(consumed > 0);
    CHECK(row.field_count == 3);
    CHECK(strncmp(row.fields[0].ptr, "name", row.fields[0].len) == 0);
    CHECK(strncmp(row.fields[1].ptr, "age", row.fields[1].len) == 0);
    CHECK(strncmp(row.fields[2].ptr, "city", row.fields[2].len) == 0);
    offset += consumed;

    /* Row 1 */
    err = hp_csv_parse_row(&parser, (const uint8_t *)csv + offset, len - offset, &consumed, &row, false);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(strncmp(row.fields[0].ptr, "Alice", row.fields[0].len) == 0);
    CHECK(strncmp(row.fields[1].ptr, "30", row.fields[1].len) == 0);
    CHECK(strncmp(row.fields[2].ptr, "New York", row.fields[2].len) == 0);
    offset += consumed;

    /* Row 2 */
    err = hp_csv_parse_row(&parser, (const uint8_t *)csv + offset, len - offset, &consumed, &row, true);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(strncmp(row.fields[0].ptr, "Bob", row.fields[0].len) == 0);
    CHECK(strncmp(row.fields[1].ptr, "25", row.fields[1].len) == 0);
    CHECK(strncmp(row.fields[2].ptr, "London", row.fields[2].len) == 0);
    offset += consumed;
    hp_csv_parser_free(&parser);
}

static void test_rfc4180_quotes_and_escapes(void) {
    hp_csv_parser_t parser;
    memset(&parser, 0, sizeof(parser)); /* zero state machine + arena cursor */
    parser.delim = ',';
    parser.quote = '"';
    parser.scan_special = hp_csv_scan_special_scalar;
    parser.scan_quote = hp_csv_scan_quote_scalar;

    /* Field with commas inside quotes, and escaped quotes: "Hello, ""World""" */
    const char *csv = "\"Hello, \"\"World\"\"\",123,\"multi\nline\nfield\"\r\n";
    size_t len = strlen(csv);
    hp_row_t row;
    size_t consumed = 0;

    hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)csv, len, &consumed, &row, false);
    CHECK(err == HP_OK);
    CHECK(consumed == len);
    CHECK(row.field_count == 3);

    /* Field 0 unescaped should be: Hello, "World" */
    CHECK(row.fields[0].has_escapes == true);
    CHECK(row.fields[0].len == strlen("Hello, \"World\""));
    CHECK(strncmp(row.fields[0].ptr, "Hello, \"World\"", row.fields[0].len) == 0);

    /* Field 1: 123 */
    CHECK(row.fields[1].len == 3);
    CHECK(strncmp(row.fields[1].ptr, "123", 3) == 0);

    /* Field 2: multi\nline\nfield */
    CHECK(row.fields[2].len == strlen("multi\nline\nfield"));
    CHECK(strncmp(row.fields[2].ptr, "multi\nline\nfield", row.fields[2].len) == 0);
    hp_csv_parser_free(&parser);
}

static void test_empty_fields_and_no_trailing_newline(void) {
    hp_csv_parser_t parser;
    memset(&parser, 0, sizeof(parser)); /* zero state machine + arena cursor */
    parser.delim = ',';
    parser.quote = '"';
    parser.scan_special = hp_csv_scan_special_scalar;
    parser.scan_quote = hp_csv_scan_quote_scalar;

    const char *csv = ",,\r\na,,c";
    size_t len = strlen(csv);
    size_t offset = 0;
    hp_row_t row;
    size_t consumed = 0;

    /* Row 1: 3 empty fields */
    hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)csv + offset, len - offset, &consumed, &row, false);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(row.fields[0].len == 0);
    CHECK(row.fields[1].len == 0);
    CHECK(row.fields[2].len == 0);
    offset += consumed;

    /* Row 2 at EOF with no trailing newline */
    err = hp_csv_parse_row(&parser, (const uint8_t *)csv + offset, len - offset, &consumed, &row, true);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(row.fields[0].len == 1 && row.fields[0].ptr[0] == 'a');
    CHECK(row.fields[1].len == 0);
    CHECK(row.fields[2].len == 1 && row.fields[2].ptr[0] == 'c');
    hp_csv_parser_free(&parser);
}

static void test_tsv_parsing(void) {
    hp_csv_parser_t parser;
    parser.delim = '\t';
    parser.quote = '"';
    parser.scan_special = hp_csv_scan_special_scalar;
    parser.scan_quote = hp_csv_scan_quote_scalar;

    const char *tsv = "colA\tcolB\tcolC\r\nval1\tval2\tval3\n";
    size_t len = strlen(tsv);
    size_t offset = 0;
    hp_row_t row;
    size_t consumed = 0;

    hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)tsv + offset, len - offset, &consumed, &row, false);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(strncmp(row.fields[0].ptr, "colA", 4) == 0);
    CHECK(strncmp(row.fields[1].ptr, "colB", 4) == 0);
    CHECK(strncmp(row.fields[2].ptr, "colC", 4) == 0);
    offset += consumed;

    err = hp_csv_parse_row(&parser, (const uint8_t *)tsv + offset, len - offset, &consumed, &row, true);
    CHECK(err == HP_OK);
    CHECK(row.field_count == 3);
    CHECK(strncmp(row.fields[0].ptr, "val1", 4) == 0);
    CHECK(strncmp(row.fields[1].ptr, "val2", 4) == 0);
    CHECK(strncmp(row.fields[2].ptr, "val3", 4) == 0);
    hp_csv_parser_free(&parser);
}

static void test_malformed_quote_errors(void) {
    hp_csv_parser_t parser;
    memset(&parser, 0, sizeof(parser)); /* zero state machine + arena cursor */
    parser.delim = ',';
    parser.quote = '"';
    parser.scan_special = hp_csv_scan_special_scalar;
    parser.scan_quote = hp_csv_scan_quote_scalar;

    /* Stray quote in unquoted field */
    const char *csv1 = "foo,bar\"baz,123\n";
    hp_row_t row;
    size_t consumed = 0;
    hp_error_t err = hp_csv_parse_row(&parser, (const uint8_t *)csv1, strlen(csv1), &consumed, &row, true);
    CHECK(err == HP_ERR_PARSE_QUOTE);

    /* Unclosed quote at EOF */
    const char *csv2 = "foo,\"unclosed quote at eof";
    err = hp_csv_parse_row(&parser, (const uint8_t *)csv2, strlen(csv2), &consumed, &row, true);
    CHECK(err == HP_ERR_PARSE_QUOTE);
    hp_csv_parser_free(&parser);
}

int main(void) {
    TAP_RUN(test_simple_csv);
    TAP_RUN(test_rfc4180_quotes_and_escapes);
    TAP_RUN(test_empty_fields_and_no_trailing_newline);
    TAP_RUN(test_tsv_parsing);
    TAP_RUN(test_malformed_quote_errors);
    return tap_done();
}
