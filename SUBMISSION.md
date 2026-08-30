# Low-latency UDP fan-out transport

## Summary

I built the transport in C++17 on Linux, using the fixed producer and consumer interfaces from the challenge. The producer writes to a lock-free shared-memory ring. The sender reads that ring and sends UDP batches with `sendmmsg`; the receiver uses `recvmmsg`, validates and deduplicates each frame, and writes accepted messages to the output ring. A sender can target more than one receiver. There is also an optional XOR FEC mode that can recover one missing datagram from a group of eight.

For the latency runs I used batches of 32, a 5 microsecond batch timeout, 64 MiB socket buffers, and a 65,536-message deduplication window. FEC was off unless a run explicitly tested it. The two `m7i.4xlarge` instances were in the same Availability Zone and used ENA with MTU 9001. Each host had eight physical cores, no SMT, and six cores isolated for the benchmark.

Main results:

- In the plain one-way topology, the original 65,536-slot ring was exact at 800,000 messages/s and failed at 1,000,000. A 1,048,576-slot ring made a finite 1,000,000-message/s run exact by buffering backlog. After the FEC-off sender fast path, 1,250,000 messages/s was exact in two 3,000,000-message repetitions, while 1,375,000 failed twice. Sender processing was 0.819-0.829 million messages/s at that boundary, so 1.25M is burst capacity, not sustainable throughput. The measurements do not demonstrate 2M.
- In the symmetric RTT/2 topology, zero-loss delivery remained exact through 725,000 source messages/s. At 750,000 messages/s the producer-to-sender shared-memory ring lapped 55,349 times. Tail latency had already collapsed at 725,000 messages/s, so the practical operating knee is lower than the exact-delivery ceiling.
- Three-destination fan-out delivered exactly at 250,000 source messages/s, or 750,000 transmitted datagrams/s. The sender did not complete at 275,000 source messages/s with three destinations. A one-destination control also failed at the equivalent aggregate rate of 825,000 datagrams/s, identifying aggregate sender capacity rather than proving that destination iteration alone was responsible.
- FEC reduced missing messages substantially under synthetic loss, but it was slower at every zero-loss percentile and reduced the usable throughput envelope to roughly one third of FEC-off. The data supports keeping FEC disabled for latency-first operation.

The [analysis notebook](analysis.ipynb) contains the tables and saved plots. The CSV files, logs, and detailed report are under [`results/`](results/).

## Transport design

### Data path

The transport does not change the producer and consumer framing fields, including `seq_id` and `send_ts_ns`. Its data path is:

```text
producer -> shared-memory ring -> sender -> UDP/ENA -> receiver -> shared-memory ring -> consumer
```

The sender busy-polls the input ring and builds fixed-size batches. On the common path, with FEC, loss injection, and echo mode all disabled, it copies each ring slot directly into the batch buffer. One loop timestamp is used while filling that batch, and the buffer is not reused until `sendmmsg` returns. The other modes go through the general path.

A batch is flushed when it reaches 32 datagrams or its oldest member reaches the 5 microsecond timeout. `sendmmsg` amortizes system-call overhead while the timeout bounds batching delay at low rates. The receiver uses `recvmmsg`, validates datagram framing, runs the optional FEC decoder, applies the deduplication window, and publishes accepted frames to shared memory.

Each destination has its own connected UDP socket. For fan-out, the sender sends the finished batch to each destination in sequence. This keeps the one-destination case simple, but sender work increases with every added destination.

### Deduplication

The receiver tracks a 65,536-message sequence window. In-order and previously unseen reordered messages are accepted once; duplicates and messages older than the window are rejected. Window advancement also records confirmed sequence loss and maximum observed reorder depth. The window has a fixed memory cost and keeps duplicate network frames or recovered FEC frames from reaching the consumer twice.

### Forward error correction

