#!/usr/bin/env bash
set -uo pipefail

bin_dir="${1:-./harness/bin}"
count="${PULSEFANOUT_SMOKE_COUNT:-5000}"
slots=8192
run_id="pulsefanout_$$"
input_shm="/${run_id}_in"
output_shm="/${run_id}_out"
port="$((20000 + $$ % 20000))"
log_dir="$(mktemp -d)"
pids=()

cleanup() {
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -f "/dev/shm/${input_shm#/}" "/dev/shm/${output_shm#/}"
  rm -rf "$log_dir"
}
trap cleanup EXIT INT TERM

fail() {
  echo "smoke test failed: $*" >&2
  for log in "$log_dir"/*.log; do
    [ -f "$log" ] || continue
    echo "----- ${log##*/} -----" >&2
    cat "$log" >&2
  done
  exit 1
}

wait_for_shm() {
  local name="$1"
  local path="/dev/shm/${name#/}"
  for _ in $(seq 1 100); do
    [ -e "$path" ] && return 0
    sleep 0.02
  done
  return 1
}

for executable in producer sender receiver consumer; do
  [ -x "$bin_dir/$executable" ] || fail "missing executable $bin_dir/$executable"
done

timeout 15s "$bin_dir/receiver" --allow-insecure-udp --out-shm "$output_shm" --slots "$slots" --bind 127.0.0.1 --port "$port" --count "$count" --idle-ms 5000 >"$log_dir/receiver.log" 2>&1 &
receiver_pid=$!
pids+=("$receiver_pid")
wait_for_shm "$output_shm" || fail "receiver did not create its shared-memory ring"

timeout 15s "$bin_dir/consumer" --shm "$output_shm" --slots "$slots" --count "$count" --idle-ms 5000 >"$log_dir/consumer.log" 2>&1 &
consumer_pid=$!
pids+=("$consumer_pid")

timeout 15s "$bin_dir/producer" --shm "$input_shm" --slots "$slots" --count "$count" --rate 25000 --start-delay-ms 500 --type mixed >"$log_dir/producer.log" 2>&1 &
producer_pid=$!
pids+=("$producer_pid")
wait_for_shm "$input_shm" || fail "producer did not create its shared-memory ring"

timeout 15s "$bin_dir/sender" --allow-insecure-udp --in-shm "$input_shm" --slots "$slots" --dst "127.0.0.1:$port" --count "$count" --idle-ms 5000 --batch-size 32 --batch-timeout-us 50 >"$log_dir/sender.log" 2>&1 &
sender_pid=$!
pids+=("$sender_pid")

failed=0
for pid in "$producer_pid" "$sender_pid" "$receiver_pid" "$consumer_pid"; do
  wait "$pid" || failed=1
done
[ "$failed" -eq 0 ] || fail "one or more transport processes exited unsuccessfully"

grep -q "producer: sent $count messages" "$log_dir/producer.log" ||
  fail "producer count did not match"
grep -q "sender: sent=$count " "$log_dir/sender.log" ||
  fail "sender count did not match"
grep -q "published=$count .*rejected=0" "$log_dir/receiver.log" ||
  fail "receiver count or validation did not match"
grep -Eq "received[[:space:]]*:[[:space:]]*$count" "$log_dir/consumer.log" ||
  fail "consumer count did not match"
grep -Eq "dropped[[:space:]]*:[[:space:]]*0" "$log_dir/consumer.log" ||
  fail "consumer reported packet loss"

echo "PulseFanout smoke test passed: $count mixed messages over localhost UDP"
