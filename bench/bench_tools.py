#!/usr/bin/env python3
"""Compare HyperPipe with other tools on the same tasks (POSIX only).

Each task gives every tool the command that produces the same result. The
outputs are parsed and compared by value, since tools differ in quoting
and number formatting; a tool whose result differs is reported as DIFF and
not timed further. Runs interleave the tools round by round; the table
shows the median time and the peak RSS of the process.

usage: bench_tools.py --csv big.csv --ndjson big.ndjson --small small.csv
                      [--events events.ndjson] [-n 7] [--out-dir /dev/shm]
                      [--hyperpipe PATH] [--only SUBSTRING]
"""
import argparse
import csv
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

csv.field_size_limit(1 << 30)


GNU_TIME = "/usr/bin/time"


def run(cmd, out_path, stdout_to_file):
    """Runs cmd and returns (seconds, peak RSS in MiB). GNU time reports the
    peak RSS: a child forked from this process starts with its memory
    counters, so wait4() alone would overstate small tools."""
    rss_file = out_path + ".rss"
    wrapped = [GNU_TIME, "-f", "%M", "-o", rss_file] + cmd if os.path.exists(GNU_TIME) else cmd
    out = open(out_path, "wb") if stdout_to_file else subprocess.DEVNULL
    t0 = time.perf_counter()
    p = subprocess.run(wrapped, stdout=out, stderr=subprocess.PIPE)
    dt = time.perf_counter() - t0
    if stdout_to_file:
        out.close()
    if p.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed: {p.stderr.decode(errors='replace').strip()[:300]}")
    rss_kib = 0.0
    if os.path.exists(rss_file):
        with open(rss_file) as f:
            rss_kib = float(f.read().split()[-1])
        os.remove(rss_file)
    return dt, rss_kib / 1024.0


# Results are reduced to a row count and a hash of the rows' values, read
# as a stream, so even large outputs need little memory to compare.

def digest_rows(rows):
    h = hashlib.sha256()
    n = 0
    for row in rows:
        h.update("\x1f".join(row).encode("utf-8", "replace"))
        h.update(b"\x1e")
        n += 1
    return n, h.hexdigest()


def read_csv_rows(path):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return digest_rows(r for r in csv.reader(f) if r)


def read_ndjson_rows(path):
    def rows(f):
        for line in f:
            line = line.strip()
            if line:
                obj = json.loads(line)
                yield [x for k, v in obj.items() for x in (k, "" if v is None else str(v))]
    with open(path, encoding="utf-8", errors="replace") as f:
        return digest_rows(rows(f))


