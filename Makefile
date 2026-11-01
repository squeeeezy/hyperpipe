CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -Iinclude
AVX2_FLAGS = -mavx2 -mbmi2
AVX512_FLAGS = -mavx512f -mavx512bw -mavx512vl -mavx2 -mbmi2

BUILD_DIR = build
SRC_DIR = src
TEST_DIR = tests
BENCH_DIR = bench

CORE_OBJS = \
	$(BUILD_DIR)/hp_ring_buffer.o \
	$(BUILD_DIR)/hp_cpuid.o \
	$(BUILD_DIR)/hp_csv_scalar.o \
	$(BUILD_DIR)/hp_csv_avx2.o \
	$(BUILD_DIR)/hp_csv_avx512.o \
	$(BUILD_DIR)/hp_json_scalar.o \
	$(BUILD_DIR)/hp_json_avx2.o \
	$(BUILD_DIR)/hp_json_avx512.o \
	$(BUILD_DIR)/hp_filter.o \
	$(BUILD_DIR)/hp_transcoder.o

all: $(BUILD_DIR)/hyperpipe $(BUILD_DIR)/bench_hyperpipe

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/hp_ring_buffer.o: $(SRC_DIR)/hp_ring_buffer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_cpuid.o: $(SRC_DIR)/hp_cpuid.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_csv_scalar.o: $(SRC_DIR)/hp_csv_scalar.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_csv_avx2.o: $(SRC_DIR)/hp_csv_avx2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(AVX2_FLAGS) -c $< -o $@

$(BUILD_DIR)/hp_csv_avx512.o: $(SRC_DIR)/hp_csv_avx512.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(AVX512_FLAGS) -c $< -o $@

$(BUILD_DIR)/hp_json_scalar.o: $(SRC_DIR)/hp_json_scalar.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_json_avx2.o: $(SRC_DIR)/hp_json_avx2.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(AVX2_FLAGS) -c $< -o $@

$(BUILD_DIR)/hp_json_avx512.o: $(SRC_DIR)/hp_json_avx512.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(AVX512_FLAGS) -c $< -o $@

$(BUILD_DIR)/hp_filter.o: $(SRC_DIR)/hp_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_transcoder.o: $(SRC_DIR)/hp_transcoder.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libhyperpipe.a: $(CORE_OBJS)
	ar rcs $@ $^

$(BUILD_DIR)/hyperpipe: $(SRC_DIR)/hp_main.c $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -lhyperpipe -o $@

$(BUILD_DIR)/bench_hyperpipe: $(BENCH_DIR)/bench_main.c $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -lhyperpipe -lpsapi -o $@

test: $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) $(TEST_DIR)/test_rfc4180.c -L$(BUILD_DIR) -lhyperpipe -o $(BUILD_DIR)/test_rfc4180
	$(CC) $(CFLAGS) $(TEST_DIR)/test_ring_buffer.c -L$(BUILD_DIR) -lhyperpipe -o $(BUILD_DIR)/test_ring_buffer
	$(CC) $(CFLAGS) $(TEST_DIR)/test_differential.c -L$(BUILD_DIR) -lhyperpipe -o $(BUILD_DIR)/test_differential
	$(CC) $(CFLAGS) $(TEST_DIR)/test_filter.c -L$(BUILD_DIR) -lhyperpipe -o $(BUILD_DIR)/test_filter
	$(CC) $(CFLAGS) $(TEST_DIR)/test_roundtrip.c -L$(BUILD_DIR) -lhyperpipe -o $(BUILD_DIR)/test_roundtrip
	$(BUILD_DIR)/test_rfc4180
	$(BUILD_DIR)/test_ring_buffer
	$(BUILD_DIR)/test_differential
	$(BUILD_DIR)/test_filter
	$(BUILD_DIR)/test_roundtrip

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test clean
