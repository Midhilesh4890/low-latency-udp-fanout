# Detailed EC2 measurement report

## Results at a glance

I measured one-way throughput, loss with and without FEC, symmetric RTT/2 saturation, fan-out scaling, and clock accuracy.

The main findings are:

- Plain one-way delivery with the FEC-off fast path and a 1,048,576-slot ring was exact at 1.25M messages/s in two 3,000,000-message repetitions. It failed at 1.375M in two repetitions. Sender processing remained about 0.80-0.84M messages/s, so 1.25M is finite-run burst capacity and does not establish sustainable 1.25M or 2M throughput.
- At 250 kmsg/s and 0.01% sender-side loss per direction, FEC k=8 delivered all 3,000,000 measured messages. This was the only tested loss level where FEC's median percentiles were not uniformly worse, but two repetitions with one fixed loss pattern do not establish a latency improvement.
- At 0.1% and 1% loss, FEC reduced missing messages by about 158x and 13x respectively, but its median latency percentiles were worse than FEC-off. It therefore improves delivery completeness without improving the scored percentiles at those loss levels.
- The FEC-off symmetric pipeline delivered exactly through 725 kmsg/s. At 750 kmsg/s the forward sender lapped its input ring 55,349 times, so the exact-delivery ceiling is bracketed at 725-750 kmsg/s. Tail latency has already collapsed at 725 kmsg/s.
- Fan-out is exact for one and two receivers at a 275 kmsg/s source rate and for three receivers at 250 kmsg/s. Three receivers at 275 kmsg/s fail sender completion. A one-receiver control at the equivalent aggregate rate of 825 kdatagrams/s also fails, so the evidence does not isolate the sequential destination loop as the sole cause.
- The Ubuntu measurement hosts exposed no PHC. Chrony tuning did not validate sub-microsecond cross-host agreement, and a later ENA PHC probe had hardware-error bounds above 22 us. Latency therefore uses the full-pipeline same-clock RTT/2 method.

The compact data is in [oneway_throughput_runs.csv](oneway_throughput_runs.csv), [oneway_throughput_summary.csv](oneway_throughput_summary.csv), [loss_matrix_runs.csv](loss_matrix_runs.csv), [loss_matrix_summary.csv](loss_matrix_summary.csv), [saturation_summary.csv](saturation_summary.csv), and [fanout_summary.csv](fanout_summary.csv). The [analysis notebook](../analysis.ipynb) reads those files directly.

## Transport under test

UDP carries messages between the two hosts, while shared-memory rings connect the fixed producer and consumer to the transport. In the latency tests, `sendmmsg` sends batches of 32 with a 5 us timeout. When FEC, loss injection, and echo mode are off, the sender copies ring slots directly into its batch buffer and fills the batch under one loop timestamp. Other modes use the general path. The receiver checks framing, deduplicates through a 65,536-message window, and publishes valid messages to shared memory.

Optional XOR FEC groups eight data messages and emits one parity datagram. Data is transmitted immediately; parity can recover one missing data datagram per generation. The cost is approximately 22% additional bytes in the observed k=8 runs, including the FEC envelope. FEC-off remains the low-latency/default measurement mode in the runners.

The fan-out sender visits destinations one at a time, which limits how far this design can scale.

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

Neither Ubuntu host had a `/dev/ptp` device. `ethtool` showed software transmit, receive, and system timestamping, but no PTP hardware clock.

Two chrony configurations were tested with the bidirectional UDP probe in [tools/clock_probe.cpp](../tools/clock_probe.cpp) because the organizers noted that chrony can sometimes achieve sub-microsecond synchronization:

| Configuration | Chrony indication | Independent TX-to-RX estimate | Independent RX-to-TX estimate | Verdict |
|---|---:|---:|---:|---|
| Amazon Time Sync, minpoll/maxpoll 0 | corrections about 0.7-3.5 us | -1.474 us | +4.824 us | reject one-way subtraction |
| Direct RX server, TX xleave plus F323 | correction about 0.370 us; source stddev about 2.1 us | -11.500 us | +9.207 us | reject one-way subtraction |

After normalizing the signs, the two directional estimates should have agreed within the error budget. They did not, so I did not treat chrony status as proof of sub-microsecond accuracy. I restored both hosts to Amazon Time Sync with minpoll/maxpoll 4.

