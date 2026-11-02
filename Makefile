CC ?= gcc
UNAME := $(shell uname)
ARCH := $(shell uname -m)
ifeq ($(UNAME),Linux)
PSAPI_LIB =
DL_LIB = -ldl
THREAD_LIB = -pthread
else
PSAPI_LIB = -lpsapi
DL_LIB =
THREAD_LIB =
endif
# SIMD translation units are x86-only; on other architectures the dispatch
# table falls back to the scalar implementations, which are always built.
ifneq ($(findstring x86,$(ARCH))$(findstring amd64,$(ARCH)),)
AVX2_FLAGS = -mavx2 -mbmi2
AVX512_FLAGS = -mavx512f -mavx512bw -mavx512vl -mavx2 -mbmi2
else
AVX2_FLAGS =
AVX512_FLAGS =
endif

# -MMD -MP: auto-generate header dependencies per object, so touching a
# header rebuilds every translation unit that includes it (stale-object
# ABI mismatches otherwise link silently and corrupt behavior).
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -MMD -MP -Iinclude

BUILD_DIR = build
SRC_DIR = src
TEST_DIR = tests
BENCH_DIR = bench

CORE_OBJS = \
	$(BUILD_DIR)/hp_ring_buffer.o \
	$(BUILD_DIR)/hp_gzip.o \
	$(BUILD_DIR)/hp_cpuid.o \
	$(BUILD_DIR)/hp_keymap.o \
	$(BUILD_DIR)/hp_summary.o \
	$(BUILD_DIR)/hp_parallel.o \
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

$(BUILD_DIR)/hp_gzip.o: $(SRC_DIR)/hp_gzip.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_cpuid.o: $(SRC_DIR)/hp_cpuid.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_keymap.o: $(SRC_DIR)/hp_keymap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_summary.o: $(SRC_DIR)/hp_summary.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hp_parallel.o: $(SRC_DIR)/hp_parallel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_LIB) -c $< -o $@

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
	$(CC) $(CFLAGS) $(THREAD_LIB) $< -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) -o $@

$(BUILD_DIR)/bench_hyperpipe: $(BENCH_DIR)/bench_main.c $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) $(THREAD_LIB) $< -L$(BUILD_DIR) -lhyperpipe $(PSAPI_LIB) $(DL_LIB) -o $@

test: $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) $(TEST_DIR)/test_rfc4180.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_rfc4180
	$(CC) $(CFLAGS) $(TEST_DIR)/test_ring_buffer.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_ring_buffer
	$(CC) $(CFLAGS) $(TEST_DIR)/test_differential.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_differential
	$(CC) $(CFLAGS) $(TEST_DIR)/test_filter.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_filter
	$(CC) $(CFLAGS) $(TEST_DIR)/test_roundtrip.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_roundtrip
	$(CC) $(CFLAGS) $(TEST_DIR)/test_fault_tolerance.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_fault_tolerance
	$(CC) $(CFLAGS) $(TEST_DIR)/test_real_dataset.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_real_dataset
	$(CC) $(CFLAGS) $(TEST_DIR)/test_gzip.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_gzip
	$(CC) $(CFLAGS) $(TEST_DIR)/test_chaos.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_chaos
	$(CC) $(CFLAGS) $(TEST_DIR)/test_summary.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_summary
	$(CC) $(CFLAGS) $(TEST_DIR)/test_parallel.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/test_parallel
	$(BUILD_DIR)/test_rfc4180
	$(BUILD_DIR)/test_ring_buffer
	$(BUILD_DIR)/test_differential
	$(BUILD_DIR)/test_filter
	$(BUILD_DIR)/test_roundtrip
	$(BUILD_DIR)/test_fault_tolerance
	$(BUILD_DIR)/test_real_dataset
	$(BUILD_DIR)/test_gzip
	$(BUILD_DIR)/test_chaos
	$(BUILD_DIR)/test_summary
	$(BUILD_DIR)/test_parallel

clean:
	rm -rf $(BUILD_DIR)

# ---------------------------------------------------------------------------
# Profile-Guided Optimization (GCC/Clang on POSIX)
#
# Three-stage build: instrument -> train on representative workloads ->
# rebuild with the observed profile. PGO gains are workload-dependent; on the
# Linux reference host it adds a few percent on CSV->CSV and up to ~10% on
# the JSON-serialization paths. The training set below is a deterministic
# replication of data/nasa_server_access_log.csv, so results are comparable
# across machines.
# ---------------------------------------------------------------------------
PGO_DIR = build-pgo
TRAIN_CSV = $(PGO_DIR)/train.csv

$(TRAIN_CSV): data/nasa_server_access_log.csv | $(PGO_DIR)
	@rm -f $@
	@for i in `seq 1 60`; do cat data/nasa_server_access_log.csv; done > $@

$(PGO_DIR):
	mkdir -p $(PGO_DIR)

