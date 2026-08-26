#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cell(cell_type, source):
    result = {
        "cell_type": cell_type,
        "metadata": {},
        "source": source.splitlines(keepends=True),
    }
    if cell_type == "code":
        result["execution_count"] = None
        result["outputs"] = []
    return result


cells = [
    cell(
        "markdown",
        """# EC2 transport analysis: loss, saturation, and fan-out

This notebook reads the committed CSV tables under `results/`.

Latency uses the organizer-approved symmetric topology producer -> sender -> receiver -> relay -> sender -> receiver -> consumer. Both timestamps are on TX, and values below are full-pipeline RTT divided by two. They are one-way-equivalent estimates, not directional one-way measurements.

The loss matrix uses 250k messages/s, 3,000,000 measured messages after 100,000 warm-up messages, two repetitions, and deterministic sender-side packet loss in both network directions. tc netem attempts were rejected because even a 0% qdisc changed throughput at 500k. Fan-out latency is intentionally absent because its producer and consumers use different clocks.
""",
    ),
    cell(
        "code",
        """from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt

root = Path.cwd()
if not (root / "results").exists():
    root = root.parent
report_dir = root / "results"
runs = pd.read_csv(report_dir / "loss_matrix_runs.csv")
loss = pd.read_csv(report_dir / "loss_matrix_summary.csv")
saturation = pd.read_csv(report_dir / "saturation_summary.csv")
fanout = pd.read_csv(report_dir / "fanout_summary.csv")
""",
    ),
    cell(
        "markdown",
        """## Loss matrix

Medians summarize the two repetitions; min/max columns in the CSV preserve their spread. Delivered sample counts are shown beside percentiles so missing messages cannot make a tail look better without disclosure.
""",
    ),
    cell(
        "code",
        """loss[[
    "loss_pct_per_direction", "fec_k", "delivered_median",
    "end_to_end_missing_pct_median", "p50_us_median", "p99_us_median",
    "p999_us_median", "p9999_us_median"
]].style.format({
    "end_to_end_missing_pct_median": "{:.6f}",
    "p50_us_median": "{:.3f}", "p99_us_median": "{:.3f}",
    "p999_us_median": "{:.3f}", "p9999_us_median": "{:.3f}",
})
""",
    ),
    cell(
        "code",
        """fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True)
for ax, metric, title in zip(
    axes.flat,
    ["p50_us_median", "p99_us_median", "p999_us_median", "p9999_us_median"],
    ["p50", "p99", "p99.9", "p99.99"],
):
    for fec_k, group in loss.groupby("fec_k"):
        ax.plot(group["loss_pct_per_direction"], group[metric], marker="o", label=f"FEC k={fec_k}")
    ax.set_xscale("symlog", linthresh=0.01)
    ax.set_title(title)
    ax.set_ylabel("RTT/2 latency (us)")
    ax.grid(True, alpha=0.3)
axes[1, 0].set_xlabel("Injected loss per direction (%)")
axes[1, 1].set_xlabel("Injected loss per direction (%)")
axes[0, 0].legend()
fig.suptitle("Latency percentiles under sender-side synthetic loss")
fig.tight_layout()
plt.show()
""",
    ),
    cell(
        "code",
        """fig, ax = plt.subplots(figsize=(8, 4.5))
for fec_k, group in loss.groupby("fec_k"):
    plotted = group["end_to_end_missing_pct_median"].clip(lower=1e-6)
    ax.plot(group["loss_pct_per_direction"], plotted, marker="o", label=f"FEC k={fec_k}")
ax.set_xscale("symlog", linthresh=0.01)
ax.set_yscale("log")
ax.set_xlabel("Injected loss per direction (%)")
ax.set_ylabel("End-to-end missing messages (%)")
ax.set_title("Delivery completeness (zero plotted at 1e-6%)")
ax.grid(True, which="both", alpha=0.3)
ax.legend()
plt.show()
""",
    ),
    cell(
        "markdown",
        """At 0.01% loss, FEC is the only tested loss level where its median percentiles are not uniformly worse, and it recovers every measured message. The apparent 5.7% p99 improvement is within run-to-run variation observed elsewhere. With two repetitions and a fixed drop seed, this is not evidence of a latency win. At 0.1% and 1%, FEC sharply reduces missing messages but worsens every reported median percentile. Zero-loss FEC is also slower at every percentile.

Loss is injected in the sender before `sendmmsg`, so a dropped datagram consumes no kernel or NIC transmit work. Real in-flight loss would retain that work and make FEC relatively more expensive. FEC also has a much lower demonstrated throughput envelope: k=8 is exact at 250k messages/s but laps at 400k and 500k, while FEC-off is exact at 725k messages/s.
""",
    ),
    cell(
        "markdown",
        """## Saturation

Accepted means exact delivery with zero sender/relay laps and zero receiver rejects. The first rejected clean-disk point is retained to show the failure mechanism.
""",
    ),
    cell(
        "code",
        """accepted = saturation[saturation["status"] == "accepted"]
fig, ax = plt.subplots(figsize=(8, 4.5))
for metric, label in [("p50_us", "p50"), ("p99_us", "p99"), ("p9999_us", "p99.99")]:
    ax.scatter(accepted["offered_rate_msg_s"] / 1000, accepted[metric], label=label)
ax.axvline(725, color="tab:green", linestyle="--", label="highest exact: 725k")
ax.axvline(750, color="tab:red", linestyle="--", label="first failed: 750k")
ax.set_yscale("log")
ax.set_xlabel("Offered source rate (kmsg/s)")
ax.set_ylabel("RTT/2 latency (us, log scale)")
ax.set_title("Single-destination saturation")
ax.grid(True, which="both", alpha=0.3)
ax.legend()
plt.show()
""",
    ),
    cell(
        "markdown",
        """The exact-delivery boundary is 725k clean / 750k failed. Tail latency has already collapsed at 725k, so the practical latency knee is below the exact-delivery ceiling. At 750k the forward sender laps its input ring.

## Fan-out

Fan-out is delivery-only evidence. One and two destinations are exact at a 275k source rate; three destinations are exact at 250k and fail sender completion at 275k. A one-destination 825k aggregate-rate control also fails, so the data does not isolate the sequential destination loop as the sole cause.
""",
    ),
    cell("code", "fanout\n"),
    cell(
        "markdown",
        """## Clock and impairment caveats

The Ubuntu measurement hosts exposed no PHC. Aggressive Amazon Time Sync polling and direct LAN chrony interleaved mode were both tested. Chrony status could display sub-microsecond correction, but independent bidirectional probes disagreed by several microseconds, so cross-host one-way subtraction was rejected. A later Amazon Linux 2023 probe exposed the ENA PHC after enabling `ena.phc_enable=1`, but hardware-error bounds of 22.735 us and 28.038 us were still too large for the target latency. The approved RTT/2 method remained the reportable clock method.

Filtered root qdisc, mq-leaf netem, and ingress IFB netem all changed the zero-loss 500k throughput; a port-specific iptables loss rule caused `sendmmsg` to return EPERM. Those runs are rejected. The accepted matrix therefore uses deterministic loss before `sendmmsg` and is labeled that way in every manifest and table. Because dropped datagrams consume no kernel or NIC transmit work, this method biases the comparison in FEC's favor relative to real in-flight loss.
""",
    ),
]

notebook = {
    "cells": cells,
    "metadata": {
        "kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
        "language_info": {"name": "python", "version": "3"},
    },
    "nbformat": 4,
    "nbformat_minor": 5,
}
(ROOT / "analysis.ipynb").write_text(json.dumps(notebook, indent=1) + "\n", encoding="utf-8")
print(f"wrote {ROOT / 'analysis.ipynb'} with {len(cells)} cells")
