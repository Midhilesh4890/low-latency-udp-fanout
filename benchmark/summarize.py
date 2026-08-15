#!/usr/bin/env python3
import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path

DEFAULT_WARMUP = 20000
PERCENTILES = (50.0, 90.0, 99.0, 99.9, 99.99)
RATE_EPSILON = 0.005
RAMP_LIMIT = 2.0
FREEZE_LIMIT = 100.0
ACHIEVED_RATE_RATIO = 0.80


def parse_args():
    parser = argparse.ArgumentParser(description="Summarize benchmark latency runs.")
    parser.add_argument("root", type=Path)
    parser.add_argument("--skip-warmup", type=int, default=None)
    parser.add_argument("--min-rate", type=float, default=None)
    parser.add_argument("--max-rate", type=float, default=None)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def fail(message):
    raise SystemExit(message)


def read_run_json(path):
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        fail(f"missing run.json: {path}")
    except json.JSONDecodeError as error:
        fail(f"invalid run.json: {path}: {error}")


def run_dir_for(latency_csv):
    return latency_csv.parent.parent


def receiver_for(latency_csv):
    return latency_csv.parent.name


def discover_latency_csvs(root):
    if not root.exists():
        fail(f"input root does not exist: {root}")
    if root.is_file():
        if root.name != "latency.csv":
            fail(f"input file is not latency.csv: {root}")
        return [root]
    return sorted(root.rglob("rx_*/latency.csv"))


def warmup_from(data):
    parameters = data.get("parameters", {})
    for source in (data, parameters):
        for key in ("warmup", "warmup_count", "skip_warmup"):
            if key in source and source[key] is not None:
                return int(source[key])
    return DEFAULT_WARMUP


def offered_rate_from(data):
    parameters = data.get("parameters", {})
    value = parameters.get("rate", data.get("rate"))
    return None if value is None else float(value)


def expected_from(data, warmup):
    parameters = data.get("parameters", {})
    value = parameters.get("total_count", data.get("total_count"))
    if value is not None:
        return max(0, int(value) - int(warmup))
    value = parameters.get("count", data.get("count"))
    return None if value is None else int(value)


def read_samples(path, warmup):
    values = []
    send_timestamps = []
    has_send_timestamps = False
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            fieldnames = reader.fieldnames or []
            if "latency_ns" not in fieldnames:
                fail(f"latency.csv missing latency_ns column: {path}")
            has_send_timestamps = "send_ts_ns" in fieldnames
            for row in reader:
                try:
                    values.append(float(row["latency_ns"]))
                    if has_send_timestamps:
                        send_timestamps.append(float(row["send_ts_ns"]))
                except ValueError:
                    fail(f"latency.csv contains nonnumeric values: {path}")
    except OSError as error:
        fail(f"failed to read latency.csv: {path}: {error}")
    if warmup < 0:
        fail("--skip-warmup must be nonnegative")
    if warmup >= len(values):
        fail(f"warmup {warmup} leaves no rows in {path}")
    samples = {"latencies": values[warmup:], "send_timestamps": []}
    if has_send_timestamps:
        samples["send_timestamps"] = send_timestamps[warmup:]
    return samples


def percentile(values, percent):
    if not values:
        return float("nan")
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = (len(ordered) - 1) * (percent / 100.0)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return float(ordered[lower] * (1.0 - weight) + ordered[upper] * weight)


def split_deciles(values):
    length = len(values)
    if length < 10:
        fail("not enough post-warmup rows for ten deciles")
    buckets = []
    for index in range(10):
        start = int((index * length) / 10)
        end = int(((index + 1) * length) / 10)
        buckets.append(values[start:end])
    return buckets


def mean(values):
    return float(sum(values) / len(values)) if values else float("nan")


