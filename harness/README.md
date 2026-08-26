# Transport harness

The C++17 harness contains the fixed shared-memory producer and consumer, the UDP transport processes, and unit and pipeline tests.

## Components

| Path | Purpose |
|---|---|
| `src/producer.cpp` | Generates timestamped, monotonically sequenced messages and publishes them to shared memory. |
| `src/sender.cpp` | Reads shared memory, batches UDP datagrams, performs fan-out, and optionally generates XOR FEC parity. |
| `src/receiver.cpp` | Receives UDP batches, validates frames, performs FEC recovery and deduplication, and publishes accepted messages. |
| `src/relay.cpp` | Connects the forward and return paths in the symmetric RTT topology. |
| `src/consumer.cpp` | Measures delivery counts and latency percentiles. |
| `include/shm_ring.h` | Single-writer shared-memory broadcast ring with lap detection. |
| `include/message.h` | Fixed message framing and market-data payload types. |
| `include/fec.h` | XOR FEC wire format and recovery state. |
| `include/dedupe_window.h` | Bounded sequence-window deduplication. |
| `test/` | Unit and local pipeline regression tests. |

## Message and ring behavior

Every message starts with `seq_id`, `send_ts_ns`, type, version, and body length. The producer stamps the sequence and timestamp immediately before publication. The consumer derives missing-message counts from sequence gaps and latency from the difference between receive and send timestamps.

The shared-memory ring is non-blocking for the writer. A reader that falls more than one ring behind detects overwritten slots, records a lap, and resumes from the oldest retained sequence. Accepted benchmark runs require zero laps.

## Build and test

```bash
make -C harness clean all test
bash harness/test/test_rtt_mode.sh
bash harness/test/test_pipeline_rtt_mode.sh
```

## Local baseline

```bash
taskset -c 2 harness/bin/producer \
  --count 500000 --rate 200000 --type mixed &

sleep 0.02
taskset -c 4 harness/bin/consumer --from-edge
```

`--from-edge` starts the consumer at the current writer position, excluding any prefix published before attachment. Without it, startup backlog becomes part of the latency distribution.

Producer and consumer timestamps are directly comparable only when both processes share a clock. Cross-host measurements use the symmetric RTT/2 topology documented in [`../SUBMISSION.md`](../SUBMISSION.md).

## Runtime model

The producer pacing loop, sender ring reader, and receiver path busy-poll. Report-grade runs pin each process to a distinct isolated physical core and move housekeeping work and interrupts elsewhere. The complete CPU mapping and host configuration are documented in [`../SUBMISSION.md`](../SUBMISSION.md).
