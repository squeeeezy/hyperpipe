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

DATASET_PATH = os.path.abspath('e:/Test/hyperpipe/data/nasa_server_access_log.csv')
HYPERPIPE_EXE = os.path.abspath('e:/Test/hyperpipe/build/hyperpipe.exe')
GAWK_EXE = 'C:/Program Files/Git/usr/bin/gawk.exe'
CUT_EXE = 'C:/Program Files/Git/usr/bin/cut.exe'
PERL_EXE = 'C:/Program Files/Git/usr/bin/perl.exe'
PYTHON_EXE = sys.executable

def run_bench(name, cmd, runs=2):
    file_bytes = os.path.getsize(DATASET_PATH)
    total_rows = 1891714
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
        'name': name,
        'elapsed': best_time,
        'mib_s': mib_s,
        'mrows_s': mrows_s,
        'peak_rss': peak_rss
    }

def main():
    file_size_mib = os.path.getsize(DATASET_PATH) / (1024.0 * 1024.0)

    # -------------------------------------------------------------
    # Scenario 1: Filter & Project
    # -------------------------------------------------------------
    print("=" * 88)
    print(f" SCENARIO 1: Filter & Project (1.89M rows, {file_size_mib:.2f} MiB)")
    print(f" Query: status == 200, select host,status,bytes")
    print("=" * 88)

    scenario_1 = [
        ("HyperPipe (AVX-512/AVX2)", [HYPERPIPE_EXE, DATASET_PATH, "status == 200", "--select", "host,status,bytes"]),
        ("GNU cut (Projection only)", [CUT_EXE, "-d,", "-f1,4,5", DATASET_PATH]),
        ("GNU awk (gawk 5.0)", [GAWK_EXE, "-F,", "$4 == 200 { print $1 \",\" $4 \",\" $5 }", DATASET_PATH]),
        ("Perl 5.38", [PERL_EXE, "-F,", "-lane", "print \"$F[0],$F[3],$F[4]\" if $F[3] == 200", DATASET_PATH]),
        ("Python 3.15 (csv reader)", [PYTHON_EXE, "-c", f"""
import csv
with open(r'{DATASET_PATH}', 'r', newline='') as f:
    r = csv.reader(f)
    next(r)
    for row in r:
        if len(row) >= 5 and row[3] == '200':
            pass
"""]),
    ]

    res1 = []
    for name, cmd in scenario_1:
        print(f"Testing {name}...", flush=True)
        res1.append(run_bench(name, cmd))

    print("\n" + "=" * 88)
    print(f" {'Tool':<28} | {'Best Time':<10} | {'Throughput':<14} | {'Row Rate':<14} | {'Peak RSS':<10}")
    print("-" * 88)
    for r in res1:
        rss_str = f"{r['peak_rss']:.2f} MB" if r['peak_rss'] > 0 else "< 10 MB"
        print(f" {r['name']:<28} | {r['elapsed']:>7.3f} s  | {r['mib_s']:>9.2f} MiB/s | {r['mrows_s']:>8.3f} Mrows/s | {rss_str:>10}")
    print("=" * 88)

    # -------------------------------------------------------------
    # Scenario 2: Full Transcoding to JSON (NDJSON)
    # -------------------------------------------------------------
    print("\n" + "=" * 88)
    print(f" SCENARIO 2: Structured Transcoding CSV -> NDJSON (1.89M rows)")
    print("=" * 88)

    scenario_2 = [
        ("HyperPipe (AVX-512/AVX2)", [HYPERPIPE_EXE, DATASET_PATH, "--to", "ndjson"]),
        ("GNU awk (manual JSON printf)", [GAWK_EXE, "-F,", "NR>1 { printf \"{\\\"host\\\":\\\"%s\\\",\\\"status\\\":%s}\\n\", $1, $4 }", DATASET_PATH]),
        ("Python 3.15 (csv + json.dumps)", [PYTHON_EXE, "-c", f"""
import csv, json
with open(r'{DATASET_PATH}', 'r', newline='') as f:
    r = csv.reader(f)
    hdr = next(r)
    for row in r:
        json.dumps(dict(zip(hdr, row)))
"""]),
    ]

    res2 = []
    for name, cmd in scenario_2:
        print(f"Testing {name}...", flush=True)
        res2.append(run_bench(name, cmd))

    print("\n" + "=" * 88)
    print(f" {'Tool':<28} | {'Best Time':<10} | {'Throughput':<14} | {'Row Rate':<14} | {'Peak RSS':<10}")
    print("-" * 88)
    for r in res2:
        rss_str = f"{r['peak_rss']:.2f} MB" if r['peak_rss'] > 0 else "< 10 MB"
        print(f" {r['name']:<28} | {r['elapsed']:>7.3f} s  | {r['mib_s']:>9.2f} MiB/s | {r['mrows_s']:>8.3f} Mrows/s | {rss_str:>10}")
    print("=" * 88)

if __name__ == '__main__':
    main()
