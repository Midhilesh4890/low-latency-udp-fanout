# Session 3 corrected-path and fan-out measurements

## Result

The corrected cross-host measurement includes the submitted receiver in both network directions. At 500 kmsg/s it delivered all 3,000,000 measured messages with no ring laps, drops, duplicates, rejects, or confirmed loss. Its RTT/2 latency was 64.156 us p50, 100.034 us p99, 177.234 us p99.9, and 364.392 us p99.99. The maximum was 404.053 us one-way-equivalent, and no raw RTT sample exceeded 1 ms. The multi-millisecond episodic stall seen in session 2 is absent.

Three-receiver fan-out delivered 9,000,000 measured messages at a 250 kmsg/s source rate, plus 300,000 warm-up deliveries, with exact counters on all three receiver/consumer pairs. A three-receiver source rate of 275 kmsg/s did not complete within the runner's bounded drain window. The demonstrated fan-out capacity is therefore bracketed between 250 kmsg/s clean and 275 kmsg/s failed, equivalent to 750-825 kdatagrams/s from the single sender.

The A/B does not show a performance win over baseline 5dc3d3c. Across two bracketed runs per revision, candidate 4d4fdad was 3.31% slower at p50, 1.11% slower at p99, and 1.04% slower at p99.9. The result is materially better evidence than the session-2 loopback A/B because it traverses two hosts and two complete receiver stages without sharing benchmark roles or housekeeping on one physical core.

## Environment and allocation

Two m7i.4xlarge instances ran in one us-east-1d cluster placement group. Each exposed eight physical vCPUs with `ThreadsPerCore=1`; Linux reported eight cores, one thread per core, and SMT `notsupported`. Both used Ubuntu's 6.17.0-1019-aws kernel, the ENA driver from that kernel, and MTU 9001. The boot line reserved CPUs 1-6 with `isolcpus`, `nohz_full`, and `rcu_nocbs`, leaving CPUs 0 and 7 for housekeeping and interrupts. Full evidence is in [01_environment.log](01_environment.log).

The symmetric latency pipeline used these roles:

| Host | CPU 1 | CPU 2 | CPU 3 | CPU 4 | CPU 5-6 |
|---|---|---|---|---|---|
| TX | producer | forward sender | return receiver | consumer | unused |
| RX | forward receiver | shared-memory relay | return sender | unused | unused |

Fan-out kept the producer and sender on TX CPUs 1 and 2. RX used receiver/consumer pairs on CPUs 1/2, 3/4, and 5/6. The preflight resolves Linux CPU numbers through physical package and core IDs, rejects any repeated physical core, and also checks that every assigned CPU is isolated. The isolation regression and all harness tests passed; see [06_tests.log](06_tests.log).

## Cross-host latency method

The measured topology is:

`producer(TX) -> sender(TX) -> receiver(RX) -> relay(RX) -> sender(RX) -> receiver(TX) -> consumer(TX)`

The producer and final consumer use the TX host's clock, so no timestamp is subtracted across hosts. The reported one-way estimate divides the complete round trip by two. This fixes session 2's most important methodological defect: both network directions now execute the submitted receiver's decode and dedupe path rather than using a UDP echo adapter.

RTT/2 is still an estimator. It assumes approximately symmetric same-AZ paths and averages two receiver stages and four shared-memory hops. It cannot expose directional asymmetry or isolate a single receiver's cost. The run manifests state this explicitly as `same_clock_symmetric_pipeline_rtt_half`; direct cross-host one-way latency is not reported.

## Revision A/B

The accepted order was baseline, candidate, candidate, baseline. Every row contains 3,000,000 measured messages after 100,000 warm-up messages, with exact transport counters and no loss. Values are RTT/2.

| Revision/run | p01 | p50 | p99 | p99.9 | p99.99 | Max |
|---|---:|---:|---:|---:|---:|---:|
| 5dc3d3c opening | 28.824 us | 48.851 us | 70.943 us | 89.415 us | 326.156 us | 546.927 us |
| 4d4fdad candidate 1 | 29.440 us | 50.345 us | 70.488 us | 83.441 us | 111.180 us | 177.223 us |
| 4d4fdad candidate 2 | 31.134 us | 50.900 us | 72.085 us | 91.520 us | 128.890 us | 362.237 us |
| 5dc3d3c closing | 29.496 us | 49.149 us | 70.066 us | 83.746 us | 109.455 us | 161.170 us |
| baseline median | - | 49.000 us | 70.504 us | 86.580 us | 217.805 us | - |
| candidate median | - | 50.622 us | 71.286 us | 87.480 us | 120.035 us | - |

The candidate's p50-p99.9 difference is small but consistently unfavorable. The lower candidate p99.99 median is not claimed as an improvement because the two baseline p99.99 values differ by 3x. More interleaved repeats would be required to separate a revision effect from run-to-run tail variance. Exact counters and rejected pre-fix diagnostics are in [02_ab.log](02_ab.log).

The first candidate attempts exposed a separate implementation artifact: FEC arrival telemetry appended one sample per message even when FEC was disabled, causing allocation stalls near vector growth boundaries. Commit 4d4fdad gates that work on nonzero `fec_k` and reserves the vector when it is needed. Those pre-fix runs are rejected; the table uses only the corrected candidate and the baseline runs surrounding it.

