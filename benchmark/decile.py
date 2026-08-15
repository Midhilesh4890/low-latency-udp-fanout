#!/usr/bin/env python3
import argparse
import csv
import json
import math
from pathlib import Path


DEFAULT_WARMUP = 20000
BUCKET_COUNT = 10
STALL_P50_MULTIPLE = 3.0
STALL_MAX_MULTIPLE = 8.0
STALL_FLAT_P50_RATIO = 6.0
RAMP_MIN_RATIO = 2.0
RAMP_MIN_NONDECREASING_STEPS = 7
RAMP_DROP_TOLERANCE = 0.05
BACKLOG_MIN_TO_FASTEST_RATIO = 10.0
BACKLOG_FLAT_P50_RATIO = 1.5


def parse_args():
    parser = argparse.ArgumentParser(description="Print latency decile diagnostics.")
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--skip-warmup", type=int, default=None)
    parser.add_argument("--compare", nargs="+", type=Path)
    return parser.parse_args()


def find_latency_csv(path):
    if path.is_file():
        if path.name != "latency.csv":
            raise SystemExit(f"input file is not latency.csv: {path}")
        return path
    if not path.is_dir():
        raise SystemExit(f"input path does not exist: {path}")
    matches = sorted(path.glob("rx_*/latency.csv"))
    if not matches:
        raise SystemExit(f"run directory has no rx_*/latency.csv: {path}")
    if len(matches) > 1:
        raise SystemExit(f"run directory has multiple rx_*/latency.csv files: {path}")
    return matches[0]


def run_dir_for(latency_csv):
    parent = latency_csv.parent
    if parent.name.startswith("rx_"):
        return parent.parent
    return parent


def warmup_from_run_json(latency_csv):
    run_json = run_dir_for(latency_csv) / "run.json"
    if not run_json.exists():
        return None
    with run_json.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    for key in ("warmup", "warmup_count", "skip_warmup"):
        if key in data:
            return int(data[key])
    if "parameters" in data and "warmup" in data["parameters"]:
        return int(data["parameters"]["warmup"])
    return None


def resolve_warmup(latency_csv, override):
    if override is not None:
        if override < 0:
            raise SystemExit("--skip-warmup must be nonnegative")
        return override
    value = warmup_from_run_json(latency_csv)
    if value is None:
        return DEFAULT_WARMUP
    if value < 0:
        raise SystemExit(f"run.json warmup must be nonnegative: {value}")
    return value


def read_latencies_us(latency_csv, warmup):
    values = []
    with latency_csv.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if "latency_ns" not in (reader.fieldnames or []):
            raise SystemExit(f"latency.csv missing latency_ns column: {latency_csv}")
        for row in reader:
            values.append(float(row["latency_ns"]) / 1000.0)
    if warmup >= len(values):
        raise SystemExit(f"warmup {warmup} leaves no rows in {latency_csv}")
    return values[warmup:]


def percentile(values, percent):
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * (percent / 100.0)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def split_buckets(values, count):
    length = len(values)
    buckets = []
    for index in range(count):
        start = (index * length) // count
        end = ((index + 1) * length) // count
        buckets.append(values[start:end])
    return buckets


def bucket_stats(latencies_us):
    buckets = split_buckets(latencies_us, BUCKET_COUNT)
    rows = []
    for index, bucket in enumerate(buckets, start=1):
        if len(bucket) == 0:
            raise SystemExit("not enough post-warmup rows for ten buckets")
        rows.append(
            {
                "index": index,
                "rows": int(len(bucket)),
                "p50": float(percentile(bucket, 50)),
                "p90": float(percentile(bucket, 90)),
                "p99": float(percentile(bucket, 99)),
                "max": float(max(bucket)),
            }
        )
    return rows


def ratio(a, b):
    if b == 0:
        return float("inf")
    return a / b