pgo-clean:
	rm -rf $(PGO_DIR)

PGO_SRCS = $(wildcard $(SRC_DIR)/*.c)
PGO_OBJS_INSTR = $(patsubst $(SRC_DIR)/%.c,$(PGO_DIR)/%.instr.o,$(PGO_SRCS))
PGO_OBJS_USE = $(patsubst $(SRC_DIR)/%.c,$(PGO_DIR)/%.use.o,$(PGO_SRCS))

release-pgo: $(TRAIN_CSV)
	@echo "== PGO stage 1/3: instrumented build =="
	for s in $(PGO_SRCS); do \
	        $(CC) $(CFLAGS) -fprofile-generate=$$PWD/$(PGO_DIR) -c $$s -o $(PGO_DIR)/`basename $$s .c`.instr.o || exit 1; \
	done
	$(CC) $(CFLAGS) -fprofile-generate=$$PWD/$(PGO_DIR) $(PGO_OBJS_INSTR) -o $(PGO_DIR)/hyperpipe-instr
	@echo "== PGO stage 2/3: training runs =="
	# CSV-filter workload only. Measured on the reference host: training the
	# NDJSON paths too made GCC reorder hp_json_ser_row's hot branches and
	# REGRESSED CSV->NDJSON by ~15%, while training CSV only gave +9% on
	# CSV->CSV with no effect elsewhere. Keep the training set minimal and
	# re-measure on your own workload before adopting (see docs/BENCHMARKS.md).
	$(PGO_DIR)/hyperpipe-instr $(TRAIN_CSV) "status == 200" --select host,status,bytes --to csv -o /dev/null
	$(PGO_DIR)/hyperpipe-instr $(TRAIN_CSV) "status == 200" --select host,status,bytes --to csv -o /dev/null
	@echo "== PGO stage 3/3: optimized build =="
	for s in $(PGO_SRCS); do \
	        $(CC) $(CFLAGS) -fprofile-use=$$PWD/$(PGO_DIR) -fprofile-correction -c $$s -o $(PGO_DIR)/`basename $$s .c`.instr.o || exit 1; \
	done
	$(CC) $(CFLAGS) -fprofile-use=$$PWD/$(PGO_DIR) $(PGO_OBJS_INSTR) -o $(BUILD_DIR)/hyperpipe-pgo
	@echo "Built $(BUILD_DIR)/hyperpipe-pgo"

# ---------------------------------------------------------------------------
# In-process fuzzing: ASan + UBSan build of tests/fuzz_harness.c with a
# standalone random-mutation driver (works with plain gcc; the same file
# also provides an LLVMFuzzerTestOneInput entry for OSS-Fuzz/libFuzzer).
# ---------------------------------------------------------------------------
fuzz: $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	        $(TEST_DIR)/fuzz_harness.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/fuzz_hyperpipe
	$(BUILD_DIR)/fuzz_hyperpipe $(wildcard $(TEST_DIR)/corpus/*)

# Longer budget with the in-repo corpus seeds; the argument is in thousands
# of mutation iterations. Scale up (e.g. `make fuzz-long FUZZ_K=5000`) before
# releases or after touching a parser.
FUZZ_K ?= 200
fuzz-long: $(BUILD_DIR)/libhyperpipe.a
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	        $(TEST_DIR)/fuzz_harness.c -L$(BUILD_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(BUILD_DIR)/fuzz_hyperpipe
	$(BUILD_DIR)/fuzz_hyperpipe $(FUZZ_K) $(wildcard $(TEST_DIR)/corpus/*)

# Coverage report over the full test suite: gcov summary pairs
# (source file -> lines executed) printed after the run. Objects are named
# *.cov.o, so gcov is pointed at them directly to find the matching .gcno.
COV_DIR = build-cov
coverage:
	rm -rf $(COV_DIR) *.gcov
	mkdir -p $(COV_DIR)
	for s in $(SRC_DIR)/*.c; do \
	        $(CC) $(CFLAGS) --coverage -c $$s -o $(COV_DIR)/`basename $$s .c`.cov.o || exit 1; \
	done
	ar rcs $(COV_DIR)/libhyperpipe.a $(COV_DIR)/*.cov.o
	for t in rfc4180 ring_buffer differential filter roundtrip fault_tolerance real_dataset gzip chaos summary parallel; do \
	        $(CC) $(CFLAGS) --coverage $(TEST_DIR)/test_$$t.c -L$(COV_DIR) -lhyperpipe $(DL_LIB) $(THREAD_LIB) -o $(COV_DIR)/test_$$t || exit 1; \
	        $(COV_DIR)/test_$$t >/dev/null || exit 1; \
	done
	@for o in $(COV_DIR)/*.cov.o; do gcov -o $(COV_DIR) "$$o" || exit 1; done 2>&1 | grep -E "^File |^Lines executed"

.PHONY: all test fuzz fuzz-long coverage clean pgo-clean release-pgo
