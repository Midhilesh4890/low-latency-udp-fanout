# Offline dedupe and FEC pass

These are loopback numbers on WSL2. The absolute latencies are not meaningful and must not be presented as benchmark results. This pass validates correctness and relative behavior: FEC recovery accounting moves when loss is injected, dedupe tolerates reordered arrivals, and disabled features do not emit FEC or injection activity.

## What ran

- Read `README_en.md`, `harness/README.md`, `harness/include/metrics.h`, and `harness/src/consumer.cpp` before code changes.
- Built with `make -C harness`; passed.
- Tested with `make -C harness test`; passed.
- Ran the requested offline matrix under `benchmark/results/offline_de/20260821T195522Z`: rates 100000 and 300000, loss 0%, 0.1%, 1%, FEC off and FEC k=8, 3 repeats, 5 seconds each.
- Ran the reorder probe at 300000 msg/s with 1% reorder and 100 us delay for old and new dedupe.
- Ran a compatibility smoke with `--fec-k 0`, `--dedupe-disable`, and no injection flags.
- Checked `tc netem`; NOT_RUN because `tc qdisc add dev lo root netem loss 0.1%` returned exit code 2, operation not permitted.

## What did not run

- `rate_100000/loss_0/fec_off/rep_1`: NOT_RUN for latency metrics because `latency.bin` was missing or empty and sender exited 1. See `04_impairment.log`.
- `rate_300000/loss_1/fec_k8/rep_1`: NOT_RUN for latency metrics because `latency.bin` was missing or empty and consumer exited 1. See `04_impairment.log`.
- Byte-identical comparison against a pre-change binary: NOT_RUN because no pre-change binaries or captured pre-change latency files existed after this task started. The compatibility smoke did run and showed raw mode: `fec_parity_sent=0`, `test_dropped=0`, `test_reordered=0`, `fec_parity_received=0`, `fec_recovered=0`.

## Metrics audit

`metrics::Accumulator` already computes `expected` from the minimum and maximum sequence IDs it has observed, not from arrival-order gaps. The relevant lines are quoted in `01_metrics_audit.log`. No `metrics.h` change was needed, so no previously reported metric changes because of measurement-side code.

## Dedupe

The receiver now uses `dedupe::Window`: a power-of-two bitmap window with default W=65536, configured by `--dedupe-window`. It reports accepted, duplicates, too_old, lost_confirmed, window_slides, and max_reorder_depth. `--dedupe-disable` restores the old `last_seq` gate for A/B probes.

Unit tests cover in-order delivery, adjacent swap, W-1 delayed acceptance, W+1 too-old classification, exact duplicate classification, single lost gap accounting, and base advancement across a full window. Full output is in `02_dedupe.log`.

Reorder probe at 300000 msg/s, FEC off:

| mode | published | duplicates | too_old | max_reorder_depth | drop_rate |
|---|---:|---:|---:|---:|---:|
| old | 521178 | 336568 | 0 | 0 | 0.652548 |
| new | 1138734 | 143 | 37 | 65535 | 0.240844 |

The old path discarded reordered arrivals as duplicates. The new path accepted most reordered frames until the run saturated the loopback path and the window started sliding.

## FEC

The sender prefixes FEC datagrams with a 16-byte envelope and emits data immediately. Parity is emitted only after a generation closes. `--fec-k` defaults to 8 and `--fec-k 0` disables the envelope and parity path. Partial generations close on `--fec-timeout-us`, default 200.

XOR FEC was chosen over NAK because a NAK costs a full RTT before recovery and lands directly in tail percentiles. The counter-argument is real: FEC costs constant bandwidth overhead and repairs only one missing frame per generation.

`sizeof(msg::OrderBook)` was logged as 576 and worst FEC datagram size as 594 bytes, below 1500. The envelope size is statically asserted at 16 bytes.

FEC recovery medians from receiver logs:

