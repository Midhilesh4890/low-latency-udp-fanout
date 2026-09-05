# PulseFanout

PulseFanout is a Linux/C++17 reference transport for moving high-rate market-data-style messages from local producers to one or more UDP consumers. It combines a single-writer shared-memory ring, batched UDP I/O, bounded deduplication, and optional Reed-Solomon forward error correction.

The project is useful as:

- a compact foundation for low-latency telemetry or market-data distribution;
- a reproducible lab for batching, fan-out, packet loss, reordering, and FEC;
- an example of a shared-memory-to-network pipeline with measurable delivery and latency.

It is not yet a drop-in production messaging system. It provides a portable wire protocol and an optional mutually authenticated TLS relay, but has not been independently audited. See [Current limitations](#current-limitations).

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

- Synchronized single-writer broadcast ring with robust process-shared locks and bounded backpressure
- Batched Linux UDP transmit and receive
- Multiple connected UDP destinations per sender
- Fixed-memory sliding-window deduplication
- Configurable Reed-Solomon recovery (1?16 parity shards, up to 128 data shards)
- Portable big-endian wire codec with explicit field serialization
- Optional TLS 1.3 transport over framed Unix streams and TCP
- Independent bounded destination queues and configurable idle backoff
- Durable SQLite relay outbox/inbox with acknowledgement and crash replay
- Stream/epoch isolation for independent publishers
- Authenticated remote discovery and configurable service supervision
- Exclusive shared-memory creation, reader readiness, and a local supervisor
- Mixed trade, best-bid/offer, and order-book demo messages
- Delivery counts and latency percentiles
- Deterministic loss and reorder injection for local experiments
- Unit tests, sanitizer builds, and a full localhost integration test
- Graceful SIGINT/SIGTERM shutdown and monotonic internal deadlines

## Requirements

- Linux
- A C++17 compiler (GCC or Clang)
- CMake 3.16+ or GNU Make
- Python 3 with TLS 1.3 support for supervision and secure relay mode
- OpenSSL command-line tool for certificate-generating integration tests
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

## Run with coordinated startup

~~~bash
python3 scripts/run_pipeline.py --bin-dir build/bin --count 5000 \
  --fec-k 8 --fec-parity 3 --state-file /tmp/pulsefanout-state.json
~~~

The supervisor assigns unique shared-memory names, checks initialization, starts
readers before publication, monitors exit codes, and terminates the group on
failure. --restarts N restarts the whole group with a fresh epoch. The optional
state file publishes current ring names and PIDs atomically for local discovery
and is removed on shutdown. Restarts do not replay prior messages. --timeout
bounds a run; this demo supervisor requires a positive count.

## Run it manually

Open four terminals. Start the receiving side first:

```bash
./build/bin/receiver --allow-insecure-udp \
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

./build/bin/sender --allow-insecure-udp \
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

## Reliability controls

- Raw UDP requires --allow-insecure-udp on sender and receiver. Unix-stream
  endpoints connect to the TLS relay without exposing an unauthenticated
  network listener.
- --durable-db on both relays enables bounded, committed SQLite outbox/inbox
  storage, remote acknowledgements, reconnection, and replay. Pair the sender
  with --durable-acks and receiver with --ack-publish --stream-reconnect.
- Rings use robust process-shared mutexes to prevent torn concurrent copies.
  Default backpressure waits for active readers, then fails explicitly after
  two seconds instead of overwriting unread data. The benchmark-only
  PULSEFANOUT_RING_POLICY=overwrite mode permits intentional lapping.
- Each destination has its own bounded queue and thread. A failed or full
  destination is isolated; healthy destinations finish. The sender reports
  the incomplete destination and returns failure after draining healthy queues.
- Every network frame carries stream and epoch identities. Receivers maintain
  separate bounded FEC/deduplication state for each identity.
- Metrics retain at most 65,536 samples per distribution. Percentiles become
  approximate after that limit; consumer totals, minimum, maximum, and mean
  remain exact. Optional consumer records stream to disk.
- scripts/supervise_services.py manages configured service groups, including
  relays. scripts/discovery.py serves/fetches registry state over mutual TLS
  and rejects stale process registrations.

See [transport operations](docs/transport.md) for complete commands and delivery
semantics. This is at-least-once durable relay delivery, not transactional
exactly-once application processing. Capacity exhaustion fails explicitly.
FEC remains finite; use reliable transport when losses exceed its repair budget.

**Compatibility:** network protocol PFS3/PUL3, application schema 2 and
shared-memory layout 3 require upgrading peers and recreating old rings together.

PULSEFANOUT_SPIN_US and PULSEFANOUT_SLEEP_US default to 50 microseconds. Set the
latter to 0 for continuous polling. --wait-readers 1 waits up to five seconds
before publication; attaching before creation also waits up to five seconds.
The local pipeline helper explicitly selects the UDP benchmark mode.

## Verification

CMake runs unit and integration tests when Python 3 and the OpenSSL command-line
tool are present. No OpenSSL development library is needed. Tests cover golden
wire bytes, malformed/truncated frames, every three-erasure combination for a
6+3 generation, partial generations, parallel fan-out, exclusive creation,
early attachment, supervised startup, TLS delivery, certificate rejection, durable crash replay, concurrent overwrite
integrity, dead mutex owners, slow destination isolation, memory bounds, and
multiple publishers.

~~~bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DPULSEFANOUT_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel 2
ctest --test-dir build-sanitize --output-on-failure
~~~

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the local workflow. Security-sensitive reports should follow [SECURITY.md](SECURITY.md).
