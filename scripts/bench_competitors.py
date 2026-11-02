#!/usr/bin/env python3
"""Competitive benchmark harness for HyperPipe.

Runs the same logical workload against every installed competitor, measures
wall-clock time (best of N) and peak RSS (VmHWM from /proc/<pid>/status), and
prints a ready-to-paste Markdown table.

Workloads (NASA-format access log, 5 columns: host,timestamp,request,status,bytes):
  W1  filter+project -> CSV : rows with status == 200, columns host,status,bytes
  W2  convert -> NDJSON     : full file, every row, JSON Lines out
  W3  parse+passthrough     : full file, all columns, CSV out

Usage: python3 scripts/bench_competitors.py <data.csv> [--rounds 3]
"""
import argparse
import os
import shutil
import subprocess
import sys

import time


def sample_rss_kib(pid, alive):
    """Poll /proc/<pid>/status until exit. VmHWM is the kernel's cumulative
    high-water mark, so sparse polling still yields the exact peak."""
    path = f"/proc/{pid}/status"
    while alive[0]:
        try:
            with open(path, "rb") as f:
                for line in f:
                    if line.startswith(b"VmHWM:"):
                        return int(line.split()[1])
        except (FileNotFoundError, ProcessLookupError):
            break
        time.sleep(0.0005)
    return 0


DEVNULL_FD = os.open(os.devnull, os.O_WRONLY)


def run_once(cmd, measure_rss=True):
    """Run cmd with posix_spawn and reap it via os.wait4, whose rusage
    provides the kernel-accounted peak RSS (ru_maxrss, KiB on Linux)."""
    argv = [cmd[0]] + list(cmd[1:])
    t0 = time.perf_counter()
    pid = os.posix_spawn(
        argv[0], argv, os.environ,
        file_actions=[(os.POSIX_SPAWN_DUP2, DEVNULL_FD, 1),
                      (os.POSIX_SPAWN_DUP2, DEVNULL_FD, 2)],
    )
    _, status, rusage = os.wait4(pid, 0)
    dt = time.perf_counter() - t0
    rss = rusage.ru_maxrss if measure_rss else 0
    return dt, rss


def run_best(cmd, rounds):
    best_t, best_rss = float("inf"), 0
    for _ in range(rounds):
        dt, rss = run_once(cmd)
        if dt < best_t:
            best_t, best_rss = dt, rss
    return best_t, best_rss


