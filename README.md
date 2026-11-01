# HyperPipe

High-performance streaming structured data transcoder and in-flight filter (CSV <-> NDJSON, TSV <-> NDJSON) in pure C11 with SIMD vector acceleration (AVX-512BW, AVX2, BMI2, and scalar fallback with dynamic runtime CPUID dispatch).

## Key Features
- **Bounded Sliding Ring Buffer**: $O(1)$ memory consumption (< 5 MB RSS) regardless of input stream size.
- **RFC 4180 Compliance**: Complete handling of escaped quotes (`""`), embedded commas, embedded newlines, CRLF, LF, and trailing record EOF boundaries.
- **Vector Acceleration**:
  - AVX-512BW 64-byte vector scanning with native mask registers (`__mmask64`).
  - AVX2 32-byte vector scanning with bitmask extraction (`_mm256_movemask_epi8`).
  - Vectorized JSON string escape scanning and zero-copy string serialization.
- **In-Flight Stream Operations**:
  - Column projection (`--select col1,col2,...`)
  - Predicate filtering (`--where "col == val, age >= 18"`)
- **Differential Equivalence**: Comprehensive test suite verifying that Scalar, AVX2, and AVX-512 engines yield identical binary outputs.

## Building

### Using CMake and Ninja
```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Using Make
```bash
make
make test
```

## CLI Usage

```bash
# Convert CSV to NDJSON
hyperpipe -i data.csv -o data.ndjson

# Convert TSV to NDJSON with column projection and filtering
hyperpipe --from tsv --to ndjson -i data.tsv --select id,user,score --where "score >= 80" -o filtered.ndjson

# Convert NDJSON back to CSV
hyperpipe --from ndjson --to csv -i data.ndjson -o roundtrip.csv

# Run with explicit SIMD engine and performance stats
hyperpipe -i huge.csv -o huge.ndjson --simd avx512 --stats
```

## Benchmarks (AMD Ryzen 5 7600X, Zen 4)

```bash
./build/bench_hyperpipe 500000
```
- **Throughput**: ~45.7 MiB/s (~0.66 Million rows/sec)
- **Peak RSS**: 4.91 MB