For `fec_k=8`, each data datagram receives a compact generation envelope and one XOR parity datagram is emitted for every generation of eight. Data is sent immediately; it does not wait for parity. A receiver can reconstruct one missing member after the other seven data members and parity arrive. A 200 microsecond generation timeout closes partial generations at low rates. The measured byte overhead was approximately 22%, including envelopes and parity.

FEC is optional because recovery adds serialization work, packets, decoder state, and delay. A recovered message stays in the latency sample set, including the time spent waiting for recovery.

## Design decisions

I chose UDP to avoid connection-level head-of-line blocking and retransmission delays. It leaves gaps, duplicates, and recovery visible to the application, so the receiver has to validate sequence numbers and the application has to decide whether a late recovered message is better than a missing one.

Batch size 32 with a 5 microsecond timeout was retained because it completed every accepted rate and fan-out run without loss. A batch-size-one diagnostic lost eight messages at 20,000 messages/s and was excluded. Disabling ENA receive moderation slightly improved low-rate p50 and p99 but worsened p99.9, p99.99, and maximum latency, so the ENA adaptive default was restored.

I did not use direct cross-host timestamp subtraction. The Ubuntu hosts exposed no PTP hardware clock, and the two directions of [tools/clock_probe.cpp](tools/clock_probe.cpp) still disagreed after chrony tuning. A later configuration check exposed the ENA PHC on Amazon Linux 2023 by enabling `ena.phc_enable=1`, but the two Nitro PHCs reported hardware error bounds of 22.735 and 28.038 microseconds. Those bounds were too large for the measured latency scale. The report therefore uses the same-clock symmetric RTT/2 topology described below.

Kernel bypass, `SO_BUSY_POLL`, and a multi-destination `sendmmsg` redesign were not evaluated. The sender and receiver already busy-poll in user space.

## Measurement method

### Environment

| Property | Value |
|---|---|
| Region/AZ | AWS `us-east-1d` |
| Instances | two `m7i.4xlarge` |
| Placement | cluster placement group linked to a precision-time parent |
| CPU layout | 8 physical cores, 1 thread/core, SMT unsupported |
| Kernel | Ubuntu `6.17.0-1019-aws` |
| Network | ENA, MTU 9001 |
| Isolated CPUs | 1-6 with `isolcpus`, `nohz_full`, `rcu_nocbs` |
| Housekeeping CPUs | 0 and 7 |

The full environment record is [`results/environment.log`](results/environment.log).

### Latency topology

The organizers confirmed that the RTT/2 setup was acceptable.

Cross-host latency uses a symmetric round trip:

```text
producer(TX)
  -> sender(TX)
  -> receiver(RX)
  -> shared-memory relay(RX)
  -> sender(RX)
  -> receiver(TX)
  -> consumer(TX)
```

The producer and final consumer timestamp on the TX host, so no cross-host clock subtraction occurs. Reported values are the complete pipeline RTT divided by two. Both network directions execute the submitted sender, receiver, FEC, and deduplication paths. This method assumes approximate path symmetry and is a one-way-equivalent estimate, not directional one-way latency.

Each accepted percentile run contains 3,000,000 measured messages after 100,000 warm-up messages. Runs are rejected if any sender or relay laps its input ring, receivers reject malformed input, counters are incomplete, or required manifests are missing.

## Plain one-way throughput

The one-way topology places the producer and sender on TX and the receiver and consumer on RX. There is no return path. Cross-host timestamps are not used. Each accepted run consumes exactly 3,000,000 measured messages after 100,000 warm-up messages, reports zero consumer drops, publishes all 3,100,000 messages, and has zero sender laps.

| Sender | Ring slots | Ring memory per segment | Highest repeatable exact finite rate | First repeatable failed rate | Observed sender processing |
|---|---:|---:|---:|---:|---:|
| Original | 65,536 | 40 MiB | 800k/s | 1M/s | not instrumented |
| Original | 1,048,576 | 640 MiB | 1M/s | 1.25M/s | backlog drained after producer completion |
| FEC-off fast path | 65,536 | 40 MiB | not tested between 800k and 1M | 1M/s | 0.849-0.855M/s |
| FEC-off fast path | 1,048,576 | 640 MiB | 1.25M/s | 1.375M/s | 0.819-0.829M/s at the boundary |