A later Amazon Linux 2023 probe on m7i exposed the ENA PHC after enabling ena.phc_enable=1. The measured hardware-error bounds were 22.735 us and 28.038 us, which were too large for reliable one-way subtraction at the observed latency scale. That probe did not change the latency method.

The accepted topology is:

producer(TX) -> sender(TX) -> receiver(RX) -> relay(RX) -> sender(RX) -> receiver(TX) -> consumer(TX)

Producer and consumer timestamps come from the TX clock. Reported latency is full-pipeline RTT divided by two. This includes the submitted receiver in both directions. It assumes approximate path symmetry and is not a directional one-way measurement, but it avoids subtracting unsynchronized host clocks and was explicitly accepted by the organizers.

## Loss methodology

I first tried to inject loss with `tc netem` on the real ENA path. I discarded four setups before running the final matrix:

- A filtered root prio/netem qdisc at 0% loss missed messages at 500 kmsg/s.
- Netem attached to mq leaves at 0% caused sender/ring pressure and large message loss.
- Ingress IFB netem at 0% similarly changed throughput.
- A port-specific iptables statistic rule caused sendmmsg to return EPERM when it matched.

The hosts were restored to the original mq root with eight fq_codel leaves, and iptables OUTPUT was restored to ACCEPT with no added rule. ENA allowance-exceeded counters remained zero.

The final matrix uses the sender's deterministic test-drop option in both directions. Traffic still crosses the real two-host ENA path and exercises deduplication, FEC decoding, the relay, and the return path. The difference is that loss happens before `sendmmsg`. Dropped datagrams never enter the kernel and use no socket or NIC transmit work, so this method understates the cost of FEC compared with real in-flight loss. It is sender-side loss, not on-wire or `tc netem` loss. Every manifest records `impairment_method=sender_test_drop`.

The 500 kmsg/s FEC control failed its zero-lap gate, and a 400 kmsg/s calibration also lapped. The matrix therefore used two repetitions at the validated 250 kmsg/s rate. Every cell contains 3,000,000 measured messages after 100,000 warm-up messages. FEC order was reversed in repetition two.

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

At 0.01%, FEC raises median p50 by 3.7%; its p99, p99.9, and p99.99 medians are 5.7%, 6.1%, and 7.5% lower, and it eliminates all measured end-to-end missing messages. This is the only loss level where FEC's median percentiles were not uniformly worse. The two repetitions use the same drop seed, so they measure system noise around one loss realization rather than independent loss patterns. The apparent 5.7% p99 improvement sits inside run-to-run variance observed elsewhere and is not sufficient evidence of a latency win.

At 0.1%, FEC reduces median missing messages from 6,241 to 39.5 but worsens p50, p99, p99.9, and p99.99. At 1%, it reduces median missing messages from 62,315 to 4,904 and again worsens all four median percentiles.

Undelivered messages produce no latency sample, while recovered messages do enter the distribution with recovery delay. The table therefore reports percentiles and delivered counts together but does not treat delivery loss as a second optimization objective. The data supports no claimed FEC latency win; k=8 is clearly slower at 0%, 0.1%, and 1%, while the 0.01% result is inconclusive.

## Plain one-way throughput

The one-way topology runs producer and sender on TX and receiver and consumer on RX. It has no return path. Cross-host latency is not reported.

| Sender | Ring slots | Offered rate | Repetitions | Result | Sender processing |
|---|---:|---:|---:|---|---:|
| Original | 65,536 | 1M/s | 1 | failed | not instrumented |
| Original | 262,144 | 1M/s | 1 | failed | not instrumented |
| Original | 1,048,576 | 1M/s | 2 | exact | backlog drained after producer |
| Original | 1,048,576 | 1.25M/s | 2 | failed | not instrumented |
| FEC-off fast path | 65,536 | 1M/s | 2 | failed | 0.849-0.855M/s |
| FEC-off fast path | 1,048,576 | 1.25M/s | 2 | exact | 0.819-0.829M/s |
| FEC-off fast path | 1,048,576 | 1.375M/s | 2 | failed | 0.802-0.840M/s |

The 1,048,576-slot ring occupies 640 MiB per shared-memory segment, compared with 40 MiB for 65,536 slots and 160 MiB for 262,144 slots. The original code became exact at 1M only because the large ring held the finite backlog. It failed at 1.25M in both repetitions.

