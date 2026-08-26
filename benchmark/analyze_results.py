#!/usr/bin/env python3
import csv
import json
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "benchmark" / "results" / "final"
OUT = ROOT / "results"
MATRIX_RE = re.compile(r"loss(0|0p01|0p1|1)_fec(0|8)_rep([12])$")


def read_text(path):
    return path.read_text(encoding="utf-8", errors="replace")


def field_map(path):
    values = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=([-+0-9.]+)", read_text(path)):
        try:
            values[key] = float(value) if "." in value else int(value)
        except ValueError:
            continue
    return values


def metric(pattern, text):
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing metric {pattern!r}")
    return int(match.group(1))


def consumer_metrics(path):
    text = read_text(path)
    return {
        "delivered": metric(r"^received\s+:\s+([0-9]+)$", text),
        "consumer_reported_dropped": metric(r"^dropped\s+:\s+([0-9]+)$", text),
        "rtt_min_ns": metric(r"^latency \(ns\) : min=([0-9]+)", text),
        "rtt_mean_ns": metric(r"^latency \(ns\) : min=[0-9]+ mean=([0-9]+)", text),
        "rtt_max_ns": metric(r"^latency \(ns\) : .* max=([0-9]+)$", text),
        "rtt_p50_ns": metric(r"^\s+p50\s+:\s+([0-9]+)$", text),
        "rtt_p99_ns": metric(r"^\s+p99\s+:\s+([0-9]+)$", text),
        "rtt_p999_ns": metric(r"^\s+p99\.9\s+:\s+([0-9]+)$", text),
        "rtt_p9999_ns": metric(r"^\s+p99\.99\s+:\s+([0-9]+)$", text),
    }


def one_way_us(rtt_ns):
    return round(rtt_ns * 0.5 / 1000.0, 3)


def write_csv(path, rows, columns):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def matrix_rows():
    rows = []
    for run_dir in sorted((RAW / "matrix250").iterdir()):
        match = MATRIX_RE.fullmatch(run_dir.name)
        if match is None:
            continue
        loss_label, fec, repetition = match.groups()
        manifest = json.loads(read_text(run_dir / "run.json"))
        expected = int(manifest["parameters"]["count"])
        stats = consumer_metrics(run_dir / "tx" / "consumer.log")
        forward_receiver = field_map(run_dir / "rx" / "forward_receiver.log")
        return_receiver = field_map(run_dir / "tx" / "return_receiver.log")
        forward_sender = field_map(run_dir / "tx" / "forward_sender.log")
        return_sender = field_map(run_dir / "rx" / "return_sender.log")
        missing = expected - stats["delivered"]
        rows.append(
            {
                "run": run_dir.name,
                "loss_pct_per_direction": float(loss_label.replace("p", ".")),
                "fec_k": int(fec),
                "repetition": int(repetition),
                "offered_rate_msg_s": int(manifest["parameters"]["rate"]),
                "expected": expected,
                "delivered": stats["delivered"],
                "missing": missing,
                "end_to_end_missing_pct": round(100.0 * missing / expected, 6),
                "p50_us": one_way_us(stats["rtt_p50_ns"]),
                "p99_us": one_way_us(stats["rtt_p99_ns"]),
                "p999_us": one_way_us(stats["rtt_p999_ns"]),
                "p9999_us": one_way_us(stats["rtt_p9999_ns"]),
                "max_us": one_way_us(stats["rtt_max_ns"]),
                "fec_recovered_both_directions": int(forward_receiver.get("fec_recovered", 0))
                + int(return_receiver.get("fec_recovered", 0)),
                "sender_test_dropped_both_directions": int(forward_sender.get("test_dropped", 0))
                + int(return_sender.get("test_dropped", 0)),
                "impairment_method": manifest["parameters"]["impairment_method"],
            }
        )
    if len(rows) != 16:
        raise ValueError(f"expected 16 matrix runs, found {len(rows)}")
    return rows


