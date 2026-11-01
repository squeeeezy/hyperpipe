#include "hyperpipe/hp_cpuid.h"

#if defined(_MSC_VER)
    #include <intrin.h>
    static void run_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
        int cpu_info[4];
        __cpuidex(cpu_info, leaf, subleaf);
        *eax = (uint32_t)cpu_info[0];
        *ebx = (uint32_t)cpu_info[1];
        *ecx = (uint32_t)cpu_info[2];
        *edx = (uint32_t)cpu_info[3];
    }
    static uint64_t read_xcr0(void) {
        return _xgetbv(0);
    }
#elif defined(__GNUC__) || defined(__clang__)
    #include <cpuid.h>
    #include <immintrin.h>
    static void run_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
        __cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
    }
    static uint64_t read_xcr0(void) {
        uint32_t eax, edx;
        __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        return ((uint64_t)edx << 32) | eax;
    }
#else
    static void run_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
        (void)leaf; (void)subleaf; *eax = *ebx = *ecx = *edx = 0;
    }
    static uint64_t read_xcr0(void) { return 0; }
#endif

hp_dispatch_table_t g_hp_dispatch = {
    .active_mode = HP_SIMD_SCALAR,
    .active_mode_name = "Scalar",
    .csv_scan_special = hp_csv_scan_special_scalar,
    .csv_scan_quote = hp_csv_scan_quote_scalar,
    .json_scan_escapes = hp_json_scan_escapes_scalar,
    .csv_parse_row = hp_csv_parse_row_scalar
};

hp_cpu_caps_t hp_detect_cpu_caps(void) {
    hp_cpu_caps_t caps;
    memset(&caps, 0, sizeof(caps));

    uint32_t eax, ebx, ecx, edx;
    run_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    if (max_leaf < 1) return caps;

    /* Check OSXSAVE support in leaf 1 */
    run_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    bool osxsave = (ecx & (1u << 27)) != 0;

    if (osxsave) {
        uint64_t xcr0 = read_xcr0();
        caps.os_avx_supported = (xcr0 & 0x06) == 0x06;
        caps.os_avx512_supported = caps.os_avx_supported && ((xcr0 & 0xE0) == 0xE0);
    }

    if (max_leaf >= 7) {
        run_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        if (caps.os_avx_supported) {
            caps.has_avx2 = (ebx & (1u << 5)) != 0;
            caps.has_bmi2 = (ebx & (1u << 8)) != 0;
        }
        if (caps.os_avx512_supported) {
            caps.has_avx512f  = (ebx & (1u << 16)) != 0;
            caps.has_avx512bw = (ebx & (1u << 30)) != 0;
            caps.has_avx512vl = (ebx & (1u << 31)) != 0;
        }
    }

    return caps;
}

const char *hp_simd_mode_to_string(hp_simd_mode_t mode) {
    switch (mode) {
        case HP_SIMD_SCALAR: return "Scalar";
        case HP_SIMD_AVX2:   return "AVX2";
        case HP_SIMD_AVX512: return "AVX-512";
        case HP_SIMD_AUTO:   return "Auto";
        default:             return "Unknown";
    }
}

void hp_dispatch_init(hp_simd_mode_t requested_mode) {
    hp_cpu_caps_t caps = hp_detect_cpu_caps();

    hp_simd_mode_t selected = HP_SIMD_SCALAR;

    if (requested_mode == HP_SIMD_AUTO) {
        if (caps.has_avx512f && caps.has_avx512bw && caps.has_avx512vl) {
            selected = HP_SIMD_AVX512;
        } else if (caps.has_avx2) {
            selected = HP_SIMD_AVX2;
        } else {
            selected = HP_SIMD_SCALAR;
        }
    } else if (requested_mode == HP_SIMD_AVX512) {
        if (caps.has_avx512f && caps.has_avx512bw && caps.has_avx512vl) {
            selected = HP_SIMD_AVX512;
        } else {
            selected = caps.has_avx2 ? HP_SIMD_AVX2 : HP_SIMD_SCALAR;
        }
    } else if (requested_mode == HP_SIMD_AVX2) {
        selected = caps.has_avx2 ? HP_SIMD_AVX2 : HP_SIMD_SCALAR;
    } else {
        selected = HP_SIMD_SCALAR;
    }

    g_hp_dispatch.active_mode = selected;
    g_hp_dispatch.active_mode_name = hp_simd_mode_to_string(selected);

    switch (selected) {
        case HP_SIMD_AVX512:
            g_hp_dispatch.csv_scan_special = hp_csv_scan_special_avx512;
            g_hp_dispatch.csv_scan_quote = hp_csv_scan_quote_avx512;
            g_hp_dispatch.json_scan_escapes = hp_json_scan_escapes_avx512;
            g_hp_dispatch.csv_parse_row = hp_csv_parse_row_avx512;
            break;
        case HP_SIMD_AVX2:
            g_hp_dispatch.csv_scan_special = hp_csv_scan_special_avx2;
            g_hp_dispatch.csv_scan_quote = hp_csv_scan_quote_avx2;
            g_hp_dispatch.json_scan_escapes = hp_json_scan_escapes_avx2;
            g_hp_dispatch.csv_parse_row = hp_csv_parse_row_scalar;
            break;
        case HP_SIMD_SCALAR:
        default:
            g_hp_dispatch.csv_scan_special = hp_csv_scan_special_scalar;
            g_hp_dispatch.csv_scan_quote = hp_csv_scan_quote_scalar;
            g_hp_dispatch.json_scan_escapes = hp_json_scan_escapes_scalar;
            g_hp_dispatch.csv_parse_row = hp_csv_parse_row_scalar;
            break;
    }
}

hp_error_t hp_csv_parser_init(hp_csv_parser_t *parser, hp_format_t fmt, hp_simd_mode_t simd) {
    if (!parser) return HP_ERR_INVALID_ARG;
    memset(parser, 0, sizeof(*parser));

    if (fmt == HP_FORMAT_TSV) {
        parser->delim = '\t';
    } else {
        parser->delim = ',';
    }
    parser->quote = '"';

    if (simd != HP_SIMD_AUTO) {
        hp_dispatch_init(simd);
    } else if (g_hp_dispatch.active_mode == HP_SIMD_SCALAR) {
        hp_dispatch_init(HP_SIMD_AUTO);
    }

    parser->scan_special = g_hp_dispatch.csv_scan_special;
    parser->scan_quote = g_hp_dispatch.csv_scan_quote;
    return HP_OK;
}