The fast path removes the ring-slot to stack-buffer to batch-buffer double copy for plain FEC-off sends. It also fills a batch using one loop timestamp and records exact skipped-message counts. The generic path remains active for FEC, sender-side impairment, and echo mode.

The 1,048,576-slot result is a burst-buffer improvement. At 1.25M/s the sender took 3.74-3.79 seconds to process 3.1 million messages, or 0.819-0.829M/s. The producer completed earlier and the sender drained the backlog afterward. The large ring also uses 16 times the shared memory and showed a lower processing rate than the small ring in some runs, consistent with cache and TLB cost. No low-rate latency run used the large ring.

The exact counters are in [`results/oneway_throughput_runs.csv`](results/oneway_throughput_runs.csv) and the boundary summary is in [`results/oneway_throughput_summary.csv`](results/oneway_throughput_summary.csv).

## Symmetric RTT/2 saturation and latency

Values are RTT/2 microseconds. The 250,000 and 700,000 rows show the range across two accepted runs; the other accepted rows have one run.

| Source rate | p50 | p99 | p99.9 | p99.99 | Result |
|---:|---:|---:|---:|---:|---|
| 20k/s | 22.441 | 35.020 | 49.330 | 77.620 | exact delivery |
| 100k/s | 50.097 | 70.770 | 85.147 | 110.627 | exact delivery |
| 250k/s | 52.634-54.551 | 75.004-96.279 | 112.179-167.953 | 140.597-187.914 | exact delivery |
| 500k/s | 74.728 | 104.511 | 121.588 | 146.392 | exact delivery |
| 600k/s | 76.669 | 173.860 | 212.006 | 263.792 | exact delivery |
| 700k/s | 84.204-136.827 | 194.691-295.659 | 265.356-314.524 | 305.694-326.017 | exact delivery |
| 725k/s | 168.219 | 1,109.958 | 1,168.397 | 1,242.297 | exact delivery; tail collapse |
| 750k/s | invalid | invalid | invalid | invalid | rejected; 55,349 sender-ring laps |

Exact delivery stops somewhere between 725,000 and 750,000 messages/s. The 750,000 failure is not an ENA allowance drop; the forward sender falls behind the producer before those packets can reach the NIC. Latency rises sharply at 725,000, so the useful operating rate for a latency-sensitive workload is lower.

The complete saturation rows are in [`results/saturation_summary.csv`](results/saturation_summary.csv); the lower-rate evidence is in [`results/rate_sweep.log`](results/rate_sweep.log).

## Fan-out results

Fan-out measurements validate delivery and capacity only. Their consumers run on the RX host and do not share the producer clock, so their latency fields are not used.

| Destinations | Source rate | Aggregate datagram rate | Result |
|---:|---:|---:|---|
| 1 | 275k/s | 275k/s | exact 3.0M measured |
| 2 | 275k/s | 550k/s | exact 3.0M per destination |
| 3 | 250k/s | 750k/s | exact 3.0M per destination |
| 3 | 275k/s | 825k/s | rejected; sender completion timeout |
| 1 | 825k/s | 825k/s | rejected; sender completion timeout |

The one-destination control also fails at 825,000 datagrams/s. That points to an aggregate sender-capacity limit, not just the cost of looping over three destinations. Detailed counters are in [`results/fanout.log`](results/fanout.log) and [`results/fanout_summary.csv`](results/fanout_summary.csv).

## Loss and FEC results

The loss matrix ran at 250,000 messages/s with two repetitions per cell. Loss was applied independently in both directions. Values below are medians of the two runs.

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

At zero loss, FEC is slower at every reported percentile. At 0.1% and 1%, it greatly improves delivery completeness but worsens all four latency percentiles. At 0.01%, it is the only tested loss level where the FEC tail percentiles are not worse: median p99 is 5.7% lower, p99.9 is 6.1% lower, and p99.99 is 7.5% lower, while p50 is 3.7% higher.

