# Architecture

HyperPipe is a library (`src/`, `include/hyperpipe/`) and a thin command line
front end (`src/hp_main.c`). A run reads records, filters them, projects
columns and writes them out, holding one record at a time.

```
reader ─▶ ring buffer ─▶ CSV or NDJSON parser ─▶ filter ─▶ CSV or NDJSON writer ─▶ page buffer ─▶ output
(gzip)                    (SIMD dispatch)                                           (gzip)
```

`hp_transcoder_run()` drives that pipeline for one stream. `hp_parallel_run()`
runs it on many chunks of a file at once.

## Input

`hp_reader_t` (`src/hp_ring_buffer.c`) wraps a `FILE`. On the first read it
checks for the gzip magic and, when present, inflates through zlib, which
`src/hp_gzip.c` loads at run time (`dlopen` or `LoadLibrary`, with the
current directory removed from the DLL search path). Nothing links against
zlib; without it gzip reports `HP_ERR_GZIP_UNAVAILABLE` and everything else
works. A reader can also be limited to a byte count, which parallel workers
use to read exactly their chunk.

The sliding buffer (`hp_sliding_buffer_t`) is one contiguous block, 512 KiB
by default, followed by 64 zero bytes so vector loads may read past the
last valid byte. Parsers get a pointer and a length; when they need more
data the unconsumed tail moves to the front and the reader fills the rest.
A record larger than the buffer is an error, or a dropped record with
`--on-error skip`.

## CSV and TSV

`hp_csv_parse_row_scalar()` (`src/hp_csv_scalar.c`) is the reference RFC 4180
state machine. It returns one record per call as zero-copy field slices into
the buffer. Fields that contained `""` are unescaped into a parser arena,
which is sized to the parse window on first use and never moves while a
record is built. Every field records whether it was quoted (`has_escapes`),
so writers can keep its string type and re-quote it.

The AVX2 and AVX-512 parsers (`src/hp_csv_avx2.c`, `src/hp_csv_avx512.c`)
handle quote-free records: each 32- or 64-byte chunk yields delimiter and
line-end bitmasks, and fields are cut at the set bits. A chunk containing a
quote hands the whole record to the scalar parser, so quoting rules live in
one place. All three parsers produce identical rows; `test_differential`
and `test_chaos` check that.

## NDJSON

`hp_json_parse_line_ex()` (`src/hp_json_scalar.c`) parses one line into a row
whose columns are the keys seen so far. It accepts exactly an RFC 8259
object; anything else is `HP_ERR_PARSE_JSON`.

- Values that need no unescaping point into the buffer. Escaped strings are
  decoded into an arena the transcoder sizes to the line, which always
  suffices because a decoded string is never longer than its source.
- Keys resolve through `hp_keymap` (`src/hp_keymap.c`), an open-addressing
  hash over the header. Keys up to 8 bytes compare as one packed word.
  Unknown keys extend the header and the map.
- An object or array value is validated by `json_value_span()`, which keeps
  the nesting on a fixed 512-entry stack instead of recursing, and becomes
  a field with `is_json` set that points at its source text. The NDJSON
  writer copies such a field as is; everything else sees a string.
- A structural fast path handles escape-free lines: one vector pass per
  64-byte chunk gives the quote and backslash bitmaps, keys and string
  values become spans between quotes, and numbers are checked against the
  JSON grammar in the same pass. A line it does not fully accept (a
  backslash, an unknown key, anything irregular) goes to the general path,
  which either parses it or reports the error. The fast path writes no
  shared state before it succeeds.

## Filter

`src/hp_filter.c` parses the filter into up to 16 predicates in OR groups of
AND terms, and binds column references to the header: a name first, then a
position. A numeric literal compares numerically against fields that are
numbers in their whole length; integers compare exactly in int64, everything
else in double. A text literal compares bytes.

`hp_filter_matches()` is inline in the header. It decides the most common
filter, a single equality, with a byte compare: for a positive integer
literal a mismatch at a leading digit proves the numbers differ, so
`status == 500` against `200` costs one byte. Anything else goes to
`hp_filter_eval()`.

## Output

All output goes through a page buffer (256 KiB) and from there, compressed
when gzip output is on, to a `FILE` or to a sink callback.

- CSV records (`emit_csv_row_pb`): every output field is a slice of the input
  record or a decoded copy no longer than its source, so the record's worst
  case size follows from its input length. The record is reserved with one
  check and written without per-field checks. Fields are copied as-is when
  they cannot contain the output delimiter; otherwise a word-at-a-time scan
  decides whether to quote. Records larger than a page take a checked path.
- NDJSON rows (`hp_json_ser_row_direct`, `src/hp_json_scalar.c`): keys are
  escaped once per header and copied per row; values are emitted as numbers,
  literals or escaped strings. The SIMD emitters (`src/hp_json_avx2.c`,
  `src/hp_json_avx512.c`) scan and copy a string in one pass.

## Streaming loop

`hp_transcoder_run()` sets up a run and calls `run_records()` with the record
parser for its format. The loop is inlined per format, so neither copy pays
an indirect call per record. For each record it binds the header on the
first one, follows NDJSON schema growth, applies the filter and passes the
record to `deliver_record()` (summary, count or write). Parse errors go to
the `--on-error` mode; everything else ends the run.

## Parallel runs

`src/hp_parallel.c`:

1. The file is cut at fixed offsets into ranges, eight per thread, between
   1 and 8 MiB each.
2. A record ends at a newline outside quotes. For CSV and TSV, threads count
   the quotes of every range in parallel, and a prefix XOR gives the quoting
   state at each cut. For NDJSON every newline ends a record.
3. Each chunk starts at the first record that begins inside its range. A
   pool of threads takes chunks in order and transcodes each into its own
   buffer; chunks after the first bind the header read up front.
4. Chunks are written in order: by the calling thread, or on POSIX regular
   files by each worker at its offset. At most `jobs + 2` chunks are in
   flight, and written buffers are reused.

Output is byte-identical to a sequential run. CSV and TSV in skip or
quarantine mode stay sequential, because a stray quote changes the parity
that the cuts rely on, while the sequential parser resynchronizes after it.

`src/hp_thread.h` wraps Win32 threads (SRW locks, condition variables,
`_beginthreadex`) and pthreads behind one small interface.

## SIMD dispatch

`hp_dispatch_init()` (`src/hp_cpuid.c`) reads CPUID and XCR0 once and fills
`g_hp_dispatch` with the best available kernels. Only the four `*_avx2.c` and
`*_avx512.c` files are compiled with vector flags; the binary runs on any
x86-64 CPU, and on other architectures the scalar kernels are used.

## Tests

`tests/` holds one TAP binary per area, all built and run by `make test`:
unit tests per module, `test_regressions` with one case per fixed defect,
`test_parallel` comparing every job count with the sequential output,
`test_differential` and `test_chaos` comparing the SIMD engines and round
trips on generated data, and `test_real_dataset` on the NASA log.
`tests/fuzz_harness.c` checks the same round-trip invariants under libFuzzer
or its own mutation driver.
