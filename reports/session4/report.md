# Final EC2 measurement report

## Outcome

The final session closed the three remaining evidence gaps: loss/FEC behavior, saturation, and fan-out attribution. No transport optimization was added during this session.

The strongest result is conditional rather than promotional:

- At 250 kmsg/s and 0.01% sender-side loss per direction, FEC k=8 delivered all 3,000,000 measured messages and improved the median p99, p99.9, and p99.99, while increasing p50.
- At 0.1% and 1% loss, FEC reduced missing messages by about 158x and 13x respectively, but its median latency percentiles were worse than FEC-off. It therefore improves delivery completeness without improving the scored percentiles at those loss levels.
- The FEC-off symmetric pipeline delivered exactly through 725 kmsg/s. At 750 kmsg/s the forward sender lapped its input ring 55,349 times, so the exact-delivery ceiling is bracketed at 725-750 kmsg/s. Tail latency has already collapsed at 725 kmsg/s.
- Fan-out is exact for one and two receivers at a 275 kmsg/s source rate and for three receivers at 250 kmsg/s. Three receivers at 275 kmsg/s fail sender completion. A one-receiver control at the equivalent aggregate rate of 825 kdatagrams/s also fails, so the evidence does not isolate the sequential destination loop as the sole cause.
- Neither host exposed a PHC. Chrony tuning was tested, including high-rate Amazon Time Sync and direct inter-host interleaved mode, but independent clock probes did not validate sub-microsecond cross-host agreement. The organizer-approved full-pipeline RTT/2 method remains the reportable latency method.

Compact data is in [loss_matrix_runs.csv](loss_matrix_runs.csv), [loss_matrix_summary.csv](loss_matrix_summary.csv), [saturation_summary.csv](saturation_summary.csv), and [fanout_summary.csv](fanout_summary.csv). The repository-level [analysis notebook](../../analysis.ipynb) reads only these committed files.

## Transport under test

The source revision is 9a2ca8c and the loss-aware runner revision is e616a3e.

The transport uses UDP datagrams between hosts and shared-memory rings at the fixed producer/consumer boundaries. The sender batches with sendmmsg, default batch size 32 and a 5 us timeout in the report-grade runs. The receiver validates framing, publishes accepted messages to shared memory, and uses a 65,536-message dedupe window.

Optional XOR FEC groups eight data messages and emits one parity datagram. Data is transmitted immediately; parity can recover one missing data datagram per generation. The cost is approximately 22% additional bytes in the observed k=8 runs, including the FEC envelope. FEC-off remains the low-latency/default measurement mode in the runners.

The fan-out sender iterates over destinations. This is a known scaling limitation, but it was documented rather than changed in the final session.

## Environment

Two m7i.4xlarge instances ran in us-east-1d in a cluster placement group linked under a precision-time parent placement group. Each instance exposed eight physical vCPUs with one thread per core; SMT was not supported. Both hosts used Ubuntu kernel 6.17.0-1019-aws, ENA, and MTU 9001.

The boot line reserved CPUs 1-6 with isolcpus, nohz_full, and rcu_nocbs. CPUs 0 and 7 remained housekeeping cores. Every accepted run passed the physical-core/isolation preflight.

The symmetric latency pipeline used:

| Host | CPU 1 | CPU 2 | CPU 3 | CPU 4 |
|---|---|---|---|---|
| TX | producer | forward sender | return receiver | consumer |
| RX | forward receiver | relay | return sender | unused |

Fan-out used producer/sender on TX CPUs 1/2 and receiver/consumer pairs on RX CPUs 1/2, 3/4, and 5/6.

## Clock gate and latency topology

No /dev/ptp device existed on either host. ethtool reported software transmit/receive/system timestamping and no PTP hardware clock.

Two chrony configurations were tested because the organizers noted that chrony can sometimes achieve sub-microsecond synchronization:

| Configuration | Chrony indication | Independent TX-to-RX estimate | Independent RX-to-TX estimate | Verdict |
|---|---:|---:|---:|---|
| Amazon Time Sync, minpoll/maxpoll 0 | corrections about 0.7-3.5 us | -1.474 us | +4.824 us | reject one-way subtraction |
| Direct RX server, TX xleave plus F323 | correction about 0.370 us; source stddev about 2.1 us | -11.500 us | +9.207 us | reject one-way subtraction |

The independent directional estimates should agree after sign normalization within the desired error budget. They did not. Chrony status alone was therefore not used as proof of sub-microsecond accuracy. Both hosts were restored to Amazon Time Sync with minpoll/maxpoll 4.

The accepted topology is:

producer(TX) -> sender(TX) -> receiver(RX) -> relay(RX) -> sender(RX) -> receiver(TX) -> consumer(TX)

Producer and consumer timestamps come from the TX clock. Reported latency is full-pipeline RTT divided by two. This includes the submitted receiver in both directions. It assumes approximate path symmetry and is not a directional one-way measurement, but it avoids subtracting unsynchronized host clocks and was explicitly accepted by the organizers.

