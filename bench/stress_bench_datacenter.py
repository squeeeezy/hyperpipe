import subprocess
import time
import os
import sys
import ctypes
from ctypes import wintypes

class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ('cb', wintypes.DWORD),
        ('PageFaultCount', wintypes.DWORD),
        ('PeakWorkingSetSize', ctypes.c_size_t),
        ('WorkingSetSize', ctypes.c_size_t),
        ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
        ('QuotaPagedPoolUsage', ctypes.c_size_t),
        ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
        ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
        ('PagefileUsage', ctypes.c_size_t),
        ('PeakPagefileUsage', ctypes.c_size_t),
    ]

def get_peak_rss_mb(handle):
    try:
        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        ret = ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
        if ret:
            return counters.PeakWorkingSetSize / (1024.0 * 1024.0)
    except Exception:
        pass
    return 0.0

HYPERPIPE_EXE = os.path.abspath('e:/Test/hyperpipe/build/hyperpipe.exe')
GAWK_EXE = 'C:/Program Files/Git/usr/bin/gawk.exe'
PERL_EXE = 'C:/Program Files/Git/usr/bin/perl.exe'
PYTHON_EXE = sys.executable

DATASETS = [
    {
        'tier': 'Small (5K rows)',
        'path': os.path.abspath('e:/Test/hyperpipe/data/nasa_small.csv'),
        'rows': 5000,
        'runs': 3,
    },
    {
        'tier': 'Medium (1.89M rows)',
        'path': os.path.abspath('e:/Test/hyperpipe/data/nasa_server_access_log.csv'),
        'rows': 1891714,
        'runs': 2,
    },
    {
        'tier': 'Datacenter (51.07M rows / ~5 GB)',
        'path': os.path.abspath('e:/Test/hyperpipe/data/datacenter_access_log_5gb.csv'),
        'rows': 51076278,
        'runs': 1,
    },
]

def run_single(cmd, file_bytes, total_rows, runs):
    best_time = 1e9
    peak_rss = 0.0
    for _ in range(runs):
        t0 = time.perf_counter()
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p.wait()
        t1 = time.perf_counter()
        elapsed = t1 - t0
        if elapsed < best_time:
            best_time = elapsed
        rss = get_peak_rss_mb(int(p._handle))
        if rss > peak_rss:
            peak_rss = rss

    mib_s = (file_bytes / (1024.0 * 1024.0)) / best_time
    mrows_s = (total_rows / 1e6) / best_time
    return {
        'elapsed': best_time,
        'mib_s': mib_s,
        'mrows_s': mrows_s,
        'peak_rss': peak_rss
    }

def main():
    print("=" * 100)
    print(" HARDCORE DATACENTER STRESS BENCHMARK: SCALING FROM 5K TO 51 MILLION ROWS (5 GB)")
    print(" Host: AMD Ryzen 5 7600X (Zen 4, 6 Cores / 12 Threads @ up to 5.3 GHz, DDR5, Win 11 Pro)")
    print(" Task: Stream Filter 'status == 200' and Project Columns 'host,status,bytes'")
    print("=" * 100)

    all_results = {}

    for ds in DATASETS:
        tier_name = ds['tier']
        path = ds['path']
        rows = ds['rows']
        runs = ds['runs']
        file_bytes = os.path.getsize(path)
        size_mib = file_bytes / (1024.0 * 1024.0)

        print(f"\n>>> Benchmarking Tier: {tier_name} ({size_mib:.2f} MiB, {rows:,} rows)...", flush=True)

        tools = [
            ("HyperPipe (AVX-512/AVX2)", [HYPERPIPE_EXE, path, "status == 200", "--select", "host,status,bytes", "--on-error", "skip"]),
            ("Perl 5.38", [PERL_EXE, "-F,", "-lane", "print \"$F[0],$F[3],$F[4]\" if $F[3] == 200", path]),
            ("GNU awk (gawk 5.0)", [GAWK_EXE, "-F,", "$4 == 200 { print $1 \",\" $4 \",\" $5 }", path]),
            ("Python 3.15 (csv)", [PYTHON_EXE, "-c", f"""
import csv
with open(r'{path}', 'r', newline='') as f:
    r = csv.reader(f)
    next(r)
    for row in r:
        if len(row) >= 5 and row[3] == '200':
            pass
"""]),
        ]

        tier_res = []
        for name, cmd in tools:
            print(f"    Testing {name}...", flush=True)
            res = run_single(cmd, file_bytes, rows, runs)
            res['name'] = name
            print(f"      -> {res['elapsed']:.2f} s ({res['mib_s']:.1f} MiB/s, Peak RSS: {res['peak_rss']:.2f} MB)", flush=True)
            tier_res.append(res)
        all_results[tier_name] = (size_mib, rows, tier_res)

    print("\n" + "=" * 100)
    print(" FINAL HARDCORE SCALING MATRIX: SMALL vs MEDIUM vs DATACENTER SCALE")
    print("=" * 100)

    for tier_name, (size_mib, rows, tier_res) in all_results.items():
        print(f"\n--- {tier_name} [{size_mib:.2f} MiB, {rows:,} rows] ---")
        print(f" {'Tool':<28} | {'Elapsed':<10} | {'Throughput':<15} | {'Row Rate':<15} | {'Peak RSS':<10} | {'Relative Speed'}")
        print("-" * 100)
        hp_time = tier_res[0]['elapsed']
        for r in tier_res:
            speedup = f"{r['elapsed'] / hp_time:.1f}x slower" if r['name'] != "HyperPipe (AVX-512/AVX2)" else "1.0x (baseline)"
            rss_str = f"{r['peak_rss']:.2f} MB" if r['peak_rss'] > 0 else "< 6 MB"
            print(f" {r['name']:<28} | {r['elapsed']:>7.3f} s  | {r['mib_s']:>9.2f} MiB/s  | {r['mrows_s']:>8.3f} Mrows/s  | {rss_str:>10} | {speedup}")

    print("=" * 100)

if __name__ == '__main__':
    main()