def fmt_row(name, cmd, data_bytes, rounds, rss_ok=True):
    if shutil.which(cmd[0]) is None and not os.path.exists(cmd[0]):
        return None
    try:
        dt, rss = run_best(cmd, rounds)
    except (FileNotFoundError, PermissionError):
        return None
    if not rss_ok:
        rss = 0  # pipeline: wait4 sees the shell, not the workers
    return {
        "tool": name,
        "time": dt,
        "mib_s": data_bytes / (1024 * 1024) / dt,
        "rss_mib": rss / 1024.0 if rss else None,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--hp", default=None, help="path to hyperpipe binary")
    args = ap.parse_args()

    data = os.path.abspath(args.data)
    data_bytes = os.path.getsize(data)

    hp = args.hp or os.environ.get("HP_BIN")
    if not hp:
        here = os.path.dirname(os.path.abspath(__file__))
        hp = os.path.join(here, "..", "build", "hyperpipe")
    hp = os.path.abspath(hp)
    if not os.path.exists(hp):
        sys.exit(f"hyperpipe binary not found at {hp} (use --hp)")

    sh = shutil.which("sh") or "/bin/sh"
    tools = {
        "sh": sh,
        "duckdb": shutil.which("duckdb") or "/tmp/competitors/duckdb",
        "mlr": shutil.which("mlr") or "/tmp/competitors/mlr",
        "qsv": shutil.which("qsv") or "/tmp/competitors/qsv",
        "mawk": shutil.which("mawk"),
    }

    run_once([hp, data, "--to", "csv", "-o", "/dev/null"], measure_rss=False)  # warm cache

    results = {1: [], 2: [], 3: []}

    print("W1: filter status==200 + project -> CSV", file=sys.stderr)
    results[1].append(fmt_row("HyperPipe",
        [hp, data, "status == 200", "--select", "host,status,bytes", "--to", "csv", "-o", "/dev/null"],
        data_bytes, args.rounds))
    if tools["qsv"]:
        results[1].append(fmt_row("qsv",
            [tools["sh"], "-c", f'{tools["qsv"]} search -s status "^200$" {data} | {tools["qsv"]} select host,status,bytes -o /dev/null'],
            data_bytes, args.rounds))
    if tools["mlr"]:
        results[1].append(fmt_row("miller",
            [tools["mlr"], "--csv", "filter", "$status == 200", "then", "cut", "-f", "host,status,bytes", data],
            data_bytes, args.rounds))
    if tools["duckdb"]:
        results[1].append(fmt_row("duckdb",
            [tools["duckdb"], "-c",
             f"COPY (SELECT host, status, bytes FROM read_csv('{data}', header=true) WHERE status = 200) TO '/dev/null' (FORMAT CSV, HEADER)"],
            data_bytes, args.rounds))
    if tools["mawk"]:
        results[1].append(fmt_row("mawk",
            [tools["sh"], "-c", f"{tools['mawk']} -F, '$4==200 {{print $1\",\"$4\",\"$5}}' {data} > /dev/null"],
            data_bytes, args.rounds, rss_ok=False))

    print("W2: convert full file -> NDJSON", file=sys.stderr)
    results[2].append(fmt_row("HyperPipe",
        [hp, data, "--to", "ndjson", "-o", "/dev/null"], data_bytes, args.rounds))
    if tools["qsv"]:
        results[2].append(fmt_row("qsv",
            [tools["sh"], "-c", f'{tools["qsv"]} tojsonl {data} -o /dev/null'], data_bytes, args.rounds, rss_ok=False))
    if tools["mlr"]:
        results[2].append(fmt_row("miller",
            [tools["mlr"], "--icsv", "--ojsonl", "cat", data], data_bytes, args.rounds))
    if tools["duckdb"]:
        results[2].append(fmt_row("duckdb",
            [tools["duckdb"], "-c",
             f"COPY (SELECT * FROM read_csv('{data}', header=true)) TO '/dev/null' (FORMAT NDJSON)"],
            data_bytes, args.rounds))

    print("W3: parse+passthrough -> CSV", file=sys.stderr)
    results[3].append(fmt_row("HyperPipe",
        [hp, data, "--to", "csv", "-o", "/dev/null"], data_bytes, args.rounds))
    if tools["qsv"]:
        results[3].append(fmt_row("qsv",
            [tools["sh"], "-c", f'{tools["qsv"]} cat rows {data} -o /dev/null'], data_bytes, args.rounds, rss_ok=False))
    if tools["mlr"]:
        results[3].append(fmt_row("miller",
            [tools["mlr"], "--csv", "cat", data], data_bytes, args.rounds))
    if tools["duckdb"]:
        results[3].append(fmt_row("duckdb",
            [tools["duckdb"], "-c",
             f"COPY (SELECT * FROM read_csv('{data}', header=true)) TO '/dev/null' (FORMAT CSV, HEADER)"],
            data_bytes, args.rounds))
    if tools["mawk"]:
        results[3].append(fmt_row("mawk",
            [tools["sh"], "-c", f"{tools['mawk']} '{{ print }}' {data} > /dev/null"], data_bytes, args.rounds, rss_ok=False))

    print(f"\nDataset: {data} ({data_bytes/1048576:.1f} MiB), best of {args.rounds}, output to /dev/null\n")
    print("| Workload | Tool | Time (s) | MiB/s | Peak RSS (MiB) |")
    print("|---|---|---:|---:|---:|")
    for w, lst in results.items():
        for r in lst:
            if r:
                rss = f"{r['rss_mib']:.1f}" if r["rss_mib"] else "n/a"
                print(f"| W{w} | {r['tool']} | {r['time']:.4f} | {r['mib_s']:.0f} | {rss} |")


if __name__ == "__main__":
    main()
