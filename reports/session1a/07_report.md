# Session 1a Report

Date: 2026-08-23 UTC / 2026-08-24 Asia/Calcutta
Repo base commit at start: 732991995586a6186dc9c4676fcda2326da9d78c

## What Changed

- `infra/bootstrap.sh`
  - Detects the actual primary NIC instead of assuming `ens5`; observed NIC is `enp39s0`.
  - Captures env under the remote default `env/$(hostname)`; no local `DESKTOP...` hostnames were produced in Session 1a.
  - Attempts live SMT disable; on tested m7i hosts SMT remained `on`, so bootstrap fell back to isolating full sibling sets.
  - Captures SMT state, IRQ affinity, NIC details, `/dev/ptp*`, chrony, cmdline, CPU topology, and network sysctls.
  - Moves IRQ affinity to housekeeping CPU 0 where permitted.

- `benchmark/preflight_isolation.sh`
  - Accepts `--isolated-cores`.
  - Fails duplicate logical cores and duplicate physical-core sharing.
  - When SMT is active, verifies every benchmark core's sibling is in the isolated set.
  - Checks runnable/stray benchmark processes by physical core, not just logical CPU id.

- `benchmark/run_remote.sh`
  - Uses strict startup gates: receiver pid/alive, UDP bind, output shm, consumer pid/alive, producer pid, sender pid, and ordered exits.
  - Replaced fragile sleeps with polling confirmations.
  - Uses a single remote TX startup transaction so sender starts immediately after the producer shm exists on the TX host.
  - Supports `--latency-output disk|tmpfs|none` for Trap1.
  - Extends receiver/consumer idle windows to 30s so confirmation round trips do not race 2s idle shutdown.

- `tools/clock_probe.cpp`
  - Adds `rtt_p999_ns` to the 100k clock probe output.

## PHC / Clock Findings

Three PHC placement cases were tested on fresh EC2 `m7i.2xlarge` hosts in `us-east-1a`:

| Case | Hostname | NIC | /dev/ptp count | Hardware timestamp count | SMT | Isolated choice |
|---|---|---:|---:|---:|---|---|
| cluster placement group | ip-172-31-12-235 | enp39s0 | 0 | 0 | on | 1,5,2,6,3,7 |
| precision-time placement group | ip-172-31-5-254 | enp39s0 | 0 | 0 | on | 1,5,2,6,3,7 |
| no placement group | ip-172-31-1-161 | enp39s0 | 0 | 0 | on | 1,5,2,6,3,7 |

Conclusion: PHC was not exposed on these `m7i.2xlarge` instances, even in the precision-time placement group. I did not configure PHC/ptp4l because there was no PHC device to use.

Cross-host clock probe examples:

- Final cross-host run before shared fallback: `samples=100000 rtt_min_ns=79028 rtt_p50_ns=583812 rtt_p99_ns=839282 rtt_p999_ns=941979 min_rtt_offset_ns=-52684`.
- Path MTU sweep found `path_mtu_payload=8973`, `path_mtu=9001`.
- Cross-host smoke produced all latency rows but was correctly flagged `CLOCK_INVALID` because `max_drift_ns=52684` exceeded 10% of raw p50 (`160826.5 ns`). Chrony tail bounds were also not tight enough, so I did not relabel it valid.

Because cross-host one-way latency could not honestly satisfy the existing clock-validity rule on these hosts, I completed the empirical smoke and Trap1 work with a same-EC2-host shared-clock fallback. That run is explicitly labeled `shared_clock` / `cluster_shared_host`.

## Final Shared-Clock Smoke

Run directory: `benchmark/results/session1a_20260823T205242Z_shared_smoke_disk`

| Mode | Received / Expected | Drops | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns | Flags |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| smoke disk | 300000 / 300000 | 0 | 3219 | 6792 | 587614 | 1067087 | 1125325 | OK |

## Trap1 Three-Way Comparison

Run prefix: `benchmark/results/session1a_20260823T205242Z_shared_trap1_*`

| Mode | Latency output | Received / Expected | Drops | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns | Flags |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| disk | consumer binary latency on disk, converted after run | 300000 / 300000 | 0 | 3244 | 6183 | 537744 | 1034168 | 1097564 | OK |
| tmpfs | consumer binary latency in `/dev/shm`, copied/converted after run | 300000 / 300000 | 0 | 3202 | 6296 | 558687 | 1041151 | 1104492 | OK |
| none | no latency file write; metrics from consumer log | 320000 / 320000 including warmup | 0 | 3190 | 6471 | 528495 | 1044602 | 1108475 | log metrics |

Observed result: disk and tmpfs are very close at p50/p99. The no-write run is also close by consumer-log metrics. The large p99.9/p99.99 spikes remain present even with no latency file writing, so Trap1 is not explained by disk CSV/bin output alone in this shared-clock fallback.

## Blockers Fixed / Characterized

1. SMT disable/isolate siblings: live SMT disable did not take on these EC2 hosts; fallback sibling isolation works and preflight verifies it.
2. PHC: no PHC was present in cluster, precision-time, or no-PG cases.
3. Clock probe contamination: probe now reports p99.9; runs are pinned and preflighted. Cross-host clock validity remains insufficient for this very low one-way latency path.
4. Orchestration: fixed receiver idle race, UDP bind matcher, clock probe daemonization, and producer shm race.
5. Trap1: disk/tmpfs/no-write comparison completed at 300k on EC2 using shared-clock fallback.
6. Hostname expansion: env dirs are EC2 `ip-...` hostnames; no `DESKTOP...` dir was created.

## Teardown

`reports/session1a/06_teardown.log` shows:

- Session 1a active instances: `[]`
- Project active instances: `[]`
- Session 1a active/available volumes: `[]`
- Session 1a ENIs: `[]`
- `spectral-ec2-pass-sg` deleted; post-delete query: `[]`
- Placement groups left available:
  - `spectral-ec2-pass-cluster` (`cluster`, `pg-0dc67b835fdc4fc8c`)
  - `spectral-ec2-pass-precision-time-session1a` (`precision-time`, `pg-0023021523452f198`)

Estimated EC2 spend stayed below the $10 cap. The rough on-demand compute cost is under $3 using `m7i.2xlarge` in us-east-1 and the observed short-lived runs; exact billing should be checked in AWS Cost Explorer.

## Verification

- EC2 build/test passed on benchmark hosts: `make -C harness clean && make -C harness && make -C harness test`.
- Local syntax checks passed: `bash -n infra/bootstrap.sh benchmark/preflight_isolation.sh benchmark/run_remote.sh`.
- Local whitespace check passed: `git diff --check`.
- Shell comment scan on modified shell scripts returned no matches.