## Loss methodology

The intended impairment was tc netem on the real ENA path. Four implementations were rejected before the final matrix:

- A filtered root prio/netem qdisc at 0% loss missed messages at 500 kmsg/s.
- Netem attached to mq leaves at 0% caused sender/ring pressure and large message loss.
- Ingress IFB netem at 0% similarly changed throughput.
- A port-specific iptables statistic rule caused sendmmsg to return EPERM when it matched.

The hosts were restored to the original mq root with eight fq_codel leaves, and iptables OUTPUT was restored to ACCEPT with no added rule. ENA allowance-exceeded counters remained zero.

The accepted fallback uses the sender's deterministic test-drop option in both directions. This still exercises the real two-host ENA path, receiver dedupe, FEC decoder, relay, and return path, but the loss occurs before the kernel transmit path. It must not be described as on-wire or tc-netem loss. Each manifest records impairment_method=sender_test_drop.

The 500 kmsg/s FEC control failed its zero-lap gate, and a 400 kmsg/s calibration also lapped. The final matrix therefore used the recommendation's alternative trim: two repetitions at the validated 250 kmsg/s rate. Every cell contains 3,000,000 measured messages after 100,000 warm-up messages. FEC order was reversed in repetition two.

The deterministic seed was held fixed. That creates a strong paired FEC-off/on comparison, but the two repetitions do not provide independent loss patterns; they measure run-to-run system noise around the same injected pattern.

## Loss matrix results

Values are medians of two runs and are RTT/2 one-way-equivalent microseconds. Missing percentage is computed from expected minus delivered, not only the consumer's internal gap counter.

| Loss per direction | FEC k | Delivered | Missing | p50 | p99 | p99.9 | p99.99 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0% | 0 | 3,000,000 | 0% | 53.593 | 85.642 | 140.066 | 164.256 |
| 0% | 8 | 3,000,000 | 0% | 61.777 | 120.265 | 170.469 | 193.999 |
| 0.01% | 0 | 2,999,366 | 0.021133% | 54.982 | 109.007 | 172.093 | 199.736 |
| 0.01% | 8 | 3,000,000 | 0% | 57.008 | 102.758 | 161.659 | 184.691 |
| 0.1% | 0 | 2,993,759 | 0.208033% | 54.052 | 99.027 | 169.722 | 201.674 |
| 0.1% | 8 | 2,999,960.5 | 0.001316% | 57.109 | 139.455 | 186.324 | 226.716 |
| 1% | 0 | 2,937,685 | 2.077167% | 53.331 | 101.170 | 169.138 | 193.289 |
| 1% | 8 | 2,995,096 | 0.163467% | 59.773 | 108.537 | 171.988 | 206.829 |

At zero loss, FEC is slower at every reported percentile.

At 0.01%, FEC raises median p50 by 3.7% but improves p99 by 5.7%, p99.9 by 6.1%, and p99.99 by 7.5%; it also eliminates all measured end-to-end missing messages. This is the only tested loss level where it improves the scored tail medians.

At 0.1%, FEC reduces median missing messages from 6,241 to 39.5 but worsens p50, p99, p99.9, and p99.99. At 1%, it reduces median missing messages from 62,315 to 4,904 and again worsens all four median percentiles.

Undelivered messages produce no latency sample, while recovered messages do enter the distribution with recovery delay. The table therefore reports percentiles and delivered counts together but does not treat delivery loss as a second optimization objective. The direct scoring conclusion is that k=8 is not uniformly beneficial: it wins the measured tail at 0.01%, not at 0.1% or 1%.

## Saturation

Only zero-loss, FEC-off runs enter the main saturation bracket.

| Source rate | Runs/status | Delivered | p50 | p99 | p99.9 | p99.99 |
|---:|---|---:|---:|---:|---:|---:|
| 250 kmsg/s | 2 accepted | 3.0M each | 52.634-54.551 | 75.004-96.279 | 112.179-167.953 | 140.597-187.914 |
| 500 kmsg/s | 1 accepted | 3.0M | 74.728 | 104.511 | 121.588 | 146.392 |
| 600 kmsg/s | 1 accepted | 3.0M | 76.669 | 173.860 | 212.006 | 263.792 |
| 700 kmsg/s | 2 accepted | 3.0M each | 84.204-136.827 | 194.691-295.659 | 265.356-314.524 | 305.694-326.017 |
| 725 kmsg/s | 1 accepted | 3.0M | 168.219 | 1,109.958 | 1,168.397 | 1,242.297 |
| 750 kmsg/s | rejected | 2,895,779 | invalid | invalid | invalid | invalid |

At 725 kmsg/s every transport counter is exact and sender/relay laps are zero, but the tail is already more than 1 ms. At 750 kmsg/s the forward sender sends 2,996,109 of 3,100,000 total messages and reports lapped=55,349. The receiver then publishes 2,995,779 and the measured consumer misses 104,221. The mechanism is producer-to-forward-sender shared-memory overrun, not an ENA allowance drop.

