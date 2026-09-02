# Transport internals

This directory contains the four PulseFanout processes and the header-only transport primitives they share.

## Components

| Path | Purpose |
|---|---|
| `src/producer.cpp` | Generates versioned market-data-style frames and publishes them to shared memory. |
| `src/sender.cpp` | Reads a ring, batches UDP datagrams, performs fan-out, and optionally generates XOR parity. |
| `src/receiver.cpp` | Receives batches, validates frames, performs FEC recovery and deduplication, and publishes accepted frames. |
| `src/consumer.cpp` | Reads accepted frames and reports delivery and latency percentiles. |
| `include/shm_ring.h` | Single-writer shared-memory broadcast ring with lap and corruption detection. |
| `include/message.h` | Fixed demo message layouts and frame validation. |
| `include/fec.h` | Bounded XOR FEC wire envelope, encoder, and decoder. |
| `include/dedupe_window.h` | Fixed-memory sequence-window deduplication. |
| `test/test_harness.cpp` | Unit and malformed-input regression tests. |

## Runtime model

The producer, sender, receiver, and consumer busy-poll. Assign each long-running process its own physical CPU core for stable low-latency measurements. The defaults favor a convenient local run; tune ring capacity, socket buffers, batches, and CPU placement for the deployment.

The shared-memory ring has one writer and any number of independent readers. A slow reader can be lapped; consumers report lap events rather than blocking the writer. Ring capacity must be a power of two.

The sender opens one connected UDP socket per destination. It flushes when the batch reaches `--batch-size` or the oldest datagram reaches `--batch-timeout-us`. Plain traffic uses the optimized path; FEC and impairment modes use the general path.

The receiver accepts raw frames and FEC-wrapped frames on the same socket. Before publication, it verifies the application version, type, and exact length, then applies the sequence window.

## Failure semantics

- Invalid configuration exits with status 2.
- Startup or I/O failures exit with status 1.
- Idle timeout and a requested SIGINT/SIGTERM produce a final counter report and clean up owned shared-memory names.
- Ring laps are observable data loss and appear in sender or consumer counters.
- Invalid network frames increment the receiver's `rejected` counter.

## Verification

```bash
make -C harness clean all test
make -C harness smoke
```

Use the root [README](../README.md) for a complete local run and deployment boundaries.
