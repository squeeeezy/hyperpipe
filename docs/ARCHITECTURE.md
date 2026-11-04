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
  JSON grammar in the same pass. Objects and arrays are measured by
  `json_value_span()` and jumped over, escapes inside them included. A
  line it does not fully accept (a backslash elsewhere, an unknown key,
  anything irregular) goes to the general path, which either parses it or
  reports the error. The fast path writes no shared state before it
  succeeds.

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
parser for its format. The loop is inlined per format, and per whether rows
may get computed columns, so no copy pays an indirect call, or a test for
work it never does, per record; the copies with computed columns live in
`run_computing()`, out of line, so the others stay as small as they were. For each record it binds the header on the
first one, follows NDJSON schema growth, computes the columns the filter
reads, applies the filter, computes the rest and passes the record to
`deliver_record()` (group, summary, count or write). Parse errors go to the
`--on-error` mode; everything else ends the run.

The hot kernels, the record parsers, the NDJSON writer and the loop itself,
are marked `HP_HOT`, which also starts them on a 64-byte boundary: the
speed of a loop that runs 30 ns a record then does not depend on how much
code the linker happens to put before it.

## Grouping

`src/hp_agg.c` keeps one table per run: open addressing over group ids,
each group's key encoded once (length, flags, bytes per column) and its
accumulators in one array. A row is hashed over its key columns, matched
against the encoded key in place, and folded in; nothing is copied unless
the key is new. Groups are numbered in arrival order, which is the output
order.

Numbers follow `src/hp_number.h`, shared with the filter. A value with a
64-bit mantissa and at most 18 decimal places is kept as mantissa and
scale; sums are 128-bit integers at the largest scale seen, so they are
exact and do not depend on the order of the rows. Anything wider falls back
to double.

`count_distinct` keeps one table of (group, value) pairs per function
(`src/hp_distinct.c`), not a set per group; a slot holds the value's hash
apart from the group, so a merge moves pairs to other groups without hashing
again. Quantiles (`src/hp_quantile.c`) keep a group's values until there are
1,024 and then move them into a DDSketch: logarithmic buckets 2% wide, at
most 4,096 of them, the lowest folding into the next when the range grows.
The fold point only rises, so the sketch, like the kept values, depends on
the values alone and merges to the single-threaded result. Parallel workers build a table per chunk (`agg_partial`); the
coordinator merges them in chunk order, which keeps arrival order, and
writes the rows through `hp_transcoder_write_groups()`, the same writer a
single-threaded run uses.

## Time

`src/hp_time.c` reads a timestamp by its shape (a `-` at offset 4 for ISO
8601, a `/` at offset 2 for the common log format, digits for Unix time)
with fixed-position digit reads, applies the offset, and converts the date
with Howard Hinnant's `days_from_civil`, so nothing depends on the C
library's time zone or locale. Times are microseconds since 1970 in UTC.
Buckets are written by hand as sortable text. `hour(col)` keys are computed
once per row and then hashed and stored like any key; time literals in
filters are parsed once, and each field is parsed as it is compared.

## Computed columns

`src/hp_compute.c` parses `--add` into expression trees, all nodes in one
fixed array, and binds their references once the header is known. A
reference that names no column but starts with one, `meta.user.id`, binds
to the longest leading part that is a column and keeps the rest as steps
for `hp_json_lookup()` (`src/hp_json_path.c`), which walks the JSON text of
the field: members by key, elements by index, values skipped by following
strings and nesting without checking the grammar again. Escapes and string
decoding are shared with the parser through `src/hp_json_text.h`. The transcoder turns every path that
an option names, and the input lacks, into a hidden computed column of its
own, so the filter, the groups and the sorter read it by index like any
field; without `--select` the output then goes through a projection of
the other columns, which also follows NDJSON keys that appear later.

Computed columns get header slots after the input's, or the slot of the
input column they replace, and are written into the row after it is
parsed: the ones the filter reads before it, the rest only for rows that
pass. A value is a slice of the record when it can be one (a column, a path
to an unescaped string, `trim`, `substr`); anything new goes into an arena
that is sized once per row from a bound computed at bind time, so many
bytes per input byte plus a constant per expression. Its bytes add to the
record's length for the CSV writer's bound, which every field otherwise
satisfies by being a slice of the record.

Numbers carry their exact decimal, mantissa and scale as
`hp_parse_decimal_exact` gives them, through `+`, `-`, `*` and `%` in
128-bit arithmetic, and are written only when a text is needed. Past 18
digits of scale the exact value is rounded once to double, through its
decimal text and `strtod`.

A double is written as the first of `%.15g`, `%.16g` and `%.17g` that reads
back (`src/hp_number.c`). Between 1e-3 and 1e15 no `printf` is involved: the
double is `m / 2^k` exactly, so its digits round correctly in 128-bit
integers, and whether they read back is a comparison with half the gap to
the neighbouring double. That is 30 ns a value instead of up to 900, with
the same text, which a test checks against the `printf` loop over a million
values.

## Sorting

`src/hp_sort.c`. A row is serialized as usual, but into a page of its own;
the line is stored with a key built from the row, so sorted output is
written without parsing anything again. The key compares with `memcmp`:
per column a type tag, then the number as an order-preserving double or
the text with zero bytes escaped, inverted for a descending column. Each
entry keeps the first 16 key bytes inline, so numeric keys never touch the
record while sorting.

Rows sort with a stable merge sort, cache-sized blocks first. Past the
memory budget a sorted run goes to a temporary file; sixteen runs on disk
of one level merge into one of the next. With `--head` a bounded heap keeps
N rows, and its keys end with the row's position to stay deterministic.

At the end of a run the rows still in memory sort as segments on several
threads, which then merge like runs, so a sort from stdin uses every core
as well. In parallel runs each worker sorts its chunk into a run. The coordinator
cuts the runs into key ranges from sampled keys, merges the ranges on
several threads and writes them in order, with at most `jobs + 2` ranges
in flight. Equal keys break ties by run and position, so the bytes match a
single-threaded run.

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
