#include "hyperpipe/hp_filter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_projection(void) {
    hp_header_t hdr;
    hdr.count = 4;
    hdr.names[0] = strdup("id");
    hdr.names[1] = strdup("name");
    hdr.names[2] = strdup("score");
    hdr.names[3] = strdup("city");

    hp_projection_t proj;
    hp_error_t err = hp_projection_init(&proj, "name,score");
    assert(err == HP_OK);
    assert(proj.count == 2);
    assert(proj.active == true);

    err = hp_projection_bind(&proj, &hdr);
    assert(err == HP_OK);
    assert(proj.col_indices[0] == 1);
    assert(proj.col_indices[1] == 2);

    hp_row_t src_row;
    src_row.field_count = 4;
    src_row.fields[0] = (hp_str_t){ .ptr = "101", .len = 3, .has_escapes = false };
    src_row.fields[1] = (hp_str_t){ .ptr = "Alice", .len = 5, .has_escapes = false };
    src_row.fields[2] = (hp_str_t){ .ptr = "98.5", .len = 4, .has_escapes = false };
    src_row.fields[3] = (hp_str_t){ .ptr = "London", .len = 6, .has_escapes = false };

    hp_row_t dst_row;
    err = hp_project_row(&proj, &src_row, &dst_row);
    assert(err == HP_OK);
    assert(dst_row.field_count == 2);
    assert(dst_row.fields[0].len == 5 && strncmp(dst_row.fields[0].ptr, "Alice", 5) == 0);
    assert(dst_row.fields[1].len == 4 && strncmp(dst_row.fields[1].ptr, "98.5", 4) == 0);

    hp_header_t dst_hdr;
    err = hp_project_header(&proj, &hdr, &dst_hdr);
    assert(err == HP_OK);
    assert(dst_hdr.count == 2);
    assert(strcmp(dst_hdr.names[0], "name") == 0);
    assert(strcmp(dst_hdr.names[1], "score") == 0);

    hp_header_free(&hdr);
    hp_header_free(&dst_hdr);
}

static void test_filtering(void) {
    hp_header_t hdr;
    hdr.count = 3;
    hdr.names[0] = strdup("name");
    hdr.names[1] = strdup("age");
    hdr.names[2] = strdup("status");

    hp_filter_t filter;
    hp_error_t err = hp_filter_init(&filter, "age >= 21, status == active");
    assert(err == HP_OK);
    assert(filter.count == 2);

    err = hp_filter_bind(&filter, &hdr);
    assert(err == HP_OK);
    assert(filter.preds[0].col_index == 1);
    assert(filter.preds[1].col_index == 2);

    /* Matching row */
    hp_row_t row1;
    row1.field_count = 3;
    row1.fields[0] = (hp_str_t){ .ptr = "Bob", .len = 3, .has_escapes = false };
    row1.fields[1] = (hp_str_t){ .ptr = "25", .len = 2, .has_escapes = false };
    row1.fields[2] = (hp_str_t){ .ptr = "active", .len = 6, .has_escapes = false };
    assert(hp_filter_matches(&filter, &row1) == true);

    /* Underage row -> fails */
    hp_row_t row2;
    row2.field_count = 3;
    row2.fields[0] = (hp_str_t){ .ptr = "Charlie", .len = 7, .has_escapes = false };
    row2.fields[1] = (hp_str_t){ .ptr = "19", .len = 2, .has_escapes = false };
    row2.fields[2] = (hp_str_t){ .ptr = "active", .len = 6, .has_escapes = false };
    assert(hp_filter_matches(&filter, &row2) == false);

    /* Non-active row -> fails */
    hp_row_t row3;
    row3.field_count = 3;
    row3.fields[0] = (hp_str_t){ .ptr = "David", .len = 5, .has_escapes = false };
    row3.fields[1] = (hp_str_t){ .ptr = "30", .len = 2, .has_escapes = false };
    row3.fields[2] = (hp_str_t){ .ptr = "inactive", .len = 8, .has_escapes = false };
    assert(hp_filter_matches(&filter, &row3) == false);

    hp_header_free(&hdr);
}

int main(void) {
    printf("[test_filter] Running projection and filter tests...\n");
    test_projection();
    test_filtering();
    printf("[test_filter] All filter and projection tests passed successfully.\n");
    return 0;
}
