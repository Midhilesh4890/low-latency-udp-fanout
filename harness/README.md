# Fan-out benchmark harness — producer & consumer

Two small, standalone C++17 binaries that form the **fixed ends** of the
benchmark: a **producer** that generates timestamped, sequence-numbered
events, and a **consumer** that receives them and reports delivery latency and
drop rate.

## What's here

```
harness/
  include/
    message.h      fixed-size frame header (seq_id + send_ts_ns) + payload
    shm_ring.h     shared-memory broadcast ring (single producer, N readers)
    shm_segment.h  POSIX shm_open + mmap RAII wrapper
    metrics.h      latency-percentile + drop-rate accumulator
    util.h         now_ns() — nanoseconds since the epoch (std::chrono::system_clock)
  src/
    producer.cpp   generates events, stamps seq_id + send_ts_ns, publishes to ring
    consumer.cpp   reads ring, stamps recv_ts, computes metrics
  test/
    test_harness.cpp  assertion tests for the ring and the metrics accumulator
  Makefile
```

## Message format

Every message is a fixed-size, 64-byte-aligned struct beginning with a common
`Header`, followed by type-specific market-data fields (`message.h`):

```
Header: seq_id (u64), send_ts_ns (u64), type (u16), version (u16), body_len (u32)

Trade      symbol/venue/currencies, ids, price, quantity, aggressor side, flags, ...
Bbo        symbol/venue, best bid/ask price+size, order counts, flags, ...
OrderBook  symbol/venue, update ids, 5 bid levels + 5 ask levels, checksum, ...
```

`seq_id` is a monotonic counter starting at 1; `send_ts_ns` is stamped
immediately before publish. Those two `Header` fields are all the consumer needs
to measure latency (recv_ts − send_ts_ns) and detect drops (gaps in seq_id) — it
never has to interpret the body.

## Basic transport: shared-memory broadcast ring

The provided transport is an in-process / same-host **shared-memory ring** (`shm_ring.h`).
It is deliberately the simplest thing that behaves like a realistic market-data fan-out:

- A slow subscriber cannot stall the producer or other readers.
- A reader that falls too far behind is **lapped**: it detects the overwrite via
  the per-slot sequence number and skips ahead.
- Publication is one release-store of the slot sequence; readers busy-spin on an
  acquire-load.

## Build & test

```bash
make          # builds bin/producer and bin/consumer
make test     # builds and runs the assertion tests
```

## Run the baseline (one host)

```bash
# terminal A — start the producer (creates the shm segment)
./bin/producer --count 500000 --rate 200000 --type mixed

# terminal B — consumer tracks the live edge and reports on idle
./bin/consumer --from-edge
```

Or scripted, one host (pin to isolated cores for a clean tail — see
[Core isolation & pinning](#core-isolation--pinning)):

```bash
taskset -c 2 ./bin/producer --count 500000 --rate 200000 --type mixed &
sleep 0.02
taskset -c 4 ./bin/consumer --from-edge --csv latencies.csv
```

Example output (single host, cores 2/4 isolated):

```
---- delivery metrics ----
received     : 495904
expected     : 495904
dropped      : 0
drop_rate    : 0.0000%
latency (ns) : min=141 mean=235 max=15217
  p01        : 152
  p50        : 218
  p99        : 457
  p99.9      : 808
  p99.99     : 6935
```

`received` is below the producer's `--count 500000` because `--from-edge` makes
the consumer start at the producer's live edge and skip whatever was published
before it attached; `expected` is counted from the first seq_id actually seen, so
`dropped` stays 0 and the skipped prefix is not mistaken for loss.

The `--csv` file (`seq,latency_ns` per row) is what the analysis notebook reads
to build percentile plots.

### Producer flags

| flag | default | meaning |
|------|---------|---------|
| `--shm NAME`     | `/fanout_ring` | shared-memory segment name |
| `--slots N`      | `1024` | ring slot count (power of two) |
| `--count N`      | `1000000` | messages to send (0 = unlimited) |
| `--rate R`       | `0` | target msgs/sec (0 = as fast as possible) |
| `--type T`       | `mixed` | message type: `trade`, `bbo`, `book`, or `mixed` |

### Consumer flags

| flag | default | meaning |
|------|---------|---------|
| `--shm NAME`     | `/fanout_ring` | segment name (must match producer) |
| `--slots N`      | `1024` | slot count (must match producer) |
| `--count N`      | `0` | stop after N messages (0 = run until idle) |
| `--from-edge`    | off | start at the producer's live edge (skip startup catch-up) |
| `--csv FILE`     | — | write per-message `seq,latency_ns` rows |
| `--idle-ms MS`   | `2000` | exit after this long with no new messages |

> Without `--from-edge` a consumer that starts after the producer has a
> head start will "catch up" through a backlog, inflating the early tail. Use
> `--from-edge` for clean steady-state latency, or start the consumer first.

## Measuring latency honestly

Both timestamps come from the same helper, `util::now_ns()`, which reads
`std::chrono::system_clock`. On **one machine** that means `send_ts_ns` and
`recv_ts` share a clock, so the reported latency is a usable one-way delivery
time.

**Across machines the clocks are not synchronized** — `recv_ts − send_ts_ns`
would mostly measure clock offset, not latency. Choosing and justifying a
correct cross-host methodology (RTT/2, PTP, loopback, a shared time source) is
part of the task.

## Core isolation & pinning

Both binaries **busy-spin** — the producer optionally busy-waits for its next
send slot, and the consumer busy-polls the ring. That gives the lowest,
most consistent latency, but only if each spinning thread owns a physical core
with nothing else scheduled on it. Without isolation the tail is dominated by
the scheduler migrating the thread, another task sharing the core, and timer
interrupts — not by the transport you are trying to measure.

Recommended setup on a Linux benchmark host:

1. **Reserve cores from the scheduler at boot.** Add to the kernel command line
   (e.g. in GRUB) and reboot — pick core ids that are real, distinct physical
   cores (avoid a hyperthread sibling pair):

   ```
   isolcpus=2,4 nohz_full=2,4 rcu_nocbs=2,4
   ```

   `isolcpus` keeps the general scheduler off cores 2 and 4; `nohz_full` stops
   the periodic scheduler tick on them; `rcu_nocbs` moves RCU callbacks away.

2. **Pin each binary to an isolated core** with `taskset`:

   ```bash
   taskset -c 2 ./bin/producer --count 500000 --rate 200000 --type mixed &
   sleep 0.02
   taskset -c 4 ./bin/consumer --from-edge --csv latencies.csv
   ```

3. **Optional, sharpens the tail further:**
   - Move IRQs off the isolated cores (`/proc/irq/*/smp_affinity`).
   - Disable frequency scaling / set the `performance` cpufreq governor so the
     core does not clock down between spins.
   - Keep producer and consumer on the **same NUMA node** as the shared-memory
     segment (`numactl --cpunodebind=0 --membind=0 ...`).

Record which cores you isolated and how you pinned them alongside your results.
