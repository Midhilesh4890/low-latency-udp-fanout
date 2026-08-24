# Session 2 cross-host latency results

## Result

The measured network path used two m8a.2xlarge instances in one us-east-1d cluster placement group. SMT was disabled at launch (CoreCount=4, ThreadsPerCore=1). Three TX cores were isolated for producer, sender, and consumer; the remaining TX core handled housekeeping and interrupts. RX used one isolated core for a UDP echo receiver and retained CPU 0 for housekeeping.

Cross-host one-way estimates are RTT/2. The producer timestamp and final consumer timestamp both come from the TX host's clock, while every message traverses TX to RX and back. This avoids cross-host timestamp subtraction, which remained insufficiently accurate. The resulting values include the local producer-to-sender and sender-to-consumer handoffs as well as both network directions.

At 500 kmsg/s, 3,100,000 messages including warmup traversed both directions with no loss or ring lapping. At 1 Mmsg/s, the sender fell behind the producer ring, lapped 13,081 times, emitted only 265,195 of 320,000 messages, and produced 18.2683% consumer-visible loss. Saturation is therefore source-side processing/ring overwrite in this configuration, not loss of packets that reached the NIC: RX echoed every datagram the sender emitted.

## Environment and core allocation

| Property | TX | RX |
|---|---:|---:|
| Instance | i-017da3a44f14eb91e | i-00cbf1d0b9412f880 |
| Type | m8a.2xlarge | m8a.2xlarge |
| Physical/logical CPUs | 4/4 | 4/4 |
| SMT state | notsupported | notsupported |
| Isolated CPUs | 1, 2, 3 | 1, 2 |
| Measured roles | producer 1, sender 2, consumer 3 | echo receiver 1 |
| Housekeeping/IRQ CPU | 0 | 0 |
| MTU | 9001 | 9001 |

lscpu reported CPU/core pairs 0/0, 1/1, 2/2, and 3/3; there are no sibling threads. The preflight now resolves each logical CPU through physical_package_id and core_id, rejects two roles on one physical core, and aggregates TX and RX role assignments before checking when roles share a host. The regression test constructs a sibling assignment when the machine exposes one and verifies both direct and same-host orchestration rejection. Evidence is in [04_m8a_phc.log](04_m8a_phc.log), [15_core_allocation.log](15_core_allocation.log), and benchmark/test_preflight_isolation.sh.

The final TX boot line was nosmt isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3.

Keeping CPU 0 available did not starve the NIC. With this allocation, 100-packet probes had 32-byte RTT min/mean/max of 57/61/147 µs and 576-byte RTT min/mean/max of 58/62/92 µs, with zero loss. An exploratory 20 kmsg/s RTT run before isolating consumer CPU 3 had raw p50/p99/p99.9 of 211.312/310.092/317.282 µs. After isolation, these were 109.461/162.861/167.841 µs. The one-way p99.9 estimate was 83.921 µs, only 3.1% above p99, rather than the 86-fold p99.9/p99 separation seen in the earlier sibling-contended layout.

The report-grade 20k run also had a compact upper tail: p99.9 was 5.1% above p99. Its absolute p50 was higher than the shorter exploratory run, so the evidence supports removal of the cliff but not a single stable absolute latency at that rate.

## Clock assessment and RTT/2 method

Chrony was changed from its default 16-second AWS Time Sync poll to server 169.254.169.123 prefer iburst minpoll 0 maxpoll 0.

Both hosts then updated once per second with normal leap status. Chrony reported system offsets of approximately +1.1 µs on TX and +3.4 µs on RX, but source uncertainty remained 61–88 µs. A 100,000-sample clock probe reported 34.370 µs minimum RTT and a 9.499 µs minimum-RTT offset estimate. An earlier probe estimated 6.298 µs. These residuals are too large to defend direct subtraction for low-hundreds-of-microseconds measurements under a 10% clock-error criterion. The clock evidence is in [10_clock_tune.log](10_clock_tune.log) and [12_clock_probe.log](12_clock_probe.log).

RTT/2 assumes approximately symmetric forward and return paths on the same instance pair and placement group. It also assigns half of the combined local ingress and egress handoff cost to each direction. It does not establish directional asymmetry or true one-way latency. Direct cross-host one-way results were not recorded.

## Revision A/B

The A/B used same-host loopback so both revisions could run the original four-process pipeline with a shared clock. Both used the same no-SMT m8a host, 100 kmsg/s, 3,000,000 messages, 64 MiB socket buffers, and cores producer 1, sender 2, receiver 3, consumer 0. Candidate revision d53e461 is the local implementation commit measured after baseline 5dc3d3c.

| Revision | Samples | Loss | p01 | p50 | p99 | p99.9 | p99.99 | Max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 5dc3d3c | 3,000,000 | 0 | 1.160 µs | 1.380 µs | 1.770 µs | 16.575 ms | 41.233 ms | 43.794 ms |
| d53e461 | 3,000,000 | 0 | 1.310 µs | 1.540 µs | 1.970 µs | 16.974 ms | 41.405 ms | 44.016 ms |

The candidate does not improve this constrained loopback result: p50 and p99 are about 12% higher, while p99.9 and p99.99 are within 2.4% and 0.4% of baseline. Both revisions show the same discontinuity between p99 and p99.9. CPU 0 handled the consumer and housekeeping/interrupt work because four benchmark roles exhaust all four physical cores. This A/B isolates revision effects but is not a network score and cannot test the final housekeeping-separated allocation. Raw excerpts and source paths are in [17_revision_ab.log](17_revision_ab.log).