def ramp_slope(values):
    count = len(values)
    if count < 2:
        return 0.0
    x_mean = float((count - 1) / 2.0)
    y_mean = mean(values)
    numerator = 0.0
    denominator = 0.0
    for index, value in enumerate(values):
        x_delta = float(index) - x_mean
        numerator += x_delta * (float(value) - y_mean)
        denominator += x_delta * x_delta
    if denominator == 0.0:
        return 0.0
    return float(numerator / denominator)


def normalized_parameters(data):
    parameters = dict(data.get("parameters", {}))
    parameters.pop("outdir", None)
    return parameters


def config_label(root, group_dir):
    try:
        relative = group_dir.relative_to(root)
    except ValueError:
        return str(group_dir)
    if str(relative) == ".":
        return group_dir.name
    return str(relative)


def repeat_number(path):
    match = re.fullmatch("rep_([0-9]+)", path.name)
    return None if match is None else int(match.group(1))


def clock_invalid_for(data, measured_p50):
    clock = data.get("clock_sync") or {}
    method = clock.get("method")
    drift = clock.get("max_drift_ns")
    if method == "shared_clock":
        return False
    if method is None or method == "none":
        return True
    if drift is None:
        return True
    return float(drift) > abs(float(measured_p50)) * 0.10


def flags_for(row):
    flags = []
    if row.get("saturated"):
        flags.append("SATURATED")
    if row.get("high_loss"):
        flags.append("LOSS")
    if row.get("clock_invalid"):
        flags.append("CLOCK_INVALID")
    if row.get("void"):
        flags.append("VOID")
    if row.get("freeze_events"):
        flags.append("FREEZE")
    return "OK" if not flags else chr(124).join(flags)


def saturation_trigger_label(ramp, rate):
    values = []
    if ramp:
        values.append("ramp")
    if rate:
        values.append("rate")
    return chr(124).join(values)


def achieved_rate_from(send_timestamps, count):
    if len(send_timestamps) < 2:
        return float("nan")
    span_ns = float(send_timestamps[-1]) - float(send_timestamps[0])
    if span_ns <= 0.0:
        return float("nan")
    return float(count * 1000000000.0 / span_ns)


def metric_key(value):
    return "p" + str(value).rstrip(chr(48)).rstrip(chr(46)).replace(chr(46), chr(95))


