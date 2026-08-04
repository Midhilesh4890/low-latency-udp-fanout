# Baseline: Shared-Memory Ring

To understand what exactly is going on, I have ran below commands and checked it

In the sametime, got curious to see if we have more slots, will the consumer lapped or not. When I checked with 65536 slots, there is no lapping and all messages are recieved by consumer 

But yeah this may not always be true as we may not have the option of using more slots. It only shows that, in this local baseline test, the smaller ring was one cause of message drops under load.

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
