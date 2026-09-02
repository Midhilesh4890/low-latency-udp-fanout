# PulseFanout

PulseFanout is a Linux/C++17 reference transport for moving high-rate market-data-style messages from local producers to one or more UDP consumers. It combines a single-writer shared-memory ring, batched UDP I/O, bounded deduplication, and optional XOR forward error correction.

The project is useful as:

- a compact foundation for low-latency telemetry or market-data distribution;
- a reproducible lab for batching, fan-out, packet loss, reordering, and FEC;
- an example of a shared-memory-to-network pipeline with measurable delivery and latency.

It is not yet a drop-in production messaging system. The current wire format is a homogeneous-host ABI, security controls are intentionally minimal, and the transport has not been independently audited. See [Current limitations](#current-limitations).

## Data path

```text
producer
   │  POSIX shared memory
   ▼
sender ───── batched UDP ─────► receiver
                                  │  validation + dedupe + optional FEC
                                  ▼
                              POSIX shared memory
                                  │
                                  ▼
                               consumer
```

One sender can connect to multiple destinations. The default fast path uses `sendmmsg` and `recvmmsg`; the receiver validates every decoded application frame before publishing it to shared memory.

## Highlights

- Lock-free, single-writer shared-memory broadcast ring
- Batched Linux UDP transmit and receive
- Multiple connected UDP destinations per sender
- Fixed-memory sliding-window deduplication
- Optional single-loss XOR recovery by generation
- Mixed trade, best-bid/offer, and order-book demo messages
- Delivery counts and latency percentiles
- Deterministic loss and reorder injection for local experiments
- Unit tests, sanitizer builds, and a full localhost integration test
- Graceful SIGINT/SIGTERM shutdown and monotonic internal deadlines

## Requirements

- Linux
- A C++17 compiler (GCC or Clang)
- CMake 3.16+ or GNU Make
- POSIX shared memory and Linux `sendmmsg`/`recvmmsg`

## Build and verify

With CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./scripts/smoke_test.sh ./build/bin
```

With Make:

```bash
make -C harness clean all test
make -C harness smoke
```

The smoke test starts all four processes, sends 5,000 mixed messages over localhost UDP, and fails unless the consumer receives every message with no validation errors.

## Run it manually

Open four terminals. Start the receiving side first:

```bash
./build/bin/receiver \
  --out-shm /pulsefanout_out --slots 65536 \
  --bind 0.0.0.0 --port 9000 --count 100000

./build/bin/consumer \
  --shm /pulsefanout_out --slots 65536 --count 100000
```

Then start the sending side. The startup delay gives the sender time to attach to the input ring:

```bash
./build/bin/producer \
  --shm /pulsefanout_in --slots 65536 --count 100000 \
  --rate 100000 --start-delay-ms 1000 --type mixed

./build/bin/sender \
  --in-shm /pulsefanout_in --slots 65536 \
  --dst 127.0.0.1:9000 --count 100000 \
  --batch-size 32 --batch-timeout-us 50
```

Add more `--dst host:port` arguments for fan-out. Set `--fec-k 8` on the sender to wrap data in eight-message FEC generations; the receiver detects FEC envelopes automatically.

## Operational controls

| Component | Important options |
|---|---|
| producer | `--shm`, `--slots`, `--count`, `--rate`, `--type`, `--start-delay-ms` |
| sender | `--in-shm`, repeated `--dst`, `--batch-size`, `--batch-timeout-us`, `--fec-k`, `--from-edge` |
| receiver | `--out-shm`, `--bind`, `--port`, `--batch-size`, `--dedupe-window`, `--rcvbuf` |
| consumer | `--shm`, `--count`, `--from-edge`, `--skip`, `--csv` |

`--count 0` keeps a component running until it reaches its idle timeout or receives SIGINT/SIGTERM. Ring sizes and the deduplication window must be powers of two.

## Safety and correctness boundaries

PulseFanout now defends the main untrusted boundaries:

- shared-memory headers are checked for magic, slot count, and slot size;
- ring reads and writes reject zero-length and oversized frames;
- UDP frames must have a known type, supported version, and exact body length;
- FEC generations and parity payloads are bounded before allocation or recovery;
- receiver publication failures propagate through the FEC decoder;
- wall-clock timestamps remain available for measurements, while timeouts use a monotonic clock.

## Repository layout

- `harness/src/` — producer, sender, receiver, and consumer applications
- `harness/include/` — transport, protocol, FEC, deduplication, and metrics headers
- `harness/test/` — deterministic unit tests
- `scripts/smoke_test.sh` — end-to-end localhost verification
- `.github/workflows/ci.yml` — compiler-matrix, integration, and sanitizer CI

## Current limitations

- The application structs are transmitted in native byte order and native C++ layout. Peers must use the same ABI and architecture.
- UDP provides no authentication, encryption, congestion control, or retransmission.
- XOR FEC can recover at most one missing datagram per generation.
- Fan-out sends each completed batch to destinations sequentially.
- Busy polling favors latency over CPU efficiency.
- Shared-memory startup is coordinated operationally; there is no supervisor or service discovery yet.

## Roadmap

The most useful next increments are:

1. an explicit endian-stable wire codec with a protocol magic and compatibility tests;
2. Prometheus/OpenTelemetry metrics and structured JSON status output;
3. configuration files plus systemd service units;
4. authenticated encryption or deployment behind a trusted network boundary;
5. destination-level backpressure and health accounting;
6. a versioned compatibility and performance-regression suite.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the local workflow. Security-sensitive reports should follow [SECURITY.md](SECURITY.md).
