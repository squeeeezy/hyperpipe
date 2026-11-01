# HyperPipe Architecture & Engineering Specification

## 1. Overview
HyperPipe is an $O(1)$ memory streaming structured data transcoder and in-flight filter (CSV <-> NDJSON, TSV <-> NDJSON) written in ISO C11 with dynamic runtime vector acceleration (AVX-512BW/VL, AVX2, BMI2, and scalar fallback).

---

## 2. Sliding Linear Ring Buffer ($O(1)$ RSS Guarantee)

Traditional circular buffers present non-contiguous wrap-arounds that break vector loads, forcing branchy split-register handling. HyperPipe uses a **Sliding Linear Window with Zero-Copy Vector Padding**:

```
+---------------------------+-----------------------------------+--------------------+-----------------+
| Parsed & Consumed Prefix  |      Active Readable Window       | Free Space (Tail)  | SIMD Tail Guard |
|                           |                                   |                    |    (64 Bytes)   |
+---------------------------+-----------------------------------+--------------------+-----------------+
0                           ^ head                              ^ tail               ^ capacity
                            |<-------- Linear Span ------------>|
```

### Sliding Operation (Zero Allocation)
1. Active data is processed contiguously between `head` and `tail`.
2. When `head > 0` and additional stream data is required, `memmove(data, data + head, tail - head)` slides unparsed bytes back to index 0 in a single memory operation.
3. `head` is reset to 0, `tail` becomes `active_len`.
4. Free space from `tail` to `capacity` is refilled directly via `fread`.
5. 64 trailing safety bytes are zeroed to prevent vector load faults (`_mm512_loadu_si512`) past buffer bounds.
6. Memory usage is bounded by `capacity + 64` (default 64 KB, peak RSS < 5 MB regardless of stream length).

---

## 3. Vectorized CSV Scanner State Machine

### AVX2 Kernel (32 Bytes / Cycle)
```
Input Stream (32 Bytes)
         │
         ▼
 _mm256_loadu_si256
         │
 ┌───────┴───────────────┬───────────────────────┬───────────────────────┐
 │                       │                       │                       │
 _mm256_cmpeq_epi8(',')  _mm256_cmpeq_epi8('"')  _mm256_cmpeq_epi8('\n') _mm256_cmpeq_epi8('\r')
 │                       │                       │                       │
 └───────┬───────────────┴───────────────────────┴───────────────────────┘
         │
     _mm256_or_si256
         │
         ▼
 _mm256_movemask_epi8  ──►  Bitmask (32 bits)
                                │
                                ├─► Mask == 0: 32 bytes plain text (skip 32)
                                └─► Mask != 0: __builtin_ctz(mask) (jump to delimiter)
```

### AVX-512BW Kernel (64 Bytes / Cycle)
Zen 4 executes 64-byte vector comparisons returning native `__mmask64` registers:
```c
__m512i chunk = _mm512_loadu_si512((const void *)ptr);
__mmask64 m_delim = _mm512_cmpeq_epi8_mask(chunk, v_delim);
__mmask64 m_quote = _mm512_cmpeq_epi8_mask(chunk, v_quote);
__mmask64 m_lf    = _mm512_cmpeq_epi8_mask(chunk, v_lf);
__mmask64 m_cr    = _mm512_cmpeq_epi8_mask(chunk, v_cr);
uint64_t mask = (uint64_t)(m_delim | m_quote | m_lf | m_cr);
```

---

## 4. Vectorized JSON String Escape Scanner

JSON string serialization bottleneck is scanning each byte for `"`, `\`, and control characters (`< 0x20`).
HyperPipe vectorizes this with unsigned saturating subtraction and bitmask tests:

```
  AVX2 Branchless Escape Detection:
  under_32 = _mm256_cmpeq_epi8(_mm256_subs_epu8(chunk, 31), _mm256_setzero_si256());
  is_quote = _mm256_cmpeq_epi8(chunk, '"');
  is_slash = _mm256_cmpeq_epi8(chunk, '\\');
  escapes  = is_quote | is_slash | under_32;
```
If `escapes == 0`, all 32 bytes are copied directly via `memcpy` without per-byte character inspection.

---

## 5. Dynamic Runtime CPUID Dispatch
CPU capabilities are queried once at startup:
- Leaf 1 & Leaf 7: AVX2, BMI2, AVX-512F, AVX-512BW, AVX-512VL.
- XCR0 register: OS vector state support.
- Function pointers in `g_hp_dispatch` are populated once with optimal SIMD or fallback scalar routines.
