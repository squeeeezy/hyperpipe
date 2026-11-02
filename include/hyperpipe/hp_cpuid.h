#ifndef HYPERPIPE_HP_CPUID_H
#define HYPERPIPE_HP_CPUID_H

#include "hp_common.h"
#include "hp_csv_parser.h"
#include "hp_json_ser.h"

typedef struct hp_cpu_caps_s {
    bool has_avx2;
    bool has_bmi2;
    bool has_avx512f;
    bool has_avx512bw;
    bool has_avx512vl;
    bool os_avx_supported;
    bool os_avx512_supported;
} hp_cpu_caps_t;

typedef struct hp_dispatch_table_s {
    hp_simd_mode_t active_mode;
    const char *active_mode_name;
    size_t (*csv_scan_special)(const uint8_t *p, size_t len, uint8_t delim);
    size_t (*csv_scan_quote)(const uint8_t *p, size_t len);
    size_t (*json_scan_escapes)(const uint8_t *p, size_t len);
    size_t (*json_emit_string)(char *dst, const char *str, size_t len);
    /* NDJSON input span scanners (see hp_json_ser.h for the contract) */
    size_t (*json_scan_str)(const uint8_t *p, size_t len);
    size_t (*json_scan_lit)(const uint8_t *p, size_t len);
    size_t (*json_scan_ws)(const uint8_t *p, size_t len);
    /* Structural fast path: one chunk (up to 64 bytes) -> quote/backslash
     * bitmaps. Bit i of the quote (qm) and backslash (bm) masks maps to p[i]. Only enabled bytes are
     * read, so any chunk_len <= 64 is safe without padding guarantees. */
    void (*json_struct_masks)(const uint8_t *p, size_t chunk_len,
                              uint64_t *qm, uint64_t *bm);
    /* --jobs chunk planner: newline/quote bitmaps for one <= 64-byte chunk
     * (contract in hp_csv_parser.h). Consumed by the parallel coordinator's
     * boundary scan. */
    void (*plan_masks)(const uint8_t *p, size_t chunk_len,
                       uint64_t *nl, uint64_t *qm);
    hp_error_t (*csv_parse_row)(hp_csv_parser_t *parser, const uint8_t *src, size_t len, size_t *consumed, hp_row_t *out_row, bool is_eof);
} hp_dispatch_table_t;

extern hp_dispatch_table_t g_hp_dispatch;

hp_cpu_caps_t hp_detect_cpu_caps(void);
void hp_dispatch_init(hp_simd_mode_t requested_mode);
const char *hp_simd_mode_to_string(hp_simd_mode_t mode);

#endif /* HYPERPIPE_HP_CPUID_H */