def matrix_summary(rows):
    output = []
    for loss in (0.0, 0.01, 0.1, 1.0):
        for fec in (0, 8):
            group = [row for row in rows if row["loss_pct_per_direction"] == loss and row["fec_k"] == fec]
            if len(group) != 2:
                raise ValueError(f"expected two runs for loss={loss}, fec={fec}")
            record = {
                "loss_pct_per_direction": loss,
                "fec_k": fec,
                "runs": len(group),
            }
            for key in (
                "delivered",
                "missing",
                "end_to_end_missing_pct",
                "p50_us",
                "p99_us",
                "p999_us",
                "p9999_us",
                "max_us",
                "fec_recovered_both_directions",
            ):
                record[f"{key}_median"] = round(statistics.median(row[key] for row in group), 6)
                record[f"{key}_min"] = min(row[key] for row in group)
                record[f"{key}_max"] = max(row[key] for row in group)
            output.append(record)
    return output


def saturation_rows():
    specs = [
        (250000, "matrix250/loss0_fec0_rep1", "accepted"),
        (250000, "matrix250/loss0_fec0_rep2", "accepted"),
        (500000, "matrix/loss0_fec0_rep1", "accepted"),
        (600000, "saturation/fec0_600k_rep1", "accepted"),
        (700000, "saturation/fec0_700k_rep1", "accepted"),
        (700000, "saturation/fec0_700k_clean_rep1", "accepted"),
        (725000, "saturation/fec0_725k_clean_rep1", "accepted"),
        (750000, "saturation/fec0_750k_clean_rep1", "rejected"),
    ]
    rows = []
    for rate, relative, status in specs:
        run_dir = RAW / relative
        stats = consumer_metrics(run_dir / "tx" / "consumer.log")
        expected = 3_000_000
        forward = field_map(run_dir / "tx" / "forward_sender.log")
        returned = field_map(run_dir / "rx" / "return_sender.log")
        rows.append(
            {
                "run": relative,
                "offered_rate_msg_s": rate,
                "status": status,
                "delivered": stats["delivered"],
                "missing": expected - stats["delivered"],
                "forward_sender_lapped": int(forward.get("lapped", 0)),
                "return_sender_lapped": int(returned.get("lapped", 0)),
                "p50_us": one_way_us(stats["rtt_p50_ns"]),
                "p99_us": one_way_us(stats["rtt_p99_ns"]),
                "p999_us": one_way_us(stats["rtt_p999_ns"]),
                "p9999_us": one_way_us(stats["rtt_p9999_ns"]),
            }
        )
    return rows


def fanout_rows():
    return [
        {
            "run": "fanout/control1_275k",
            "receivers": 1,
            "source_rate_msg_s": 275000,
            "offered_datagrams_s": 275000,
            "status": "accepted",
            "measured_deliveries": 3000000,
            "reason": "exact delivery; sender lapped=0",
        },
        {
            "run": "fanout/fanout2_275k",
            "receivers": 2,
            "source_rate_msg_s": 275000,
            "offered_datagrams_s": 550000,
            "status": "accepted",
            "measured_deliveries": 6000000,
            "reason": "exact delivery to both receivers; sender lapped=0",
        },
        {
            "run": "fanout/three_receivers_250k",
            "receivers": 3,
            "source_rate_msg_s": 250000,
            "offered_datagrams_s": 750000,
            "status": "accepted",
            "measured_deliveries": 9000000,
            "reason": "exact delivery to all three receivers; sender lapped=0",
        },
        {
            "run": "fanout/fanout3_275k",
            "receivers": 3,
            "source_rate_msg_s": 275000,
            "offered_datagrams_s": 825000,
            "status": "rejected",
            "measured_deliveries": "",
            "reason": "sender did not exit within 72 s; no counters accepted",
        },
        {
            "run": "fanout/control1_825k",
            "receivers": 1,
            "source_rate_msg_s": 825000,
            "offered_datagrams_s": 825000,
            "status": "rejected",
            "measured_deliveries": "",
            "reason": "sender did not exit within 64 s; no counters accepted",
        },
    ]


def main():
    rows = matrix_rows()
    summary = matrix_summary(rows)
    write_csv(OUT / "loss_matrix_runs.csv", rows, list(rows[0]))
    write_csv(OUT / "loss_matrix_summary.csv", summary, list(summary[0]))
    saturation = saturation_rows()
    write_csv(OUT / "saturation_summary.csv", saturation, list(saturation[0]))
    fanout = fanout_rows()
    write_csv(OUT / "fanout_summary.csv", fanout, list(fanout[0]))
    print(f"wrote {len(rows)} matrix runs, {len(summary)} matrix cells, {len(saturation)} saturation rows, {len(fanout)} fan-out rows")


if __name__ == "__main__":
    main()
