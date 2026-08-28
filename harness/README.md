# Transport harness

The C++17 harness contains the fixed shared-memory producer and consumer, the UDP sender and receiver, and focused unit tests.

## Components

| Path | Purpose |
|---|---|
| src/producer.cpp | Generates timestamped, monotonically sequenced messages and publishes them to shared memory. |
| src/sender.cpp | Reads shared memory, batches UDP datagrams, performs fan-out, and optionally generates XOR FEC parity. |
| src/receiver.cpp | Receives UDP batches, validates frames, performs FEC recovery and deduplication, and publishes accepted messages. |
| src/consumer.cpp | Measures delivery counts and latency percentiles. |
| include/shm_ring.h | Single-writer shared-memory broadcast ring with lap detection. |
| include/message.h | Fixed message framing and market-data payload types. |
| include/fec.h | XOR FEC wire format and recovery state. |
| include/dedupe_window.h | Bounded sequence-window deduplication. |
| test/test_harness.cpp | Unit validation for metrics, rings, framing, deduplication, and FEC. |

## Build and test

    make -C harness clean all test

## Direct two-host run

Build on both hosts. Start these commands in order, using a separate terminal for each command.

On the receiving host, start the receiver and then the consumer:

    harness/bin/receiver --out-shm /fanout_ring_out --slots 65536 --bind 0.0.0.0 --port 9000 --count 1000000 --batch-size 32

    harness/bin/consumer --shm /fanout_ring_out --slots 65536 --count 1000000

On the sending host, start the producer and then the sender before the producer delay expires:

    harness/bin/producer --shm /fanout_ring --slots 65536 --count 1000000 --rate 250000 --start-delay-ms 5000 --type mixed

    harness/bin/sender --in-shm /fanout_ring --slots 65536 --dst RX_PRIVATE_IP:9000 --count 1000000 --batch-size 32 --batch-timeout-us 5 --fec-k 0

Repeat the destination argument to fan out to multiple receivers. Set fec-k to 8 to enable the measured XOR FEC mode.

Cross-host delivery counts are valid, but direct one-way latency subtraction requires synchronized clocks. The submitted latency results use the symmetric same-clock RTT/2 method described in [SUBMISSION.md](../SUBMISSION.md).

## Runtime model

The producer pacing loop, sender ring reader, and receiver path busy-poll. Measurement runs pin each process to a distinct isolated physical core and move housekeeping work and interrupts elsewhere. The measured CPU mapping and host configuration are documented in [SUBMISSION.md](../SUBMISSION.md).