The fast path removes the second payload copy for plain FEC-off sends and fills a batch with one loop timestamp. It also records the exact number of source messages skipped when the reader is lapped. With the large ring, it raised the repeatable finite-run boundary to 1.25M exact and 1.375M failed. At 1.25M the sender needed 3.74-3.79 seconds to process 3.1 million messages, so the producer completed first and the sender drained afterward. This is burst capacity, not sustained 1.25M throughput. No run demonstrated 2M.

## Symmetric RTT/2 saturation

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

Exact delivery ends somewhere between 725 and 750 kmsg/s. Tail latency is already above 1 ms at 725 kmsg/s, so a low-latency deployment should stay below that point.

FEC has a lower throughput envelope. At 500 kmsg/s with k=8, the forward sender lapped 223,540 and the measured consumer received 2,744,600. At 400 kmsg/s the short calibration also lapped. At 250 kmsg/s it was exact. Compared with FEC-off exact delivery at 725 kmsg/s, the demonstrated exact FEC envelope is roughly one-third as large. This practical cost is larger than the latency table alone suggests and explains why the loss matrix uses 250 kmsg/s.

## Fan-out

Fan-out consumers run on RX and do not share the producer clock, so fan-out latency is intentionally excluded. This phase measures delivery and scaling only.

| Receivers | Source rate | Aggregate datagrams | Result |
|---:|---:|---:|---|
| 1 | 275 kmsg/s | 275 kdatagrams/s | exact 3.0M measured |
| 2 | 275 kmsg/s | 550 kdatagrams/s | exact 3.0M per receiver |
| 3 | 250 kmsg/s | 750 kdatagrams/s | exact 3.0M per receiver |
| 3 | 275 kmsg/s | 825 kdatagrams/s | rejected: sender did not exit within 72 s |
| 1 | 825 kmsg/s | 825 kdatagrams/s | rejected: sender did not exit within 64 s |

The three-receiver boundary is reproduced at 250 kmsg/s clean and 275 kmsg/s failed. The 1/2/3 matched-source controls show that adding the third destination crosses the capacity boundary at 275 kmsg/s. However, the one-destination 825 kdatagrams/s control also fails, so the data supports a sender aggregate-capacity limitation and does not prove that the sequential destination loop alone is responsible.

## Validity checks

I treated a run as valid only when it had no sender or relay ring laps, no receiver rejects, complete counters, and a complete manifest.

Runs affected by TX root-volume exhaustion are excluded. Clean-disk reruns produced the accepted 700 and 725 kmsg/s points and the rejected 750 kmsg/s boundary.

No result from the rejected netem, iptables, disk-full, FEC-overload, or incomplete fan-out attempts enters an accepted latency aggregate.

## Reproduction

Build and validate the transport with:

    make -C harness clean all test

Direct sender and receiver commands are documented in [harness/README.md](../harness/README.md). The environment and topology needed to reproduce the measurement conditions are recorded above.

Raw per-message files are not included because of their size. The per-run CSVs, aggregate tables, saved notebook outputs, logs, and this report contain the results needed for review.

## Limitations

- RTT/2 assumes approximate symmetry and averages two receiver stages plus four shared-memory hops.
- Loss is deterministic at the sender, not an on-wire tc-netem process.
- Two repetitions are enough for a compact paired check but not a strong tail confidence interval.
- Repeated loss runs use the same deterministic drop pattern.
- FEC k=8 and 200 us timeout are evaluated, not exhaustively tuned.
- Fan-out uses multiple receiver/consumer pairs on one RX host, not three physical RX hosts.
- The 1.25M one-way result depends on a 640 MiB ring and finite test duration. It is not sustainable 1.25M throughput.
- No exact 2M run was demonstrated.
- The 725 kmsg/s point is an exact-delivery ceiling observation, not a recommended low-latency operating rate.

## Recommendation

I would keep FEC off for normal latency-sensitive use. At 0.01% loss its tail numbers look slightly better, but the difference is within ordinary run-to-run variation. At 0.1% and 1%, it recovers far more messages but slows every reported percentile. It also reduces the demonstrated exact-throughput range to about one-third of the FEC-off result. The symmetric RTT/2 path delivers exactly at 725 kmsg/s, although its useful latency limit is lower. The large-ring one-way test handles a finite 1.25M-message/s burst, but the sender processes less than 0.85M messages/s. With three receivers, the measured exact-delivery point is 250 kmsg/s.