def read_count(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        numbers = [w for w in f.read().split() if w.isdigit()]
    return (1, numbers[-1]) if numbers else (0, "")


def read_groups(path):
    """Grouped results without the header, in any row order and with any
    number formatting: tools name their columns and print numbers in their
    own ways."""
    def cell(x):
        try:
            return "%.12g" % float(x)
        except ValueError:
            return x
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rows = [r for r in csv.reader(f) if r][1:]
    return digest_rows(sorted([cell(x) for x in r] for r in rows))


class Approx:
    """Grouped results whose numbers may differ by 2%: quantiles that one
    tool estimates and another computes exactly."""
    def __init__(self, rows):
        self.rows = rows

    def __getitem__(self, i):
        return len(self.rows) if i == 0 else None

    def __eq__(self, other):
        def close(a, b):
            try:
                x, y = float(a), float(b)
            except ValueError:
                return a == b
            return abs(x - y) <= 0.02 * max(abs(x), abs(y)) + 1e-9
        return len(self.rows) == len(other.rows) and all(
            len(r) == len(s) and all(close(a, b) for a, b in zip(r, s))
            for r, s in zip(self.rows, other.rows))


def read_approx(path):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return Approx(sorted([r for r in csv.reader(f) if r][1:]))


READERS = {"csv": read_csv_rows, "ndjson": read_ndjson_rows, "count": read_count,
           "groups": read_groups, "approx": read_approx}


# DuckDB infers the mixed numbers-and-"-" bytes column as JSON and would
# write it back as JSON text; the schema says what the column is.
NASA_COLUMNS = ("{host: 'VARCHAR', timestamp: 'VARCHAR', request: 'VARCHAR', "
                "status: 'BIGINT', bytes: 'VARCHAR'}")


# A sort over every column, so no two tools may break ties differently;
# "-" in bytes is not a number and goes after the numbers.
SORT_HP = "-bytes,host,timestamp,request"
SORT_SQL = "TRY_CAST(bytes AS BIGINT) DESC NULLS LAST, host, timestamp, request"


# DuckDB 2.0 pushes the cast into its CSV reader, where TRY_CAST fails on
# "-" like CAST does, so the "-" rows are filtered out first
QUANTILES_SQL = ", ".join(f"quantile_cont(CAST(bytes AS BIGINT), {p}) AS p{int(p * 100)}"
                          for p in (0.5, 0.95, 0.99))

# The common log format timestamp as a time, in SQL
CLF_SQL = "strptime(timestamp, '%d/%b/%Y:%H:%M:%S %z')"


def tasks(a, hp, out):
    """Each task: name, output kind, {tool: (argv, writes_stdout)}."""
    q = lambda s: s.replace("'", "''")
    big, nd, small, ev = a.csv, a.ndjson, a.small, a.events
    return [
        ("filter + project, CSV to CSV", "csv", {
            "hyperpipe": ([hp, big, "status == 200", "--select", "host,status,bytes", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "status == 200", "--select", "host,status,bytes", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT host, status, bytes FROM _t_1 WHERE status = 200", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT host, status, bytes FROM read_csv('{q(big)}') WHERE status = 200) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "filter", "$status == 200", "then", "cut", "-o", "-f", "host,status,bytes", big], True),
        }),
        ("CSV to NDJSON", "ndjson", {
            "hyperpipe": ([hp, big, "--to", "ndjson", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--to", "ndjson", "--jobs", "1", "-o", out], False),
            "qsv tojsonl": (["qsv", "tojsonl", big, "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT * FROM read_csv('{q(big)}', all_varchar = true)) TO '{q(out)}' (FORMAT json)"], False),
            "miller": (["mlr", "--icsv", "--ojsonl", "cat", big], True),
        }),
        ("NDJSON to CSV", "csv", {
            "hyperpipe": ([hp, nd, "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, nd, "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv jsonl": (["qsv", "jsonl", nd, "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT * FROM read_json('{q(nd)}', columns = {NASA_COLUMNS})) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--ijsonl", "--ocsv", "cat", nd], True),
            "jq": (["jq", "-rn", '["host", "timestamp", "request", "status", "bytes"], (inputs | [.host, .timestamp, .request, .status, .bytes]) | @csv', nd], True),
        }),
        ("NDJSON filter + project", "ndjson", {
            "hyperpipe": ([hp, nd, "status == 404", "--select", "host,request", "--to", "ndjson", "-o", out], False),
            "hyperpipe -j1": ([hp, nd, "status == 404", "--select", "host,request", "--to", "ndjson", "--jobs", "1", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT host, request FROM read_json('{q(nd)}', columns = {NASA_COLUMNS}) WHERE status = 404) TO '{q(out)}' (FORMAT json)"], False),
            "miller": (["mlr", "--ijsonl", "--ojsonl", "filter", "$status == 404", "then", "cut", "-o", "-f", "host,request", nd], True),
            "jq": (["jq", "-c", "select(.status == 404) | {host, request}", nd], True),
        }),
        ("count matching rows", "count", {
            "hyperpipe": ([hp, big, "status == 404", "--count"], True),
            "hyperpipe -j1": ([hp, big, "status == 404", "--count", "--jobs", "1"], True),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT count(*) AS n FROM _t_1 WHERE status = 404"], True),
            "duckdb": (["duckdb", "-noheader", "-list", "-c", f"SELECT count(*) FROM read_csv('{q(big)}') WHERE status = 404"], True),
            "miller": (["mlr", "--icsv", "--onidx", "filter", "$status == 404", "then", "count", big], True),
        }),
        ("group by status: count, sum(bytes)", "groups", {
            "hyperpipe": ([hp, big, "--group-by", "status", "--agg", "count, sum(bytes)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--group-by", "status", "--agg", "count, sum(bytes)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT status, count(*) AS n, sum(TRY_CAST(bytes AS BIGINT)) AS b FROM _t_1 GROUP BY status", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT status, count(*), sum(TRY_CAST(bytes AS BIGINT)) FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) GROUP BY status) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "stats1", "-a", "count,sum", "-f", "bytes", "-g", "status", big], True),
        }),
        ("group by host: count", "groups", {
            "hyperpipe": ([hp, big, "--group-by", "host", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--group-by", "host", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT host, count(*) AS n FROM _t_1 GROUP BY host", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT host, count(*) FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) GROUP BY host) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "count", "-g", "host", big], True),
        }),
        ("NDJSON group by status: count, avg(bytes)", "groups", {
            "hyperpipe": ([hp, nd, "--group-by", "status", "--agg", "count, avg(bytes)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, nd, "--group-by", "status", "--agg", "count, avg(bytes)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT status, count(*), avg(TRY_CAST(bytes AS BIGINT)) FROM read_json('{q(nd)}', columns = {NASA_COLUMNS}) GROUP BY status) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--ijsonl", "--ocsv", "stats1", "-a", "count,mean", "-f", "bytes", "-g", "status", nd], True),
        }),
        ("distinct hosts per status", "groups", {
            "hyperpipe": ([hp, big, "--group-by", "status", "--agg", "count_distinct(host)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--group-by", "status", "--agg", "count_distinct(host)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT status, count(DISTINCT host) AS n FROM _t_1 GROUP BY status", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT status, count(DISTINCT host) FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) GROUP BY status) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "count-distinct", "-f", "status,host", "then", "count", "-g", "status", big], True),
        }),
        ("p50, p95, p99 of bytes per status", "approx", {
            "hyperpipe": ([hp, big, "bytes >= 0", "--group-by", "status", "--agg", "p50(bytes), p95(bytes), p99(bytes)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "bytes >= 0", "--group-by", "status", "--agg", "p50(bytes), p95(bytes), p99(bytes)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT status, {QUANTILES_SQL} FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) WHERE bytes <> '-' GROUP BY status) TO '{q(out)}' (HEADER)"], False),
            "qsv sqlp": (["qsv", "sqlp", big, f"SELECT status, {QUANTILES_SQL} FROM _t_1 WHERE bytes <> '-' GROUP BY status", "-o", out], False),
        }),
        ("count per hour", "groups", {
            "hyperpipe": ([hp, big, "--group-by", "hour(timestamp)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--group-by", "hour(timestamp)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, f"SELECT strftime({CLF_SQL}, '%Y-%m-%d %H:00') AS h, count(*) AS n FROM _t_1 GROUP BY h", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"SET TimeZone = 'UTC'; COPY (SELECT strftime(date_trunc('hour', {CLF_SQL}), '%Y-%m-%d %H:00') AS h, count(*) FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) GROUP BY h) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "put", "-q", "@n[strftime(strptime($timestamp, \"%d/%b/%Y:%H:%M:%S %z\"), \"%Y-%m-%d %H:00\")] += 1; end { emit @n, \"h\" }", big], True),
        }),
        ("count in a time range", "count", {
            "hyperpipe": ([hp, big, "timestamp >= 1995-07-01T04:30Z and timestamp < 1995-07-01T05:00Z", "--count"], True),
            "hyperpipe -j1": ([hp, big, "timestamp >= 1995-07-01T04:30Z and timestamp < 1995-07-01T05:00Z", "--count", "--jobs", "1"], True),
            "qsv sqlp": (["qsv", "sqlp", big, f"SELECT count(*) AS n FROM _t_1 WHERE {CLF_SQL} >= '1995-07-01 04:30:00+00:00' AND {CLF_SQL} < '1995-07-01 05:00:00+00:00'"], True),
            "duckdb": (["duckdb", "-noheader", "-list", "-c", f"SELECT count(*) FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) WHERE {CLF_SQL} >= TIMESTAMPTZ '1995-07-01 04:30:00+00' AND {CLF_SQL} < TIMESTAMPTZ '1995-07-01 05:00:00+00'"], True),
            "miller": (["mlr", "--icsv", "--onidx", "filter", "t = strptime($timestamp, \"%d/%b/%Y:%H:%M:%S %z\"); t >= 804573000 && t < 804574800", "then", "count", big], True),
        }),
        ("computed column: kb = bytes / 1024", "groups", {
            "hyperpipe": ([hp, big, "bytes >= 0", "--add", "kb = bytes / 1024", "--select", "host,kb", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "bytes >= 0", "--add", "kb = bytes / 1024", "--select", "host,kb", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, "SELECT host, CAST(bytes AS DOUBLE) / 1024 AS kb FROM _t_1 WHERE bytes <> '-'", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT host, CAST(bytes AS BIGINT) / 1024 AS kb FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) WHERE bytes <> '-') TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "filter", "$bytes != \"-\"", "then", "put", "$kb = $bytes / 1024", "then", "cut", "-o", "-f", "host,kb", big], True),
        }),
    ] + ([] if not ev else [
        ("nested NDJSON: filter on a path", "csv", {
            "hyperpipe": ([hp, ev, "http.status == 404", "--select", "time,client.host,http.request.path", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, ev, "http.status == 404", "--select", "time,client.host,http.request.path", "--to", "csv", "--jobs", "1", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT time, client.host AS \"client.host\", http.request.path AS \"http.request.path\" FROM read_json('{q(ev)}') WHERE http.status = 404) TO '{q(out)}' (HEADER)"], False),
            "jq": (["jq", "-rn", "[\"time\", \"client.host\", \"http.request.path\"], (inputs | select(.http.status == 404) | [.time, .client.host, .http.request.path]) | @csv", ev], True),
            "miller": (["mlr", "--ijsonl", "--ocsv", "flatten", "then", "filter", "${http.status} == 404", "then", "cut", "-o", "-f", "time,client.host,http.request.path", ev], True),
        }),
        ("nested NDJSON: group by a path", "groups", {
            "hyperpipe": ([hp, ev, "--group-by", "http.request.method", "--agg", "count(http.bytes), sum(http.bytes)", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, ev, "--group-by", "http.request.method", "--agg", "count(http.bytes), sum(http.bytes)", "--to", "csv", "--jobs", "1", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT http.request.method, count(http.bytes), sum(http.bytes) FROM read_json('{q(ev)}') GROUP BY 1) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--ijsonl", "--ocsv", "flatten", "then", "stats1", "-a", "count,sum", "-f", "http.bytes", "-g", "http.request.method", ev], True),
        }),
    ]) + [
        ("sort by bytes, top 10", "csv", {
            "hyperpipe": ([hp, big, "--sort", SORT_HP, "--head", "10", "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--sort", SORT_HP, "--head", "10", "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, f"SELECT * FROM _t_1 ORDER BY {SORT_SQL} LIMIT 10", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT * FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) ORDER BY {SORT_SQL} LIMIT 10) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "sort", "-nr", "bytes", "-f", "host,timestamp,request", "then", "head", "-n", "10", big], True),
        }),
        ("sort CSV by bytes", "csv", {
            "hyperpipe": ([hp, big, "--sort", SORT_HP, "--to", "csv", "-o", out], False),
            "hyperpipe -j1": ([hp, big, "--sort", SORT_HP, "--to", "csv", "--jobs", "1", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", big, f"SELECT * FROM _t_1 ORDER BY {SORT_SQL}", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT * FROM read_csv('{q(big)}', columns = {NASA_COLUMNS}) ORDER BY {SORT_SQL}) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "sort", "-nr", "bytes", "-f", "host,timestamp,request", big], True),
        }),
        ("startup: filter a 5,000-row file", "csv", {
            "hyperpipe": ([hp, small, "status == 404", "--to", "csv", "-o", out], False),
            "qsv sqlp": (["qsv", "sqlp", small, "SELECT * FROM _t_1 WHERE status = 404", "-o", out], False),
            "duckdb": (["duckdb", "-c", f"COPY (SELECT * FROM read_csv('{q(small)}') WHERE status = 404) TO '{q(out)}' (HEADER)"], False),
            "miller": (["mlr", "--icsv", "--ocsv", "filter", "$status == 404", small], True),
        }),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--ndjson", required=True)
    ap.add_argument("--small", required=True)
    ap.add_argument("--events", default=None, help="nested NDJSON from make_events.py")
    ap.add_argument("-n", type=int, default=7)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--hyperpipe", default="hyperpipe")
    ap.add_argument("--only", default="", help="run the tasks whose name contains this")
    a = ap.parse_args()
    a.csv, a.ndjson, a.small = map(os.path.abspath, (a.csv, a.ndjson, a.small))
    a.events = a.events and os.path.abspath(a.events)
    hp = os.path.abspath(a.hyperpipe) if os.sep in a.hyperpipe else shutil.which(a.hyperpipe)

    tmp = tempfile.mkdtemp(prefix="hptools_", dir=a.out_dir)
    placeholder = os.path.join(tmp, "OUT")
    for name, kind, tools in tasks(a, hp, placeholder):
        if a.only not in name:
            continue
        print(f"\n## {name}\n", flush=True)
        ref = None
        live = {}
        for tool, (argv, to_stdout) in tools.items():
            out = os.path.join(tmp, tool.replace(" ", "_"))
            argv = [x.replace(placeholder, out) for x in argv]
            if shutil.which(argv[0]) is None and not os.path.exists(argv[0]):
                print(f"| {tool} | not installed |")
                continue
            try:
                run(argv, out, to_stdout)
            except RuntimeError as e:
                print(f"| {tool} | failed: {e} |")
                continue
            got = READERS[kind](out)
            if ref is None:
                ref = got
            if got != ref:
                print(f"| {tool} | DIFF: {got[0]} rows vs {ref[0]}, values differ |")
                continue
            live[tool] = (argv, out, to_stdout)

        if not live:
            continue
        times = {t: [] for t in live}
        rss = {t: 0.0 for t in live}
        for _ in range(a.n):
            for tool, (argv, out, to_stdout) in live.items():
                dt, mem = run(argv, out, to_stdout)
                times[tool].append(dt)
                rss[tool] = max(rss[tool], mem)
        fastest = min(statistics.median(v) for v in times.values())
        print("| Tool | Time | vs fastest | Peak RSS |")
        print("|---|---:|---:|---:|")
        for tool in sorted(live, key=lambda t: statistics.median(times[t])):
            med = statistics.median(times[tool])
            print(f"| {tool} | {med * 1000:,.0f} ms | {med / fastest:.1f}x | {rss[tool]:,.0f} MiB |", flush=True)
        for f in os.listdir(tmp):
            os.remove(os.path.join(tmp, f))
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