def verdict_for(rows, latencies_us):
    p50s = [row["p50"] for row in rows]
    maxes = [row["max"] for row in rows]
    global_p50 = float(percentile(latencies_us, 50))
    ramp_ratio = ratio(float(p50s[-1]), float(p50s[0]))
    highest_max_index = int(rows[max(range(len(maxes)), key=maxes.__getitem__)]["index"])
    tail_fraction = float(sum(1 for value in latencies_us if value > (global_p50 * 10.0)) / len(latencies_us))
    p50_flat_ratio = ratio(float(max(p50s)), float(min(p50s)))
    fastest_latency = float(min(latencies_us))
    backlog_ratio = ratio(float(min(p50s)), fastest_latency)
    allowed_drop = float(min(p50s)) * RAMP_DROP_TOLERANCE
    nondecreasing_steps = int(sum(1 for previous, current in zip(p50s, p50s[1:]) if current - previous >= -allowed_drop))
    stall_p50_limit = float(percentile(p50s, 50)) * STALL_P50_MULTIPLE
    stall_bucket_indexes = [int(rows[index]["index"]) for index, value in enumerate(p50s) if value >= stall_p50_limit]
    rest_p50s = [rows[index]["p50"] for index, value in enumerate(p50s) if value < stall_p50_limit]
    rest_flat_ratio = ratio(float(max(rest_p50s)), float(min(rest_p50s))) if len(rest_p50s) else float("inf")
    max_spike_limit = float(percentile(maxes, 50)) * STALL_MAX_MULTIPLE
    max_spike_indexes = [int(rows[index]["index"]) for index, value in enumerate(maxes) if value >= max_spike_limit]
    stall = 1 <= len(stall_bucket_indexes) <= 2 and rest_flat_ratio <= STALL_FLAT_P50_RATIO and 1 <= len(max_spike_indexes) <= 2
    ramp = ramp_ratio >= RAMP_MIN_RATIO and nondecreasing_steps >= RAMP_MIN_NONDECREASING_STEPS
    backlog = backlog_ratio >= BACKLOG_MIN_TO_FASTEST_RATIO and p50_flat_ratio <= BACKLOG_FLAT_P50_RATIO
    if stall:
        verdict = "STALL"
    elif ramp:
        verdict = "RAMP"
    elif backlog:
        verdict = "BACKLOG"
    elif ramp_ratio >= p50_flat_ratio:
        verdict = "RAMP"
    elif p50_flat_ratio <= BACKLOG_FLAT_P50_RATIO:
        verdict = "BACKLOG"
    else:
        verdict = "STALL"
    return {
        "global_p50": global_p50,
        "ramp_ratio": ramp_ratio,
        "highest_max_index": highest_max_index,
        "tail_fraction": tail_fraction,
        "p50_flat_ratio": p50_flat_ratio,
        "backlog_ratio": backlog_ratio,
        "nondecreasing_steps": nondecreasing_steps,
        "stall_p50_limit": stall_p50_limit,
        "stall_bucket_indexes": stall_bucket_indexes,
        "rest_flat_ratio": rest_flat_ratio,
        "max_spike_limit": max_spike_limit,
        "max_spike_indexes": max_spike_indexes,
        "verdict": verdict,
    }


def format_us(value):
    return f"{value:12.3f}"


