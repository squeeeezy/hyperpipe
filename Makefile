# HyperPipe
#
#   make            build build/hyperpipe
#   make test       build and run every tests/test_*.c (TAP output)
#   make bench      build build/bench_hyperpipe
#   make fuzz       ASan + UBSan fuzz run; FUZZ_K sets thousands of iterations
#   make coverage   line coverage of the test suite (gcov)
#   make pgo        profile-guided build/hyperpipe-pgo, trained on data/
#
# CFLAGS and LDFLAGS are the user's; the flags the code needs are added below.

CC      ?= cc
CFLAGS  ?= -O3
BUILD   ?= build
FUZZ_K  ?= 50

HP_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Iinclude -MMD -MP

ifeq ($(OS),Windows_NT)
    EXE    = .exe
    LDLIBS = -lpsapi
else
    EXE    =
    LDLIBS = -pthread -ldl
endif

# The SIMD units are x86 only; elsewhere they compile to nothing usable and
# runtime dispatch keeps the scalar paths.
ARCH := $(shell $(CC) -dumpmachine)
ifneq ($(findstring x86_64,$(ARCH))$(findstring i686,$(ARCH)),)
    AVX2_FLAGS   = -mavx2 -mbmi2
    AVX512_FLAGS = -mavx512f -mavx512bw -mavx512vl -mavx2 -mbmi2
endif

LIB_SRCS  = $(filter-out src/hp_main.c,$(wildcard src/*.c))
LIB_OBJS  = $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB       = $(BUILD)/libhyperpipe.a
TESTS     = $(patsubst tests/%.c,$(BUILD)/%$(EXE),$(wildcard tests/test_*.c))

all: $(BUILD)/hyperpipe$(EXE)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(HP_CFLAGS) $(CFLAGS) $(SIMD_FLAGS) -c $< -o $@

$(BUILD)/hp_csv_avx2.o $(BUILD)/hp_json_avx2.o: SIMD_FLAGS = $(AVX2_FLAGS)
$(BUILD)/hp_csv_avx512.o $(BUILD)/hp_json_avx512.o: SIMD_FLAGS = $(AVX512_FLAGS)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/hyperpipe$(EXE): src/hp_main.c $(LIB)
	$(CC) $(HP_CFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/bench_hyperpipe$(EXE): bench/bench_main.c $(LIB)
	$(CC) $(HP_CFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

$(BUILD)/test_%$(EXE): tests/test_%.c $(LIB)
	$(CC) $(HP_CFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

bench: $(BUILD)/bench_hyperpipe$(EXE)

test: $(TESTS)
	@for t in $(TESTS); do echo "# $$t"; ./$$t || exit 1; done

fuzz: $(LIB)
	$(CC) $(HP_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    tests/fuzz_harness.c $(LIB_SRCS) $(AVX512_FLAGS) $(LDLIBS) -o $(BUILD)/fuzz_hyperpipe$(EXE)
	./$(BUILD)/fuzz_hyperpipe$(EXE) $(FUZZ_K) $(wildcard tests/corpus/*)

COV = $(BUILD)/coverage
coverage:
	rm -rf $(COV) && mkdir -p $(COV)
	$(MAKE) BUILD=$(COV) CFLAGS="-O0 --coverage" LDFLAGS=--coverage test
	gcov -n -o $(COV) $(LIB_SRCS) | grep -A1 "^File 'src"

# Trained on the CSV filter path only: training the NDJSON paths as well
# reordered the serializer's branches and slowed CSV to NDJSON.
PGO = $(BUILD)/pgo
pgo:
	rm -rf $(PGO) && mkdir -p $(PGO)
	$(MAKE) BUILD=$(PGO) CFLAGS="$(CFLAGS) -fprofile-generate -fprofile-update=atomic" \
	    LDFLAGS=-fprofile-generate
	./$(PGO)/hyperpipe$(EXE) data/nasa_server_access_log.csv "status == 200" \
	    --select host,status,bytes --to csv --jobs 1 -o $(PGO)/train.out
	rm -f $(PGO)/*.o $(PGO)/libhyperpipe.a $(PGO)/hyperpipe$(EXE)
	$(MAKE) BUILD=$(PGO) CFLAGS="$(CFLAGS) -fprofile-use -fprofile-correction"
	cp $(PGO)/hyperpipe$(EXE) $(BUILD)/hyperpipe-pgo$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all bench test fuzz coverage pgo clean

-include $(wildcard $(BUILD)/*.d)