## ENA moderation and batching

At 20 kmsg/s, disabling RX moderation improved RTT/2 p50 by 0.417 us and p99 by 4.305 us, but worsened p99.9 by 4.999 us and p99.99 by 81.793 us; the maximum increased from 152.686 to 389.837 us. The ENA default (`Adaptive RX: on`, `rx-usecs: 20`) was restored for the report-grade sweep. Detailed values are in [03_nic_coalescing.log](03_nic_coalescing.log).

A separate batch-size-1 diagnostic lost eight of one million messages on the forward leg. It is excluded from latency comparisons. Batch size 32 with a 5 us timeout was lossless throughout the accepted A/B and sweep, so it remains the documented configuration.

## Rate sweep

Each row has 3,000,000 measured observations and 100,000 warm-up messages. Raw sample files were retained locally and the durable statistics and integrity counters are in [04_rate_sweep.log](04_rate_sweep.log).

| Source rate | Loss | Min | p01 | p50 | p99 | p99.9 | p99.99 | Max |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 kmsg/s | 0 | 19.016 us | 20.581 us | 22.441 us | 35.020 us | 49.330 us | 77.620 us | 142.632 us |
| 100 kmsg/s | 0 | 18.943 us | 29.777 us | 50.097 us | 70.770 us | 85.147 us | 110.627 us | 145.917 us |
| 500 kmsg/s | 0 | 27.057 us | 47.767 us | 64.156 us | 100.034 us | 177.234 us | 364.392 us | 404.053 us |

At 500 kmsg/s, 96.113267% of samples lie between 50 and 100 us one-way-equivalent, 0.912533% lie between 100 and 200 us, and 0.090233% lie between 200 and 500 us. No sample exceeds 500 us. The ten chronological raw-RTT deciles have p50 values from 126.695 to 129.795 us and a last/first ratio of 1.005694, so there is no run-long ramp. The highest single-receiver tier tested is clean; saturation was not reached in this topology.

## Fan-out

Fan-out uses genuine receiver and consumer pairs, each on a dedicated physical core. Because producer timestamps originate on TX while these consumers run on RX and `clock_sync.method=none`, their printed latency fields are excluded. This phase is delivery and scaling evidence only.

| Source rate | Receivers | Offered packet rate | Measured deliveries | Result |
|---:|---:|---:|---:|---|
| 100 kmsg/s | 1 | 100 kdatagrams/s | 3.0M | clean, exact counters |
| 100 kmsg/s | 3 | 300 kdatagrams/s | 9.0M | clean, exact counters |
| 250 kmsg/s | 3 | 750 kdatagrams/s | 9.0M | clean, exact counters |
| 500 kmsg/s | 1 | 500 kdatagrams/s | 3.0M | clean, exact counters |
| 275 kmsg/s | 3 | 825 kdatagrams/s | NOT_RUN | sender completion timeout |
| 300 kmsg/s | 3 | 900 kdatagrams/s | NOT_RUN | sender completion timeout |
| 375 kmsg/s | 3 | 1.125 Mdatagrams/s | NOT_RUN | sender completion timeout |
| 500 kmsg/s | 3 | 1.500 Mdatagrams/s | NOT_RUN | sender completion timeout |

`NOT_RUN` here means no result was accepted: the producer/sender orchestration began, but the sender did not reach its exact source count before the bounded completion window. The confirmed clean/failed boundary is 250-275 kmsg/s at fan-out 3. Full counters and timeout windows are in [05_fanout.log](05_fanout.log).

## Anomalies and limitations

- Candidate runs before 4d4fdad showed millisecond tail cliffs caused by disabled-FEC telemetry allocation, not the network. They are excluded; [02_ab.log](02_ab.log) identifies them.
- Batch size 1 lost eight datagrams at 20 kmsg/s. It is a rejected single diagnostic, not a general loss-rate estimate; [03_nic_coalescing.log](03_nic_coalescing.log) records the counters.
- The first fan-out-3 attempt expired during sequential readiness checks before traffic. Commit e5fb156 records a 120-second startup allowance and the source revision in manifests. Capacity probes use the hardened runner.
- The fan-out consumers do not share the producer's clock. Their latency output is invalid and deliberately absent from the report.
- FEC overhead under injected loss was not remeasured in session 3. The sweep and fan-out establish the no-loss, `fec_k=0` performance envelope.

## Spend and teardown

The instances accumulated 3.438333 aggregate instance-hours at the recorded on-demand rate of $0.8064 per m7i.4xlarge hour. Estimated compute was $2.772672; public IPv4 and 8 GiB gp3 root-volume time bring the session estimate to **$2.793**, below the $15 session cap. See [07_cost.log](07_cost.log).

Both instances were terminated at 12:59:41 UTC. Independent post-termination queries returned both states as `terminated`, no recorded session volumes, no Elastic IPs for either instance, and no ENIs for the session security group. The security group lookup returned `InvalidGroup.NotFound`; the placement group lookup returned `InvalidPlacementGroup.Unknown`. See [08_teardown.log](08_teardown.log).
