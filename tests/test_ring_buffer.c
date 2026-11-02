#include "hyperpipe/hp_ring_buffer.h"
#include "tap.h"
#include <stdio.h>
#include <string.h>

static void test_basic_alloc(void) {
    hp_sliding_buffer_t buf;
    hp_error_t err = hp_sliding_buffer_init(&buf, 4096);
    CHECK(err == HP_OK);
    CHECK(buf.capacity == 4096);
    CHECK(buf.head == 0);
    CHECK(buf.tail == 0);
    CHECK(buf.data != NULL);

    size_t len = 999;
    const uint8_t *ptr = hp_sliding_buffer_read_ptr(&buf, &len);
    CHECK(ptr == buf.data);
    CHECK(len == 0);

    hp_sliding_buffer_free(&buf);
    CHECK(buf.data == NULL);
}

static void test_feed_and_consume(void) {
    hp_sliding_buffer_t buf;
    hp_error_t err = hp_sliding_buffer_init(&buf, 1024);
    CHECK(err == HP_OK);

    const char *msg = "The quick brown fox jumps over the lazy dog";
    size_t msg_len = strlen(msg);
    size_t fed = 0;

    err = hp_sliding_buffer_feed(&buf, (const uint8_t *)msg, msg_len, &fed);
    CHECK(err == HP_OK);
    CHECK(fed == msg_len);

    size_t active = 0;
    const uint8_t *ptr = hp_sliding_buffer_read_ptr(&buf, &active);
    CHECK(active == msg_len);
    CHECK(memcmp(ptr, msg, msg_len) == 0);

    /* Consume half */
    hp_sliding_buffer_consume(&buf, 10);
    ptr = hp_sliding_buffer_read_ptr(&buf, &active);
    CHECK(active == msg_len - 10);
    CHECK(memcmp(ptr, msg + 10, active) == 0);

    /* Feed more to trigger slide back */
    const char *msg2 = " - repeated sequence";
    size_t msg2_len = strlen(msg2);
    err = hp_sliding_buffer_feed(&buf, (const uint8_t *)msg2, msg2_len, &fed);
    CHECK(err == HP_OK);
    CHECK(fed == msg2_len);

    ptr = hp_sliding_buffer_read_ptr(&buf, &active);
    CHECK(buf.head == 0); /* Slid back to origin */
    CHECK(active == (msg_len - 10) + msg2_len);

    hp_sliding_buffer_free(&buf);
}

static void test_boundary_wrap_stress(void) {
    /* Test with an artificially tiny 1024 byte buffer */
    hp_sliding_buffer_t buf;
    hp_error_t err = hp_sliding_buffer_init(&buf, 1024);
    CHECK(err == HP_OK);

    char pattern[500];
    for (int i = 0; i < 500; i++) pattern[i] = (char)('A' + (i % 26));

    for (int iter = 0; iter < 1000; iter++) {
        size_t fed = 0;
        err = hp_sliding_buffer_feed(&buf, (const uint8_t *)pattern, 500, &fed);
        CHECK(err == HP_OK);
        CHECK(fed == 500);

        /* Consume 400 bytes */
        hp_sliding_buffer_consume(&buf, 400);

        /* Feed 400 more bytes */
        err = hp_sliding_buffer_feed(&buf, (const uint8_t *)pattern, 400, &fed);
        CHECK(err == HP_OK);
        CHECK(fed == 400);

        /* Consume remainder */
        hp_sliding_buffer_consume(&buf, 500);
    }

    hp_sliding_buffer_free(&buf);
}

int main(void) {
    TAP_RUN(test_basic_alloc);
    TAP_RUN(test_feed_and_consume);
    TAP_RUN(test_boundary_wrap_stress);
    return tap_done();
}