def table_lines(label, latency_csv, warmup, rows, details):
    lines = [
        f"{label}",
        f"input: {latency_csv}",
        f"skip_warmup: {warmup}",
        f"{'bucket':>6} {'rows':>8} {'p50_us':>12} {'p90_us':>12} {'p99_us':>12} {'max_us':>12}",
    ]
    for row in rows:
        lines.append(
            f"{row['index']:6d} {row['rows']:8d} {format_us(row['p50'])} {format_us(row['p90'])} {format_us(row['p99'])} {format_us(row['max'])}"
        )
    stall_bucket_text = ",".join(str(index) for index in details["stall_bucket_indexes"]) if details["stall_bucket_indexes"] else "none"
    max_spike_text = ",".join(str(index) for index in details["max_spike_indexes"]) if details["max_spike_indexes"] else "none"
    lines.extend(
        [
            f"ramp_ratio: {details['ramp_ratio']:.6f}",
            f"highest_max_bucket: {details['highest_max_index']}",
            f"tail_fraction_gt_10x_global_p50: {details['tail_fraction']:.6f}",
            f"threshold STALL: 1<=localized_p50_buckets<=2 where bucket_p50_us>=median_bucket_p50_us*{STALL_P50_MULTIPLE:.1f}; rest_p50_flat_ratio<={STALL_FLAT_P50_RATIO:.2f}; 1<=max_spike_buckets<=2 where bucket_max_us>=median_bucket_max_us*{STALL_MAX_MULTIPLE:.1f}; localized_p50_buckets={stall_bucket_text}; rest_p50_flat_ratio={details['rest_flat_ratio']:.6f}; max_spike_buckets={max_spike_text}",
            f"threshold RAMP: ramp_ratio>={RAMP_MIN_RATIO:.2f}; nondecreasing_steps>={RAMP_MIN_NONDECREASING_STEPS}/9 with drop_tolerance={RAMP_DROP_TOLERANCE:.2%} of min_bucket_p50; nondecreasing_steps={details['nondecreasing_steps']}",
            f"threshold BACKLOG: min_bucket_p50/fastest_latency>={BACKLOG_MIN_TO_FASTEST_RATIO:.2f}; p50_flat_ratio<={BACKLOG_FLAT_P50_RATIO:.2f}; min_bucket_p50/fastest_latency={details['backlog_ratio']:.6f}; p50_flat_ratio={details['p50_flat_ratio']:.6f}",
            f"verdict: {details['verdict']}",
        ]
    )
    return lines


def analyze_path(path, warmup_override):
    latency_csv = find_latency_csv(path)
    warmup = resolve_warmup(latency_csv, warmup_override)
    latencies_us = read_latencies_us(latency_csv, warmup)
    rows = bucket_stats(latencies_us)
    details = verdict_for(rows, latencies_us)
    return {
        "path": path,
        "latency_csv": latency_csv,
        "warmup": warmup,
        "latencies_us": latencies_us,
        "rows": rows,
        "details": details,
    }


def print_single(path, warmup_override):
    result = analyze_path(path, warmup_override)
    print("\n".join(table_lines(f"run: {path}", result["latency_csv"], result["warmup"], result["rows"], result["details"])))


def print_compare(paths, warmup_override):
    results = []
    for path in paths:
        result = analyze_path(path, warmup_override)
        results.append(result)
        print("\n".join(table_lines(f"run: {path}", result["latency_csv"], result["warmup"], result["rows"], result["details"])))
        print()
    labels = [str(result["path"]) for result in results]
    label_width = max(12, max(len(label) for label in labels))
    print("bucket_p50_side_by_side_us")
    print(f"{'bucket':>6} " + " ".join(f"{label:>{label_width}}" for label in labels))
    for bucket_index in range(BUCKET_COUNT):
        values = " ".join(f"{result['rows'][bucket_index]['p50']:>{label_width}.3f}" for result in results)
        print(f"{bucket_index + 1:6d} {values}")


def main():
    args = parse_args()
    if args.compare:
        if args.paths:
            raise SystemExit("positional paths cannot be combined with --compare")
        if len(args.compare) < 2:
            raise SystemExit("--compare requires two or more run directories")
        for path in args.compare:
            if not path.is_dir():
                raise SystemExit(f"--compare accepts run directories only: {path}")
        print_compare(args.compare, args.skip_warmup)
        return
    if len(args.paths) != 1:
        raise SystemExit("provide one latency.csv or run directory, or use --compare with two or more run directories")
    print_single(args.paths[0], args.skip_warmup)


if __name__ == "__main__":
    main()
