# Session 1 Report

## What ran
Provisioned two `m7i.2xlarge` instances in `us-east-1a`, subnet `subnet-0e13c6a8d44c1a0c0`, cluster placement group `spectral-ec2-pass-cluster` (`pg-0dc67b835fdc4fc8c`). Instances were `i-048926a1afd8d25ac` (`172.31.11.39`, public `100.63.200.242`) and `i-059f31f1084a159e6` (`172.31.13.132`, public `100.54.211.130`). Quota `L-1216C47A` was `16.0`.

Bootstrap ran on both hosts, rebooted into `isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3`, installed required packages, applied socket sysctls, and disabled irqbalance. Harness build and tests passed on both EC2 hosts.

Clock probe ran cross-host for `100000` iterations: `samples=100000 rtt_min_ns=46488 rtt_p50_ns=471237 rtt_p99_ns=716451 min_rtt_offset_ns=-24018`. I set `clock_residual_bound_ns=24018` and method `ntp_local_timesync`.

## What did not run
Trap 1 disk/tmpfs/no-write empirical comparison: NOT_RUN because the cross-host smoke orchestration never produced latency rows, and local latency measurement is forbidden.

Successful smoke test: NOT_RUN. The failed result is in `benchmark/results/session1_smoke_20260823T1818Z`: receiver exited with `received=0`, consumer failed `shm_open`, and sender hit `sendmmsg: Connection refused`. No latency number was fabricated.

## Environment
AMI `ami-052355af2a014bd2c`, Ubuntu 24.04 AWS kernel `6.17.0-1019-aws`, instance type `m7i.2xlarge`, MTU `9001`, NIC `enp39s0`, ENA driver reported by `ethtool -i` as `ena` version `6.17.0-1019-aws`. The task expected `ens5`, but this AMI exposed `enp39s0`; the `ens5` captures therefore show `No such device`.

Physical core sibling map was `0:0,4;1:1,5;2:2,6;3:3,7;4:0,4;5:1,5;6:2,6;7:3,7`; isolated cores chosen were `1,2,3`. Sysctls set: `rmem_max=67108864`, `wmem_max=67108864`, `netdev_max_backlog=250000`.

## Traps
Trap 1 audit found no file I/O in the consumer receive loop; records are buffered in memory and written after exit. Empirical comparison is NOT_RUN for the smoke failure reason above.

Trap 2 fixed with `benchmark/preflight_isolation.sh`, called by `benchmark/run_remote.sh`. Deliberate EC2 failure: `preflight failed: duplicate benchmark core 1 for deliberate_duplicate`.

Trap 3 fixed in `benchmark/sweep_rate.sh`, `benchmark/run_remote.sh`, and `benchmark/summarize.py`: p99.99-grade runs default to at least `10000000` samples and `30` seconds; exploratory runs carry `p9999_grade=false`; `sample_count` is in run metadata and summary output.

Trap 4 fixed by interleaving repeat order. Dry-run proof: `1 100000`, `2 200000`, `3 100000`, `4 200000`, `5 100000`, `6 200000` in `benchmark/results/session1_order_check/20260823T174721Z/planned_order.tsv`.

## Clock
PHC path rejected: `ethtool -T enp39s0` reported `PTP Hardware Clock: none`, and `/dev/ptp*` was absent on both hosts. Chrony selected `169.254.169.123`. Fallback is `ntp_local_timesync`, not `shared_clock`.

`clock_residual_bound_ns=24018`. That means p50 values at or below roughly `24 us` are not resolvable with this methodology; higher percentiles are credible only when materially above that bound.

## Open items for Midhilesh to verify
Verify the task’s expected `ens5` name against this AMI exposing `enp39s0`: see `reports/session1/04_clock.log`.

Verify whether `infra/bootstrap.sh` should quote remote `$(hostname)` usage in the driver commands; bootstrap env landed under `DESKTOP-RLS4QHI`, while chrony samples landed under EC2 hostnames.

Verify the remote orchestration model: fixed endpoints can exit before peer attachment, causing the failed smoke in `benchmark/results/session1_smoke_20260823T1818Z`.

Verify whether to keep the newly created cluster placement group `pg-0dc67b835fdc4fc8c`; I left it in place per task instructions.

## Before pushing
Review `benchmark/run_remote.sh`, `benchmark/sweep_rate.sh`, `benchmark/summarize.py`, and `benchmark/preflight_isolation.sh`.

Commands:

```bash
git status
git log --oneline origin/main..HEAD
git push origin main
```

## Spend and teardown
Instance runtime was approximately 0.55 hours per instance. At an assumed `m7i.2xlarge` Linux us-east-1 rate near `$0.4032/hour`, estimated compute spend is about `$0.45`, excluding small EBS pennies. Teardown verification returned empty for running/stopped instances, volumes, addresses, and spectral security groups. Placement group was retained.