The exact-delivery ceiling is therefore 725-750 kmsg/s. For low-tail operation, 725 kmsg/s is already beyond the practical knee.

FEC has a lower throughput envelope. At 500 kmsg/s with k=8, the forward sender lapped 223,540 and the measured consumer received 2,744,600. At 400 kmsg/s the short calibration also lapped. At 250 kmsg/s it was exact. These rejected controls explain why the loss matrix was moved to 250 kmsg/s.

## Fan-out

Fan-out consumers run on RX and do not share the producer clock, so fan-out latency is intentionally excluded. This phase measures delivery and scaling only.

| Receivers | Source rate | Aggregate datagrams | Result |
|---:|---:|---:|---|
| 1 | 275 kmsg/s | 275 kdatagrams/s | exact 3.0M measured |
| 2 | 275 kmsg/s | 550 kdatagrams/s | exact 3.0M per receiver |
| 3 | 250 kmsg/s | 750 kdatagrams/s | exact 3.0M per receiver (session 3) |
| 3 | 275 kmsg/s | 825 kdatagrams/s | rejected: sender did not exit within 72 s |
| 1 | 825 kmsg/s | 825 kdatagrams/s | rejected: sender did not exit within 64 s |

The three-receiver boundary is reproduced at 250 kmsg/s clean and 275 kmsg/s failed. The 1/2/3 matched-source controls show that adding the third destination crosses the capacity boundary at 275 kmsg/s. However, the one-destination 825 kdatagrams/s control also fails, so the data supports a sender aggregate-capacity limitation and does not prove that the sequential destination loop alone is responsible.

## Rejected and contaminated runs

Pre-registered gates rejected any run with sender/relay ring laps, receiver rejects, incomplete counters, or missing manifests.

A runner hygiene issue was discovered during the saturation sweep: remote RTT result directories accumulated under /tmp, eventually filling the TX root volume while a latency file was being written. Apparent late failures at 650-725 kmsg/s from that period are excluded. The benchmark-owned remote copies were deleted after their local results were preserved, freeing 4.3 GB. Clean-disk reruns produced the accepted 700 and 725 kmsg/s points and the rejected 750 kmsg/s boundary. The runner cleanup is fixed in the final branch so future runs remove their remote temp directory.

No result from the rejected netem, iptables, disk-full, FEC-overload, or incomplete fan-out attempts enters an accepted latency aggregate.

## Reproduction

Build and test:

    make -C harness clean all test
    bash benchmark/test_preflight_isolation.sh

Provision and bootstrap are documented in [infra/README_RUN.md](../../infra/README_RUN.md). Use two no-SMT m7i.4xlarge instances in one AZ, MTU 9001, CPUs 1-6 isolated, and ENA defaults.

A matrix cell is run with benchmark/run_pipeline_rtt_remote.sh using:

    --rate 250000
    --count 3000000
    --warmup 100000
    --batch-size 32
    --batch-timeout-us 5
    --fec-k 0 or 8
    --test-drop-pct 0, 0.01, 0.1, or 1
    --allow-loss for nonzero loss
    --latency-output disk

Use the full TX/RX public and private host arguments shown by the runner usage. Alternate FEC order between repetitions. Every accepted nonzero-loss manifest must say sender_test_drop.

Regenerate compact evidence and the notebook:

    python3 -m pip install -r requirements-analysis.txt
    python3 benchmark/analyze_session4.py
    python3 benchmark/build_analysis_notebook.py
    jupyter notebook analysis.ipynb

The raw files are deliberately not committed because they occupy 4.8 GB. The per-run CSV, aggregate CSV, notebook, manifests in local raw results, and this report preserve the definitions and conclusions needed for review.

## Limitations

- RTT/2 assumes approximate symmetry and averages two receiver stages plus four shared-memory hops.
- Loss is deterministic at the sender, not an on-wire tc-netem process.
- Two repetitions are enough for a compact paired check but not a strong tail confidence interval.
- Repeated loss runs use the same deterministic drop pattern.
- FEC k=8 and 200 us timeout are evaluated, not exhaustively tuned.
- Fan-out uses multiple receiver/consumer pairs on one RX host, not three physical RX hosts.
- The 725 kmsg/s point is an exact-delivery ceiling observation, not a recommended low-latency operating rate.

## Teardown

AWS confirmed both Session 4 instances terminated. Their two root volume IDs no
longer resolve, and filter-based checks returned empty lists for session ENIs,
the child cluster placement group, the precision-time parent placement group,
and the session security group. No session EC2 resource remains billable after
teardown.

## Final recommendation

Keep FEC disabled by default for latency-first operation. Enable k=8 only when the expected loss regime and score make the 0.01% tail improvement relevant, and do not claim a percentile win at 0.1% or 1% from these data. Treat 725 kmsg/s as the exact-delivery ceiling and operate below the sharp tail knee. Keep the documented 250 kmsg/s three-receiver fan-out limit rather than changing transport architecture this late.
