# HyperPipe Architecture & Systems Specification

## 1. Overview
HyperPipe is a bounded $O(1)$ memory streaming structured data transcoder and in-flight projection/filtering engine written in pure ISO C11 with dynamic runtime vector acceleration (AVX-512BW/VL, AVX2, BMI2, and scalar fallback).

---

## 2. Sliding Linear Ring Buffer ($O(1)$ RSS Guarantee)

Traditional circular buffers suffer from non-contiguous wrap-around boundaries that break SIMD vector loads, forcing branchy split-register handling. HyperPipe solves this using a **Sliding Linear Window with Zero-Copy Vector Padding**:

```mermaid
flowchart LR
    subgraph MemoryBuffer["Continuous 64 KB Memory Buffer"]
        direction LR
        A["Prefix (0..head)<br/>Parsed & Consumed"] --> B["Active Window (head..tail)<br/>Contiguous Vector Scan Span"]
        B --> C["Tail Free Space (tail..cap)<br/>Zero-Copy DMA/fread Target"]
        C --> D["Tail Guard Zone (64 Bytes)<br/>Zero-Padded SIMD Safety Overrun"]
    end
```

### Sliding Window Lifecycle (Zero Allocation)

```mermaid
sequenceDiagram
    autonumber
    participant Stream as Input Stream (Pipe/Disk)
    participant Buffer as Sliding Linear Ring Buffer
    participant Parser as Vectorized SIMD Scanner
    
    Stream->>Buffer: Initial block read (fread into tail free space)
    loop Continuous Streaming
        Buffer->>Parser: Zero-copy pointer to contiguous active window
        Parser->>Parser: Vectorized delimiter & quote scanning
        Parser->>Buffer: Consume parsed row bytes (advance head)
        alt Active Window Low (head > 0)
            Buffer->>Buffer: memmove(data, data + head, tail - head)
            Note over Buffer: Reset head to 0, slide unconsumed bytes to base
            Stream->>Buffer: fread fills free space (tail to capacity)
            Buffer->>Buffer: memset 64 trailing bytes to 0 (SIMD guard)
        end
    end
```

---

## 3. Vectorized CSV Scanner State Machine

### AVX2 Scanner Pipeline (32 Bytes / Cycle)

```mermaid
flowchart TD
    In["Input Stream Chunk (32 Bytes)"] --> Load["_mm256_loadu_si256"]
    
    Load --> CmpDelim["_mm256_cmpeq_epi8(',')"]
    Load --> CmpQuote["_mm256_cmpeq_epi8('\"')"]
    Load --> CmpLF["_mm256_cmpeq_epi8('\\n')"]
    Load --> CmpCR["_mm256_cmpeq_epi8('\\r')"]
    
    CmpDelim --> OrAll["_mm256_or_si256"]
    CmpQuote --> OrAll
    CmpLF --> OrAll
    CmpCR --> OrAll
    
    OrAll --> Movemask["_mm256_movemask_epi8 (Extract 32-bit Mask)"]
    
    Movemask --> Check{"Mask == 0 ?"}
    Check -- Yes --> Skip["Advance 32 Bytes (Zero Branches)"]
    Check -- No --> CTZ["__builtin_ctz(mask) (Jump Directly to Delimiter)"]
```

### AVX-512BW Scanner Pipeline (64 Bytes / Cycle)

Zen 4 and modern Xeon architectures execute 64-byte vector comparisons returning native `__mmask64` registers:

```mermaid
flowchart TD
    Chunk["Input Stream Chunk (64 Bytes)"] --> Load512["_mm512_loadu_si512"]
    
    Load512 --> M1["_mm512_cmpeq_epi8_mask(chunk, v_delim)"]
    Load512 --> M2["_mm512_cmpeq_epi8_mask(chunk, v_quote)"]
    Load512 --> M3["_mm512_cmpeq_epi8_mask(chunk, v_lf)"]
    Load512 --> M4["_mm512_cmpeq_epi8_mask(chunk, v_cr)"]
    
    M1 --> BitwiseOr["Bitwise OR on k-registers (m1 | m2 | m3 | m4)"]
    M2 --> BitwiseOr
    M3 --> BitwiseOr
    M4 --> BitwiseOr
    
    BitwiseOr --> Mask64["64-bit Combined Mask"]
    Mask64 --> BranchlessJump["_tzcnt_u64(mask) -> Direct Opcode Dispatch"]
```

---

## 4. Fault-Tolerant Streaming State Machine

```mermaid
stateDiagram-v2
    [*] --> SniffFormat
    SniffFormat --> StreamIngest: Auto-Sniff Delimiters & Headers
    
    state StreamIngest {
        [*] --> ParseRow
        ParseRow --> ValidateRow: Check Syntax & Escape Boundaries
        ValidateRow --> InFlightFilter: Valid Row
        InFlightFilter --> PageBufferOutput: Predicate Match
        InFlightFilter --> ParseRow: Filter Reject
        
        ValidateRow --> ErrorDispatch: Malformed / Corrupt Row
        state ErrorDispatch <<choice>>
        ErrorDispatch --> Abort: on-error fail
        ErrorDispatch --> ForwardSync: on-error skip / quarantine
        ForwardSync --> WriteQuarantine: quarantine path configured
        WriteQuarantine --> ResyncNewline: Write Raw Line
        ForwardSync --> ResyncNewline: on-error skip
        ResyncNewline --> ParseRow: SIMD search for next '\\n'
    }
    
    PageBufferOutput --> FlushOutput: Buffer Full (64 KB)
    FlushOutput --> [*]: Stream EOF
```

---

## 5. Dynamic Runtime CPUID Dispatch

CPU capabilities are detected once at process initialization:

```mermaid
flowchart TD
    Init["hp_dispatch_init()"] --> CPUID["cpuid(EAX=1, EAX=7)"]
    CPUID --> XCR0["_xgetbv(0) -> OS Vector Support Check"]
    
    XCR0 --> Branch{"Features Enabled?"}
    Branch -- "AVX-512F + BW + VL & OS State" --> Select512["Select AVX-512BW Engine (64B/cycle)"]
    Branch -- "AVX2 + BMI2 & OS State" --> Select256["Select AVX2 Engine (32B/cycle)"]
    Branch -- Fallback --> SelectScalar["Select Pure C11 Scalar Fallback"]
    
    Select512 --> Table["Populate g_hp_dispatch Function Pointers"]
    Select256 --> Table
    SelectScalar --> Table
```
