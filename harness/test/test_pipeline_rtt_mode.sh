#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp="$(mktemp -d)"
run_id="pipeline_rtt_test_$$"
in_shm="/${run_id}_in"
forward_shm="/${run_id}_forward"
relay_shm="/${run_id}_relay"
return_shm="/${run_id}_return"
forward_port="$((20000 + $$ % 15000))"
return_port="$((forward_port + 1))"
pids=()

cleanup() {
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -f "/dev/shm/${in_shm#/}" "/dev/shm/${forward_shm#/}" "/dev/shm/${relay_shm#/}" "/dev/shm/${return_shm#/}"
  rm -rf -- "$tmp"
}
trap cleanup EXIT

"$repo_root/harness/bin/receiver" --out-shm "$return_shm" --slots 2048 --port "$return_port" --count 1100 --idle-ms 5000 >"$tmp/return_receiver.log" 2>&1 &
pids+=("$!")
"$repo_root/harness/bin/receiver" --out-shm "$forward_shm" --slots 2048 --port "$forward_port" --count 1100 --idle-ms 5000 >"$tmp/forward_receiver.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 1000); do
  [[ -e "/dev/shm/${return_shm#/}" && -e "/dev/shm/${forward_shm#/}" ]] && break
  sleep 0.001
done
"$repo_root/harness/bin/consumer" --shm "$return_shm" --slots 2048 --count 1000 --skip 100 --idle-ms 5000 >"$tmp/consumer.log" 2>&1 &
pids+=("$!")
"$repo_root/harness/bin/relay" --in-shm "$forward_shm" --out-shm "$relay_shm" --slots 2048 --count 1100 --idle-ms 5000 >"$tmp/relay.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 1000); do
  [[ -e "/dev/shm/${relay_shm#/}" ]] && break
  sleep 0.001
done
"$repo_root/harness/bin/sender" --in-shm "$relay_shm" --slots 2048 --count 1100 --dst "127.0.0.1:$return_port" --batch-size 32 --batch-timeout-us 5 >"$tmp/return_sender.log" 2>&1 &
pids+=("$!")
"$repo_root/harness/bin/producer" --shm "$in_shm" --slots 2048 --count 1100 --rate 10000 --start-delay-ms 500 >"$tmp/producer.log" 2>&1 &
pids+=("$!")
for _ in $(seq 1 1000); do
  [[ -e "/dev/shm/${in_shm#/}" ]] && break
  sleep 0.001
done
"$repo_root/harness/bin/sender" --in-shm "$in_shm" --slots 2048 --count 1100 --dst "127.0.0.1:$forward_port" --batch-size 32 --batch-timeout-us 5 >"$tmp/forward_sender.log" 2>&1 &
pids+=("$!")

for pid in "${pids[@]}"; do
  wait "$pid"
done

grep -q 'sent=1100 packets=1100 echoed=0 lapped=0' "$tmp/forward_sender.log"
grep -q 'received=1100 published=1100 echoed=0 rejected=0 accepted=1100' "$tmp/forward_receiver.log"
grep -q 'forwarded=1100 lapped=0' "$tmp/relay.log"
grep -q 'sent=1100 packets=1100 echoed=0 lapped=0' "$tmp/return_sender.log"
grep -q 'received=1100 published=1100 echoed=0 rejected=0 accepted=1100' "$tmp/return_receiver.log"
grep -q 'skipped 100 lapped 0 times' "$tmp/consumer.log"
grep -q 'received     : 1000' "$tmp/consumer.log"
grep -q 'dropped      : 0' "$tmp/consumer.log"
printf '%s\n' "pipeline RTT mode test passed"
