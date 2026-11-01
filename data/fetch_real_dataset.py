#!/usr/bin/env python3
"""
Downloads and extracts the authentic NASA Kennedy Space Center WWW access log (July 1995)
into hyperpipe/data/nasa_server_access_log.csv (183.18 MB, 1,891,714 authentic rows).
"""
import os
import gzip
import csv
import urllib.request
import time

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(DATA_DIR, "nasa_server_access_log.csv")
GZ_URL = "https://ita.ee.lbl.gov/traces/NASA_access_log_Jul95.gz"

def ensure_dataset():
    if os.path.exists(CSV_PATH) and os.path.getsize(CSV_PATH) > 100 * 1024 * 1024:
        print(f"Authentic dataset already present: {CSV_PATH} ({os.path.getsize(CSV_PATH) / (1024*1024):.2f} MB)")
        return CSV_PATH

    print(f"Downloading authentic NASA access logs from {GZ_URL}...")
    t0 = time.time()
    req = urllib.request.Request(GZ_URL, headers={"User-Agent": "Mozilla/5.0"})
    gz_data = urllib.request.urlopen(req).read()
    print(f"Downloaded {len(gz_data) / (1024*1024):.2f} MB compressed in {time.time() - t0:.2f}s")

    print(f"Extracting and converting to RFC-4180 CSV: {CSV_PATH}...")
    decompressed = gzip.decompress(gz_data).decode("latin1", errors="replace")
    
    count = 0
    with open(CSV_PATH, "w", newline="", encoding="utf-8") as fout:
        writer = csv.writer(fout)
        writer.writerow(["host", "timestamp", "request", "status", "bytes"])
        for line in decompressed.splitlines():
            p1 = line.find(" - - [")
            if p1 == -1: continue
            host = line[:p1]
            p2 = line.find('] "', p1)
            if p2 == -1: continue
            ts = line[p1 + 6 : p2]
            p3 = line.rfind('" ')
            if p3 == -1: continue
            req = line[p2 + 3 : p3]
            rest = line[p3 + 2 :].strip().split()
            status = rest[0] if len(rest) > 0 else "200"
            bsize = rest[1] if len(rest) > 1 else "0"
            writer.writerow([host, ts, req, status, bsize])
            count += 1

    sz_mb = os.path.getsize(CSV_PATH) / (1024 * 1024)
    print(f"Ready: {count} authentic rows ({sz_mb:.2f} MB) in {CSV_PATH}")
    return CSV_PATH

if __name__ == "__main__":
    ensure_dataset()
