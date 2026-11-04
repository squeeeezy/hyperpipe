#!/usr/bin/env python3
"""Turns the NASA access log CSV into nested NDJSON events, the shape that
logging pipelines write:

  {"time": "...", "client": {"host": "..."},
   "http": {"request": {"method": "GET", "path": "/..."}, "status": 200, "bytes": 6245},
   "tags": ["..."]}

usage: make_events.py big.csv events.ndjson
"""
import csv
import json
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, newline="", encoding="utf-8", errors="replace") as f, \
         open(dst, "w", encoding="utf-8", newline="\n") as out:
        for row in csv.DictReader(f):
            parts = row["request"].split(" ")
            method = parts[0] if len(parts) > 1 else ""
            path = parts[1] if len(parts) > 1 else row["request"]
            ext = path.rsplit(".", 1)[-1] if "." in path.rsplit("/", 1)[-1] else ""
            event = {
                "time": row["timestamp"],
                "client": {"host": row["host"]},
                "http": {
                    "request": {"method": method, "path": path},
                    "status": int(row["status"]),
                    "bytes": int(row["bytes"]) if row["bytes"].isdigit() else None,
                },
                "tags": [ext] if ext else [],
            }
            out.write(json.dumps(event, separators=(",", ":"), ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