That 0.01% result is not strong evidence of a general tail improvement. Both repetitions use the same deterministic drop seed, so they measure system noise around one loss realization rather than independent loss patterns. The 5.7% p99 difference also lies inside the run-to-run variation visible in the individual cells. The supported conclusion is limited to: **0.01% was the only tested loss level where FEC's median tail percentiles were not worse.**

The impairment method further biases the comparison in FEC's favor. Loss is injected in the sender before `sendmmsg`. A dropped datagram therefore never enters the kernel, consumes socket-buffer space, or uses NIC transmission time. Real in-flight loss would charge the sender the complete transmit cost before the packet disappeared, making FEC more expensive relative to FEC-off than this matrix shows. The results must not be described as `tc netem` or on-wire loss.

Throughput is the larger practical FEC cost in the symmetric RTT/2 pipeline. FEC-off delivered exactly at 725,000 messages/s and failed at 750,000. FEC k=8 was exact at 250,000 messages/s but already lapped the sender ring in the 400,000 calibration and again at 500,000. Its demonstrated exact-delivery envelope is therefore roughly one third of the FEC-off ceiling, a much larger penalty than the 250,000-message/s latency table alone suggests.

The individual loss runs and their spread are retained in [`results/loss_matrix_runs.csv`](results/loss_matrix_runs.csv) and [`results/loss_matrix_summary.csv`](results/loss_matrix_summary.csv).

### Recommendation

For a latency-first setup, I would leave FEC off. I would enable `fec_k=8` only when recovering missing messages matters more than throughput, and only after testing with a realistic on-wire loss pattern. These runs do not show a reliable latency improvement from FEC.

## Reproduction

### Build and unit validation

    make -C harness clean all test

### Direct transport run

The sender and receiver can be run directly on two Linux hosts using the commands in [harness/README.md](harness/README.md). Use an m7i.4xlarge instance in one Availability Zone, MTU 9001, ENA defaults, one process per isolated physical core, and CPUs 0 and 7 for housekeeping and interrupts to match the measured environment. Add additional sender destination arguments for fan-out, and set fec-k to 8 on the sender to enable the measured XOR FEC mode.

The notebook includes saved outputs and reads only the CSV files in `results/`.

## Limitations

- RTT/2 assumes approximate path symmetry and combines two receiver stages with four shared-memory handoffs. It is not directional one-way latency.
- Sender-side synthetic loss avoids kernel and NIC work for dropped datagrams and therefore understates the relative cost of FEC under real in-flight loss.
- The loss matrix has two repetitions and one fixed loss pattern, which is insufficient for a strong tail-confidence claim.
- Only XOR FEC with `k=8` and a 200 microsecond generation timeout was measured.
- The FEC saturation boundary is coarse: 250,000 messages/s was exact and 400,000 messages/s lapped. The true boundary was not narrowed further.
- Fan-out uses three receiver/consumer pairs on one RX host rather than three physical destination hosts.
- Fan-out performs sequential batch transmission to each destination.
- The 1.25M one-way result is finite-run burst capacity. Measured sender processing was 0.819-0.829M/s at that boundary, and 2M was not demonstrated.
- A 1,048,576-slot ring consumes 640 MiB per shared-memory segment. Low-rate latency with that ring was not measured.
- Kernel bypass, `SO_BUSY_POLL`, and multi-destination batching were not evaluated.
- The reported values are specific to the listed EC2 placement, CPU isolation, kernel, and ENA configuration.

## Evidence

The compact evidence needed to check each result is retained in [`results/`](results/). [`results/measurement_report.md`](results/measurement_report.md) contains the detailed gates, rejected impairment methods, and counter-level interpretation. The notebook presents the same committed CSV data graphically. Local validation output is in [`results/tests.log`](results/tests.log).

- [AWS local Amazon Time Sync and PHC configuration](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/configure-ec2-ntp.html)