def summarize_receiver(root, latency_csv, skip_warmup):
    run_dir = run_dir_for(latency_csv)
    data = read_run_json(run_dir / "run.json")
    parameters = data.get("parameters", {})
    recorded_warmup = warmup_from(data)
    warmup = recorded_warmup if skip_warmup is None else int(skip_warmup)
    samples = read_samples(latency_csv, warmup)
    values = samples["latencies"]
    received = int(len(values))
    expected = expected_from(data, warmup)
    dropped = None if expected is None else int(expected - received)
    drop_rate = None if expected in (None, 0) else float(dropped / expected)
    p50_raw = percentile(values, 50.0)
    void = any(value < 0.0 for value in values)
    clock_invalid = clock_invalid_for(data, p50_raw)
    valid_latency = not clock_invalid and not void
    deciles = split_deciles(values)
    first_decile_median = percentile(deciles[0], 50.0)
    last_decile_median = percentile(deciles[-1], 50.0)
    ramp_ratio = float("inf") if first_decile_median == 0.0 else float(last_decile_median / first_decile_median)
    bucket_p50s = [percentile(bucket, 50.0) for bucket in deciles]
    min_bucket_p50 = min(bucket_p50s)
    freeze_events = any(value > min_bucket_p50 * FREEZE_LIMIT for value in bucket_p50s)
    high_loss = bool(drop_rate is not None and drop_rate > RATE_EPSILON)
    duration = float(data.get("wall_clock_duration_s", 0.0))
    wall_clock_received_rate = float(received / duration) if duration > 0.0 else float("nan")
    achieved_rate = achieved_rate_from(samples["send_timestamps"], received)
    offered_rate = offered_rate_from(data)
    saturated_by_ramp = bool(ramp_ratio > RAMP_LIMIT)
    saturated_by_rate = bool(offered_rate is not None and not math.isnan(achieved_rate) and achieved_rate < offered_rate * ACHIEVED_RATE_RATIO)
    saturated = bool(saturated_by_ramp or saturated_by_rate)
    metrics = {metric_key(item): percentile(values, item) for item in PERCENTILES}
    row = {
        "row_type": "run",
        "config": "",
        "repeat": "",
        "run_dir": str(run_dir),
        "receiver": receiver_for(latency_csv),
        "offered_rate": offered_rate,
        "rate": parameters.get("rate", ""),
        "count": parameters.get("count", ""),
        "slots": parameters.get("slots", ""),
        "type": parameters.get("type", ""),
        "cpu_producer": parameters.get("cpu_producer", ""),
        "cpu_sender": parameters.get("cpu_sender", ""),
        "cpu_receiver": parameters.get("cpu_receiver", ""),
        "cpu_consumer": parameters.get("cpu_consumer", ""),
        "sndbuf": parameters.get("sndbuf", ""),
        "rcvbuf": parameters.get("rcvbuf", ""),
        "hostname": data.get("hostname", ""),
        "wall_clock_received_rate": wall_clock_received_rate,
        "achieved_rate": achieved_rate,
        "received": received,
        "expected": expected,
        "dropped": dropped,
        "drop_rate": drop_rate,
        "p50": metrics["p50"] if valid_latency else float("nan"),
        "p90": metrics["p90"] if valid_latency else float("nan"),
        "p99": metrics["p99"] if valid_latency else float("nan"),
        "p99_9": metrics["p99_9"] if valid_latency else float("nan"),
        "p99_99": metrics["p99_99"] if valid_latency else float("nan"),
        "min": min(values) if valid_latency else float("nan"),
        "mean": mean(values) if valid_latency else float("nan"),
        "max": max(values) if valid_latency else float("nan"),
        "fanout_spread_p99": float("nan"),
        "ramp_ratio": ramp_ratio,
        "ramp_slope_ns": ramp_slope(values),
        "saturated": saturated,
        "saturated_by_ramp": saturated_by_ramp,
        "saturated_by_rate": saturated_by_rate,
        "saturation_triggers": saturation_trigger_label(saturated_by_ramp, saturated_by_rate),
        "high_loss": high_loss,
        "freeze_events": freeze_events,
        "clock_invalid": clock_invalid,
        "void": void,
        "valid_latency": valid_latency,
        "clock_method": (data.get("clock_sync") or {}).get("method", ""),
        "max_drift_ns": (data.get("clock_sync") or {}).get("max_drift_ns", ""),
        "warmup": recorded_warmup,
        "skip_warmup": warmup,
        "aggregate_repeats": "",
        "p50_median": float("nan"),
        "p50_min": float("nan"),
        "p50_max": float("nan"),
        "p99_median": float("nan"),
        "p99_min": float("nan"),
        "p99_max": float("nan"),
        "max_median": float("nan"),
        "max_min": float("nan"),
        "max_max": float("nan"),
        "freeze_count": "",
        "aggregate_note": "",
        "parameters_key": normalized_parameters(data),
        "group_dir": run_dir.parent if repeat_number(run_dir) is not None else run_dir,
        "repeat_number": repeat_number(run_dir)
    }
    if void:
        print(f"VOID: negative latency in {latency_csv}", file=sys.stderr)
    return row


def apply_run_spread(rows):
    by_run = {}
    for row in rows:
        by_run.setdefault(row["run_dir"], []).append(row)
    for run_rows in by_run.values():
        p99s = [float(row["p99"]) for row in run_rows if not math.isnan(float(row["p99"]))]
        spread = 0.0 if len(p99s) <= 1 else float(max(p99s) - min(p99s))
        for row in run_rows:
            row["fanout_spread_p99"] = spread


def same_parameters(rows):
    keys = [json.dumps(row["parameters_key"], sort_keys=True, separators=(",", ":")) for row in rows]
    return len(set(keys)) == 1


