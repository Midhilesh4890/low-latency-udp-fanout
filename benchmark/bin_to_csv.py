#!/usr/bin/env python3
import csv
import struct
import sys
from pathlib import Path

RECORD = struct.Struct("<QQQ")

def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: bin_to_csv.py BINARY CSV")
    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    data = source.read_bytes()
    if len(data) % RECORD.size != 0:
        raise SystemExit("binary latency file has a partial record")
    with target.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("seq", "latency_ns", "send_ts_ns"))
        for seq_id, send_ts_ns, recv_ts_ns in RECORD.iter_unpack(data):
            latency_ns = recv_ts_ns - send_ts_ns if recv_ts_ns > send_ts_ns else 0
            writer.writerow((seq_id, latency_ns, send_ts_ns))

if __name__ == "__main__":
    main()
