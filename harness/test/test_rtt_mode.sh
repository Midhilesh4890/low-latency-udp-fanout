#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp="$(mktemp -d)"
run_id="rtt_test_$$"
in_shm="/${run_id}_in"
out_shm="/${run_id}_out"
unused_shm="/${run_id}_unused"
port="$((20000 + $$ % 20000))"
pids=()

cleanup() {
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -f "/dev/shm/${in_shm#/}" "/dev/shm/${out_shm#/}" "/dev/shm/${unused_shm#/}"
  rm -rf -- "$tmp"
}
trap cleanup EXIT

"$repo_root/harness/bin/receiver" --echo --out-shm "$unused_shm" --slots 1024 --port "$port" --count 1100 --idle-ms 5000 >"$tmp/receiver.log" 2>&1 &
pids+=("$!")
"$repo_root/harness/bin/producer" --shm "$in_shm" --slots 1024 --count 1100 --rate 10000 --start-delay-ms 500 >"$tmp/producer.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 1000); do
  [[ -e "/dev/shm/${in_shm#/}" ]] && break
  sleep 0.001
done
"$repo_root/harness/bin/sender" --in-shm "$in_shm" --echo-out-shm "$out_shm" --slots 1024 --count 1100 --dst "127.0.0.1:$port" --batch-size 32 --batch-timeout-us 50 >"$tmp/sender.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 1000); do
  [[ -e "/dev/shm/${out_shm#/}" ]] && break
  sleep 0.001
done
"$repo_root/harness/bin/consumer" --shm "$out_shm" --slots 1024 --count 1000 --skip 100 --idle-ms 5000 >"$tmp/consumer.log" 2>&1 &
pids+=("$!")

for pid in "${pids[@]}"; do
  wait "$pid"
done

grep -q 'sent=1100 packets=1100 echoed=1100 lapped=0' "$tmp/sender.log"
grep -q 'received=1100 published=0 echoed=1100' "$tmp/receiver.log"
grep -q 'skipped 100 lapped 0 times' "$tmp/consumer.log"
grep -q 'received     : 1000' "$tmp/consumer.log"
grep -q 'dropped      : 0' "$tmp/consumer.log"
printf '%s\n' "rtt mode test passed"