| rate | loss | runs | recovered median | unrecoverable median | recovery p50 ns | recovery p99 ns | recovery max ns |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 0 | 3 | 26 | 55 | 0 | 38780 | 38780 |
| 100000 | 0.1 | 3 | 484 | 51 | 2724 | 250756 | 657895 |
| 100000 | 1 | 3 | 3592 | 194 | 21319 | 268225 | 13413703 |
| 300000 | 0 | 3 | 21 | 149 | 3511 | 23230 | 23230 |
| 300000 | 0.1 | 3 | 474 | 276 | 23145 | 165326 | 10869142 |
| 300000 | 1 | 3 | 10551 | 478 | 22867 | 134261 | 11436802 |

Verdict: FEC recovered frames under injected loss, but in these saturated WSL2 loopback runs it did not consistently improve percentiles or drop rate. It often made the loopback overload worse because parity adds datagrams. That is a correctness finding, not a performance result.

## Impairment pivots

Median p99, p99.9, and drop rate from valid run rows:

| rate | loss | fec | runs | p99 ns | p99.9 ns | drop_rate |
|---:|---:|---|---:|---:|---:|---:|
| 100000 | 0 | k8 | 3 | 47528275 | 70624523 | 0.119050 |
| 100000 | 0 | off | 2 | 73935166 | 110145173 | 0.010799 |
| 100000 | 0.1 | k8 | 3 | 20247090 | 27535254 | 0.075064 |
| 100000 | 0.1 | off | 3 | 15973849 | 35031227 | 0.074532 |
| 100000 | 1 | k8 | 3 | 34809004 | 36418570 | 0.241000 |
| 100000 | 1 | off | 3 | 17501080 | 28745741 | 0.189660 |
| 300000 | 0 | k8 | 3 | 308035018 | 345325052 | 0.421148 |
| 300000 | 0 | off | 3 | 232171914 | 246123436 | 0.251246 |
| 300000 | 0.1 | k8 | 3 | 512835062 | 525191080 | 0.696097 |
| 300000 | 0.1 | off | 3 | 202674996 | 271588622 | 0.229869 |
| 300000 | 1 | k8 | 2 | 651970317 | 728478478 | 0.375664 |
| 300000 | 1 | off | 3 | 238973045 | 255795400 | 0.236334 |

The full `summarize.py` output is pasted in `04_impairment.log`.

## Open items for Midhilesh to verify

- `04_impairment.log:1`: Treat all WSL2 loopback latencies as correctness-only evidence, not performance evidence.
- `04_impairment.log:13`: Decide whether to rerun the two NOT_RUN cases or leave them as missing data.
- `04_impairment.log:21`: `tc netem` could not be applied without elevated network privileges; decide whether to rerun with privileges on a Linux host.
- `04_impairment.log:167`: The compatibility smoke cannot prove byte-identical behavior against a pre-change binary because none was preserved.
- `03_fec.log:5`: Confirm the fixed `--fec-k 8` default is the desired default for later AWS tests, since it increases bandwidth even at zero loss.
- `04_impairment.log:120`: The FEC pivot shows worse loopback drop rates in several saturated cases; rerun on hardware before interpreting this as a transport result.
- `04_impairment.log:143`: The new dedupe probe still has `too_old=37` and large `lost_confirmed` because the run saturated and the window slid; verify at lower saturation or on real hardware.

## What still needs real hardware

- Confirm that FEC improves loss resilience on a real two-host network where parity overhead does not saturate loopback processing.
- Confirm recovery latency percentiles with realistic ENA/NIC behavior and real packet loss.
- Confirm dedupe reorder depth on real L2 and L3 paths, not synthetic sender-side delay.
- Confirm whether `--dedupe-window 65536` is enough for real reorder bursts at target rates.
- Confirm whether FEC k=8 and timeout 200 us are the right operating point or need tuning by rate and message type.
- Confirm one-to-many fan-out behavior with 2 or 3 receivers.