def aggregate_rows(root, rows):
    output = []
    warnings = []
    groups = {}
    for row in rows:
        if row["repeat_number"] is not None:
            groups.setdefault(row["group_dir"], []).append(row)
    for group_dir, group_rows in sorted(groups.items(), key=lambda item: str(item[0])):
        repeat_numbers = sorted(set(row["repeat_number"] for row in group_rows))
        if len(repeat_numbers) < 2:
            continue
        if repeat_numbers != list(range(1, max(repeat_numbers) + 1)):
            warnings.append(f"repeat sequence is not contiguous under {group_dir}")
            continue
        if not same_parameters(group_rows):
            warnings.append(f"repeat parameters differ under {group_dir}; no aggregate row emitted")
            continue
        valid = [row for row in group_rows if row["valid_latency"]]
        if not valid:
            warnings.append(f"no valid latency rows under {group_dir}; aggregate row has blank latency fields")
        source = group_rows[0]
        aggregate = dict(source)
        aggregate["row_type"] = "aggregate"
        aggregate["config"] = config_label(root, group_dir)
        aggregate["repeat"] = "all"
        aggregate["run_dir"] = str(group_dir)
        aggregate["receiver"] = "all"
        aggregate["aggregate_repeats"] = len(repeat_numbers)
        aggregate["freeze_count"] = int(sum(1 for row in group_rows if row["freeze_events"]))
        aggregate["high_loss"] = any(row["high_loss"] for row in group_rows)
        aggregate["clock_invalid"] = any(row["clock_invalid"] for row in group_rows)
        aggregate["void"] = any(row["void"] for row in group_rows)
        aggregate["valid_latency"] = bool(valid)
        expected_values = [int(row["expected"]) for row in group_rows if row["expected"] not in (None, "")]
        received_values = [int(row["received"]) for row in group_rows if row["received"] not in (None, "")]
        dropped_values = [int(row["dropped"]) for row in group_rows if row["dropped"] not in (None, "")]
        wall_clock_values = [float(row["wall_clock_received_rate"]) for row in group_rows if row["wall_clock_received_rate"] not in (None, "") and not math.isnan(float(row["wall_clock_received_rate"]))]
        achieved_values = [float(row["achieved_rate"]) for row in group_rows if row["achieved_rate"] not in (None, "") and not math.isnan(float(row["achieved_rate"]))]
        expected_total = sum(expected_values)
        received_total = sum(received_values)
        dropped_total = sum(dropped_values)
        aggregate["received"] = received_total if received_values else ""
        aggregate["expected"] = expected_total if expected_values else ""
        aggregate["dropped"] = dropped_total if dropped_values else ""
        aggregate["drop_rate"] = float(dropped_total / expected_total) if expected_total > 0 else ""
        aggregate["wall_clock_received_rate"] = percentile(wall_clock_values, 50.0) if wall_clock_values else ""
        aggregate["achieved_rate"] = percentile(achieved_values, 50.0) if achieved_values else ""
        aggregate["saturated_by_ramp"] = any(row["saturated_by_ramp"] for row in group_rows)
        aggregate["saturated_by_rate"] = bool(source["offered_rate"] is not None and aggregate["achieved_rate"] != "" and aggregate["achieved_rate"] < float(source["offered_rate"]) * ACHIEVED_RATE_RATIO)
        aggregate["saturated"] = bool(aggregate["saturated_by_ramp"] or aggregate["saturated_by_rate"])
        aggregate["saturation_triggers"] = saturation_trigger_label(aggregate["saturated_by_ramp"], aggregate["saturated_by_rate"])
        for field in ("p50", "p90", "p99", "p99_9", "p99_99", "min", "mean", "max"):
            values = [float(row[field]) for row in valid]
            aggregate[field] = percentile(values, 50.0) if values else float("nan")
            if field in ("p50", "p99", "max"):
                aggregate[f"{field}_median"] = aggregate[field]
                aggregate[f"{field}_min"] = min(values) if values else float("nan")
                aggregate[f"{field}_max"] = max(values) if values else float("nan")

        aggregate["ramp_ratio"] = ""
        aggregate["ramp_slope_ns"] = ""
        aggregate["fanout_spread_p99"] = ""
        aggregate["aggregate_note"] = "median/min/max over repeat rows"
        output.append(aggregate)
    return output, warnings


