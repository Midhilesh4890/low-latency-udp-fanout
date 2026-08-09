# Baseline: Shared-Memory Ring

To understand what exactly is going on, I have ran below commands and checked it

In the sametime, got curious to see if we have more slots, will the consumer lapped or not. When I checked with 65536 slots, there is no lapping and all messages are recieved by consumer 

But yeah this may not always be true as we may not have the option of using more slots. It only shows that, in this local baseline test, the smaller ring was one cause of message drops under load.

## Current Network Transport

I added two binaries for the middle transport layer:

- `sender`: reads frames from the producer shared-memory ring and sends them over UDP.
- `receiver`: receives UDP frames and publishes them into another shared-memory ring for the consumer.

Build:

```bash
make -C harness
```

Single-machine smoke test flow, in four terminals:

```bash
./harness/bin/receiver --out-shm /fanout_out --slots 65536 --bind 127.0.0.1 --port 9000 --count 200000 --rcvbuf 4194304
```

```bash
./harness/bin/consumer --shm /fanout_out --slots 65536 --from-edge --count 200000 --csv data/latency_udp.csv
```

```bash
./harness/bin/producer --shm /fanout_in --slots 65536 --count 200000 --rate 250000 --type mixed
```

```bash
./harness/bin/sender --in-shm /fanout_in --slots 65536 --host 127.0.0.1 --port 9000 --count 200000 --sndbuf 4194304 --repeat 1
```

For a two-machine run, start `receiver` and `consumer` on the receiving host, start `producer` and `sender` on the sending host, and pass the receiving host IP to `sender --host`.

For fan-out, run one `receiver` and one `consumer` per receiving host, then pass multiple destinations to `sender`:

```bash
./harness/bin/sender --in-shm /fanout_in --slots 65536 --dst 10.0.1.11:9000 --dst 10.0.1.12:9000 --dst 10.0.1.13:9000 --count 200000 --sndbuf 4194304 --repeat 1
```

The UDP sender uses a connected socket and `send()` on the hot path. The `--sndbuf` and `--rcvbuf` options tune kernel socket buffers for burst tolerance.

For packet-loss tolerance, `sender --repeat N` sends each frame N times to each destination. The receiver deduplicates by `seq_id` and publishes only the first copy into the consumer shared-memory ring. With independent packet loss, `--repeat 2` changes the probability of losing a message from `p` to approximately `p^2`, at the cost of doubling network bandwidth. This is useful for the 0.01-1% packet-loss regime described in the task when the network has enough spare capacity.

## UDP Smoke Test on WSL2 Loopback

This was a functional single-machine check on Ubuntu 24.04 under WSL2, using UDP over `127.0.0.1`. It verifies that the full path works:

```text
producer -> sender -> UDP -> receiver -> consumer
```

Run with finite producer count and sender starting from index 0:

```text
producer: sent 200000 messages
sender: sent=200000 lapped=0
receiver: received=200000
consumer: lapped 0 times
received     : 200000
expected     : 200000
dropped      : 0
drop_rate    : 0.0000%
latency (ns) : min=31034104 mean=44390136 max=66914294
p50          : 43596915
p99          : 66404127
p99.9        : 66849829
p99.99       : 66903253
```

Run with an unlimited producer and `sender --from-edge`:

```text
sender: sent=200000 lapped=0
receiver: received=200000
consumer: lapped 0 times
received     : 200000
expected     : 200000
dropped      : 0
drop_rate    : 0.0000%
latency (ns) : min=1818 mean=105049614 max=207956837
p50          : 84200415
p99          : 206642518
p99.9        : 207894854
p99.99       : 207935323
```

Both runs delivered all 200000 messages with no ring lapping and no detected drops. The latency numbers should be treated only as WSL2 loopback smoke-test results, not as final transport performance. The processes were running on one machine without isolated cores, CPU pinning, IRQ isolation, or a real two-host network setup, so scheduler contention and local buffering dominate these measurements.

## 1024 Slots

```bash
./harness/bin/producer --shm /fanout_demo --slots 1024 --count 0 --rate 250000 --type mixed
./harness/bin/consumer --shm /fanout_demo --slots 1024 --from-edge --count 200000 --csv data/latency.csv
```

```text
consumer: lapped 9 times
---- delivery metrics ----
received     : 200000
expected     : 208171
dropped      : 8171
drop_rate    : 3.9251%
latency (ns) : min=50 mean=116418 max=8107263
  p01        : 70
  p50        : 216
  p99        : 2594780
  p99.9      : 3946376
  p99.99     : 8030320
```

At 1024 slots, messages were dropped under this load.

## 65536 Slots

```bash
./harness/bin/producer --shm /fanout_demo --slots 65536 --count 0 --rate 250000 --type mixed
./harness/bin/consumer --shm /fanout_demo --slots 65536 --from-edge --count 200000 --csv data/latency_slots65536.csv
```

```text
consumer: lapped 0 times
---- delivery metrics ----
received     : 200000
expected     : 200000
dropped      : 0
drop_rate    : 0.0000%
latency (ns) : min=53 mean=18309 max=4256476
  p01        : 74
  p50        : 205
  p99        : 335049
  p99.9      : 3529985
  p99.99     : 4178030
```

At 65536 slots, this run had no drops.
