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
    hp_error_t (*csv_parse_row)(hp_csv_parser_t *parser, const uint8_t *src, size_t len, size_t *consumed, hp_row_t *out_row, bool is_eof);
} hp_dispatch_table_t;

extern hp_dispatch_table_t g_hp_dispatch;

hp_cpu_caps_t hp_detect_cpu_caps(void);
void hp_dispatch_init(hp_simd_mode_t requested_mode);
const char *hp_simd_mode_to_string(hp_simd_mode_t mode);

#endif /* HYPERPIPE_HP_CPUID_H */