def public_keys():
    return ["row_type", "config", "repeat", "run_dir", "receiver", "offered_rate", "rate", "count", "slots", "type", "cpu_producer", "cpu_sender", "cpu_receiver", "cpu_consumer", "sndbuf", "rcvbuf", "warmup", "skip_warmup", "hostname", "clock_method", "max_drift_ns", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "p50", "p90", "p99", "p99_9", "p99_99", "min", "mean", "max", "fanout_spread_p99", "ramp_ratio", "ramp_slope_ns", "saturated", "saturated_by_ramp", "saturated_by_rate", "saturation_triggers", "high_loss", "freeze_events", "clock_invalid", "void", "valid_latency", "aggregate_repeats", "p50_median", "p50_min", "p50_max", "p99_median", "p99_min", "p99_max", "max_median", "max_min", "max_max", "freeze_count", "aggregate_note"]


def public_row(row):
    return {key: row.get(key, "") for key in public_keys()}


def table_value(row, key):
    value = row.get(key, "")
    if value == "" or value is None:
        return ""
    if isinstance(value, float):
        if math.isnan(value):
            return ""
        if key in ("drop_rate", "ramp_ratio", "ramp_slope_ns", "wall_clock_received_rate", "achieved_rate"):
            return f"{value:.6g}"
        return f"{value:.0f}"
    return str(value)


def print_table(rows, warnings):
    display = []
    for row in rows:
        item = public_row(row)
        item["flags"] = flags_for(row)
        display.append(item)
    columns = ["row_type", "config", "repeat", "receiver", "rate", "count", "warmup", "skip_warmup", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "p50", "p99", "p99_9", "max", "ramp_ratio", "ramp_slope_ns", "saturation_triggers", "flags", "freeze_count"]
    widths = {column: len(column) for column in columns}
    lines = []
    for row in display:
        for column in columns:
            widths[column] = max(widths[column], len(table_value(row, column)))
    lines.append(" ".join(column.rjust(widths[column]) for column in columns))
    for row in display:
        lines.append(" ".join(table_value(row, column).rjust(widths[column]) for column in columns))
    if warnings:
        lines.append("")
        for warning in warnings:
            lines.append(f"WARNING: {warning}")
    print(chr(10).join(lines))


def write_csv(path, rows):
    keys = public_keys()
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(public_row(row))


def main():
    args = parse_args()
    root = args.root.resolve()
    latency_csvs = discover_latency_csvs(root)
    if not latency_csvs:
        fail(f"no rx_*/latency.csv files found below {root}")
    rows = []
    for latency_csv in latency_csvs:
        row = summarize_receiver(root, latency_csv, args.skip_warmup)
        rate = row["offered_rate"]
        if args.min_rate is not None and rate is not None and rate < args.min_rate:
            continue
        if args.max_rate is not None and rate is not None and rate > args.max_rate:
            continue
        rows.append(row)
    if not rows:
        fail("no runs remain after filters")
    for row in rows:
        row["config"] = config_label(root, row["group_dir"])
        row["repeat"] = row["repeat_number"] if row["repeat_number"] is not None else ""
    apply_run_spread(rows)
    aggregates, warnings = aggregate_rows(root, rows)
    all_rows = rows + aggregates
    public_rows = [public_row(row) for row in all_rows]
    output_path = root / ("summary.json" if args.json else "summary.csv")
    if args.json:
        with output_path.open("w", encoding="utf-8") as handle:
            json.dump(public_rows, handle, indent=2, sort_keys=True)
            handle.write(chr(10))
    else:
        write_csv(output_path, all_rows)
    print_table(all_rows, warnings)


if __name__ == "__main__":
    main()
