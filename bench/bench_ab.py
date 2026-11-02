#!/usr/bin/env python3
"""A/B benchmark of two (or more) hyperpipe binaries on the same workloads.

Every workload is run best-of-N per binary; outputs are hashed and compared
so a speedup is only reported when the bytes are identical (or the column
is flagged DIFF). Cross-platform: works on Windows and POSIX.

usage: bench_ab.py --csv big.csv --ndjson big.ndjson [-n 5] BIN [BIN ...]
"""
import argparse
import hashlib
import os
import statistics
import subprocess
import sys
import tempfile
import time

# A raised priority keeps desktop background work from landing in the timings.
PRIORITY = getattr(subprocess, "HIGH_PRIORITY_CLASS", 0)

def workloads(csv, ndjson):
    return [
        ("W1 filter+project csv->csv", [csv, "status == 200", "--select", "host,status,bytes", "--to", "csv"]),
        ("W2 full csv->ndjson", [csv, "--to", "ndjson"]),
        ("W3 passthrough csv->csv", [csv, "--to", "csv"]),
        ("W4 count status==500", [csv, "status == 500", "--count"]),
        ("W5 contains filter", [csv, "request contains apollo", "--to", "csv"]),
        ("W6 numeric range", [csv, "bytes > 5000 and bytes < 20000", "--select", "host,bytes", "--to", "csv"]),
        ("W7 ndjson->csv", [ndjson, "--to", "csv"]),
        ("W8 ndjson filter", [ndjson, "status == 404", "--select", "host,request", "--to", "csv"]),
        ("W9 ndjson->ndjson", [ndjson, "--to", "ndjson"]),
        ("W10 summary", [csv, "--summary"]),
    ]


def run_once(binary, args, out_path):
    # Row output goes through -o; --count and --summary only print to stdout.
    to_stdout = "--count" in args or "--summary" in args
    cmd = [os.path.abspath(binary)] + args + ([] if to_stdout else ["-o", out_path])
    with open(out_path if to_stdout else os.devnull, "wb") as out:
        t0 = time.perf_counter()
        r = subprocess.run(cmd, stdout=out, stderr=subprocess.PIPE, creationflags=PRIORITY)
        dt = time.perf_counter() - t0
    if r.returncode != 0:
        raise RuntimeError(f"{binary} {args}: exit {r.returncode}: {r.stderr.decode(errors='replace')}")
    return dt


def digest(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--ndjson", required=True)
    ap.add_argument("-n", type=int, default=5)
    ap.add_argument("-k", "--filter", default="", help="substring of workload names to run")
    ap.add_argument("bins", nargs="+")
    a = ap.parse_args()

    sizes = {a.csv: os.path.getsize(a.csv), a.ndjson: os.path.getsize(a.ndjson)}
    names = [os.path.basename(b) for b in a.bins]
    print(f"{'workload':32}" + "".join(f"{n:>22}" for n in names) + "   speedup  same")
    tmp = tempfile.mkdtemp(prefix="hpab_")
    for label, args in workloads(a.csv, a.ndjson):
        if a.filter and a.filter not in label:
            continue
        mib = sizes[args[0]] / (1 << 20)
        outs = [os.path.join(tmp, f"out{i}") for i in range(len(a.bins))]
        rounds = []
        # Interleave the binaries in every round so background load drift
        # hits all of them alike instead of skewing whichever ran last.
        for _ in range(a.n):
            rounds.append([run_once(b, args, outs[i]) for i, b in enumerate(a.bins)])
        best = [min(r[i] for r in rounds) for i in range(len(a.bins))]
        # The speedup is the median of per-round ratios: each ratio compares
        # runs made seconds apart, which cancels slow drift better than
        # comparing the two global minimums.
        speedup = statistics.median(r[0] / r[-1] for r in rounds)
        hashes = [digest(o) for o in outs]
        cells = "".join(f"{t*1000:9.1f}ms {mib/t:7.0f}MiB/s" for t in best)
        same = "ok" if len(set(hashes)) == 1 else "DIFF"
        print(f"{label:32}{cells}   x{speedup:5.2f}  {same}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