## Cross-host rate sweep

Every report-grade row contains 3,000,000 measured messages after 100,000 warmup messages. Thus p99.9 is supported by 3,000 tail samples and p99.99 by 300. Values below are one-way estimates obtained by dividing raw RTT samples by two.

| Rate | Samples | Loss | Min | p01 | p50 | p99 | p99.9 | p99.99 | Max |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 kmsg/s | 3,000,000 | 0 | 19.746 µs | 43.306 µs | 152.172 µs | 258.812 µs | 272.107 µs | 279.677 µs | 2.490 ms |
| 100 kmsg/s | 3,000,000 | 0 | 18.320 µs | 31.205 µs | 87.420 µs | 172.476 µs | 180.331 µs | 1.148 ms | 2.371 ms |
| 500 kmsg/s | 3,000,000 | 0 | 23.705 µs | 34.046 µs | 118.601 µs | 213.782 µs | 1.680 ms | 2.279 ms | 2.452 ms |

The highest lossless rate tested was 500 kmsg/s. Its retained raw distribution is not a simple broadening of the body:

| One-way latency bin | Share |
|---|---:|
| ≤50 µs | 4.126400% |
| 50–100 µs | 28.942633% |
| 100–200 µs | 64.431133% |
| 200–500 µs | 2.143900% |
| 0.5–1 ms | 0.156300% |
| 1–2 ms | 0.146533% |
| >2 ms | 0.053100% |

The 5,989 samples above 1 ms one-way occurred in five contiguous runs. The longest contained 5,719 consecutive sequence numbers (2096987 through 2102705), showing an episodic queueing/stall mode rather than independent random outliers. The p99.9 jump at 500k is therefore a saturation precursor despite exact delivery.

The 1 Mmsg/s diagnostic was intentionally only 300,000 requested samples and is not percentile-grade. It observed 245,195 samples, 54,805 missing sequence numbers, one-way p50 around 32.8 ms, and source-ring lapping. It serves only to identify the first failed tier and the failure mechanism. Exact raw RTT statistics and counters are in [16_rate_sweep.log](16_rate_sweep.log); retained artifacts are under benchmark/results/session2_sweep_* and benchmark/results/session2_diag_postiso_1m_failed.

## Fan-out

Three receiver/consumer pairs require six physical benchmark cores in addition to the producer and sender. The two hosts exposed eight physical cores total. A valid run also requires at least one housekeeping/IRQ core per active host:

- all RX roles on one host requires at least 3 physical cores on TX and 7 on RX;
- splitting one receiver pair onto TX requires at least 5 physical cores on each host.

The available 4+4 layout can place all eight benchmark threads only by consuming both housekeeping cores, recreating the NIC-starvation and interrupt-contention failure modes. Three-way fan-out was therefore NOT_RUN. A defensible attempt needs at least ten physical cores across the two hosts, with the practical layout being two no-SMT eight-core instances.

## m8a PHC check

The m8a ENA interface exposes software transmit, receive, and system-clock timestamping only. ethtool -T enp39s0 reported PTP Hardware Clock: none, hardware transmit modes none, and hardware receive filters none; neither /dev/ptp0 nor /dev/ptp_hyperv existed. m8a therefore does not provide the PHC needed to replace RTT/2 in this environment. Capacity attempts in us-east-1a and us-east-1c failed without launching billable resources; us-east-1d succeeded. See [04_m8a_phc.log](04_m8a_phc.log).

## Anomalies

- A blocking first RTT adapter serialized send and receive work and produced raw 20k p50/p99.9 of 352.573/863.496 µs. It was rejected and replaced with asynchronous echo draining; the discarded run is benchmark/results/session2_preiso_20k_fixed.
- A partial 20k report-grade attempt hit a fixed 30-second orchestration counter timeout although its nominal stream duration was 155 seconds. No result directory was overwritten. The runner now derives its completion window from count/rate, and session2_sweep_20k_3m_retry completed with exact counters.
- The report-grade 20k p50 estimate of 152.172 µs is 2.8 times the shorter post-isolation exploratory p50 of 54.731 µs. Both have compact p99-to-p99.9 tails and zero loss, but the absolute run-to-run shift merits replication.
- Chrony was briefly given a malformed minpollx{20} line during tuning. Chrony rejected it, the valid AWS source line was restored, and normal status was verified before any retained measurement. The sequence is preserved in [10_clock_tune.log](10_clock_tune.log).
- One 576-byte ping command lost its SSH session and exited 255; the immediately repeated 100-packet probe completed with zero loss and 58/62/92 µs min/mean/max. Both attempts are preserved in [15_core_allocation.log](15_core_allocation.log).

## Spend and teardown

The two successful instances accumulated 2.4025 instance-hours. The AWS catalog rate effective 2026-08-01 was $0.48688 per m8a.2xlarge hour, yielding $1.1697 compute. Estimated public IPv4 and 8 GiB gp3 charges add approximately $0.0141, for a session estimate of **$1.184**, excluding taxes and negligible control traffic. Same-AZ private benchmark traffic has no cross-AZ charge. Details are in [19_cost.log](19_cost.log).

Both instances were terminated at 10:38:24 UTC. At 10:39:49 UTC, independent inventory queries returned both instance states as terminated, no volumes with the session tag, no addresses for either instance, and no ENIs for the session security group. Security-group lookup returned InvalidGroup.NotFound; placement-group lookup returned InvalidPlacementGroup.Unknown. Teardown evidence is summarized in [18_teardown.log](18_teardown.log).
