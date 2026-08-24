#!/usr/bin/env bash
set -euo pipefail

tx_host=""
rx_host=""
rx_private=""
ssh_user="ubuntu"
ssh_key="${SSH_KEY:-}"
remote_repo="${REMOTE_REPO:-~/task}"
outdir=""
rate="100000"
count="3000000"
warmup="100000"
slots="65536"
port="9300"
cpu_producer="1"
cpu_sender="2"
cpu_consumer="3"
cpu_echo="1"
tx_isolated="1,2,3"
rx_isolated="1,2"
sndbuf="67108864"
rcvbuf="67108864"
batch_size="32"
batch_timeout_us="50"
latency_output="none"
start_delay_ms="1000"
source_revision="HEAD"
no_build="false"

need() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --tx-host) need "$@"; tx_host="$2"; shift 2 ;;
    --rx-host) need "$@"; rx_host="$2"; shift 2 ;;
    --rx-private) need "$@"; rx_private="$2"; shift 2 ;;
    --ssh-user) need "$@"; ssh_user="$2"; shift 2 ;;
    --ssh-key) need "$@"; ssh_key="$2"; shift 2 ;;
    --remote-repo) need "$@"; remote_repo="$2"; shift 2 ;;
    --outdir) need "$@"; outdir="$2"; shift 2 ;;
    --rate) need "$@"; rate="$2"; shift 2 ;;
    --count) need "$@"; count="$2"; shift 2 ;;
    --warmup) need "$@"; warmup="$2"; shift 2 ;;
    --slots) need "$@"; slots="$2"; shift 2 ;;
    --port) need "$@"; port="$2"; shift 2 ;;
    --cpu-producer) need "$@"; cpu_producer="$2"; shift 2 ;;
    --cpu-sender) need "$@"; cpu_sender="$2"; shift 2 ;;
    --cpu-consumer) need "$@"; cpu_consumer="$2"; shift 2 ;;
    --cpu-echo) need "$@"; cpu_echo="$2"; shift 2 ;;
    --tx-isolated) need "$@"; tx_isolated="$2"; shift 2 ;;
    --rx-isolated) need "$@"; rx_isolated="$2"; shift 2 ;;
    --sndbuf) need "$@"; sndbuf="$2"; shift 2 ;;
    --rcvbuf) need "$@"; rcvbuf="$2"; shift 2 ;;
    --batch-size) need "$@"; batch_size="$2"; shift 2 ;;
    --batch-timeout-us) need "$@"; batch_timeout_us="$2"; shift 2 ;;
    --latency-output) need "$@"; latency_output="$2"; shift 2 ;;
    --start-delay-ms) need "$@"; start_delay_ms="$2"; shift 2 ;;
    --source-revision) need "$@"; source_revision="$2"; shift 2 ;;
    --no-build) no_build="true"; shift ;;
    *) printf '%s\n' "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$tx_host" || -z "$rx_host" || -z "$rx_private" || -z "$outdir" ]]; then
  printf '%s\n' "--tx-host, --rx-host, --rx-private, and --outdir are required" >&2
  exit 2
fi
case "$latency_output" in
  none|disk) ;;
  *) printf '%s\n' "--latency-output must be none or disk" >&2; exit 2 ;;
esac

outdir="$(mkdir -p "$outdir" && cd "$outdir" && pwd)"
run_id="rtt_$(date -u +%Y%m%dT%H%M%SZ)_$$"
remote_base="/tmp/$run_id"
total_count="$((count + warmup))"
stream_seconds="$(((total_count + rate - 1) / rate))"
completion_seconds="$((stream_seconds + 45))"
in_shm="/${run_id}_in"
out_shm="/${run_id}_out"
unused_shm="/${run_id}_unused"
start_ns="$(date +%s%N)"
ssh_opts=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/tmp/session2_known_hosts -o ServerAliveInterval=10)
if [[ -n "$ssh_key" ]]; then
  ssh_opts+=(-i "$ssh_key")
fi

remote() {
  local host="$1"
  shift
  ssh "${ssh_opts[@]}" "$ssh_user@$host" "$@"
}

copy_from() {
  local host="$1"
  local source="$2"
  local target="$3"
  local ssh_command="ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/tmp/session2_known_hosts"
  if [[ -n "$ssh_key" ]]; then
    ssh_command+=" -i $ssh_key"
  fi
  rsync -az -e "$ssh_command" "$ssh_user@$host:$source" "$target"
}

cleanup() {
  remote "$tx_host" "pkill -f '$run_id' 2>/dev/null || true" >/dev/null 2>&1 || true
  remote "$rx_host" "pkill -f '$run_id' 2>/dev/null || true" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

wait_for() {
  local host="$1"
  local seconds="$2"
  local label="$3"
  local command="$4"
  local end="$((SECONDS + seconds))"
  while (( SECONDS < end )); do
    if remote "$host" "$command" >/dev/null 2>&1; then
      printf '%s\n' "confirmed: $label"
      return 0
    fi
    sleep 0.05
  done
  printf '%s\n' "confirmation failed: $label" >&2
  return 1
}

wait_exit() {
  local host="$1"
  local file="$2"
  local label="$3"
  local end="$((SECONDS + 180))"
  while (( SECONDS < end )); do
    if remote "$host" "[[ -s '$file' ]] && ! kill -0 \$(cat '$file') 2>/dev/null" >/dev/null 2>&1; then
      printf '%s\n' "exited: $label"
      return 0
    fi
    sleep 0.2
  done
  printf '%s\n' "process timeout: $label" >&2
  return 1
}

if [[ "$no_build" == "false" ]]; then
  remote "$tx_host" "cd $remote_repo && make -C harness clean all test"
  remote "$rx_host" "cd $remote_repo && make -C harness clean all test"
fi

remote "$tx_host" "cd $remote_repo && bash benchmark/preflight_isolation.sh --cores '$cpu_producer,$cpu_sender,$cpu_consumer' --isolated-cores '$tx_isolated' --label rtt-tx"
remote "$rx_host" "cd $remote_repo && bash benchmark/preflight_isolation.sh --cores '$cpu_echo' --isolated-cores '$rx_isolated' --label rtt-rx"
remote "$tx_host" "mkdir -p '$remote_base/tx' && rm -f '/dev/shm/${in_shm#/}' '/dev/shm/${out_shm#/}'"
remote "$rx_host" "mkdir -p '$remote_base/rx' && rm -f '/dev/shm/${unused_shm#/}'"

remote "$rx_host" "cd $remote_repo && setsid -f taskset -c '$cpu_echo' harness/bin/receiver --echo --out-shm '$unused_shm' --slots '$slots' --port '$port' --count '$total_count' --idle-ms 30000 --rcvbuf '$rcvbuf' --batch-size '$batch_size' </dev/null >'$remote_base/rx/receiver.log' 2>&1"
wait_for "$rx_host" 10 "echo receiver" "ss -H -lun | grep -q ':$port'"

csv_arg=""
if [[ "$latency_output" == "disk" ]]; then
  csv_arg="--csv '$remote_base/tx/latency.bin'"
fi
remote "$tx_host" "cd $remote_repo && setsid -f taskset -c '$cpu_producer' harness/bin/producer --shm '$in_shm' --slots '$slots' --count '$total_count' --rate '$rate' --type mixed --start-delay-ms '$start_delay_ms' </dev/null >'$remote_base/tx/producer.log' 2>&1 && for i in \$(seq 1 10000); do [[ -e '/dev/shm/${in_shm#/}' ]] && break; sleep 0.001; done && [[ -e '/dev/shm/${in_shm#/}' ]] && setsid -f taskset -c '$cpu_sender' harness/bin/sender --in-shm '$in_shm' --echo-out-shm '$out_shm' --slots '$slots' --count '$total_count' --dst '$rx_private:$port' --sndbuf '$sndbuf' --rcvbuf '$rcvbuf' --batch-size '$batch_size' --batch-timeout-us '$batch_timeout_us' --echo-timeout-ms 2000 </dev/null >'$remote_base/tx/sender.log' 2>&1 && for i in \$(seq 1 10000); do [[ -e '/dev/shm/${out_shm#/}' ]] && break; sleep 0.001; done && [[ -e '/dev/shm/${out_shm#/}' ]] && setsid -f taskset -c '$cpu_consumer' harness/bin/consumer --shm '$out_shm' --slots '$slots' --count '$count' --skip '$warmup' --idle-ms 30000 $csv_arg </dev/null >'$remote_base/tx/consumer.log' 2>&1"
wait_for "$tx_host" 10 "consumer" "pgrep -f 'harness/bin/consumer.*$out_shm' >/dev/null"

wait_for "$tx_host" "$completion_seconds" "sender counters" "grep -q 'echoed=$total_count' '$remote_base/tx/sender.log'"
wait_for "$tx_host" 30 "consumer counters" "grep -q 'received     : $count' '$remote_base/tx/consumer.log'"
wait_for "$rx_host" 30 "receiver counters" "grep -q 'echoed=$total_count' '$remote_base/rx/receiver.log'"

if [[ "$latency_output" == "disk" ]]; then
  remote "$tx_host" "cd $remote_repo && python3 benchmark/bin_to_csv.py '$remote_base/tx/latency.bin' '$remote_base/tx/latency.csv'"
fi
mkdir -p "$outdir/tx" "$outdir/rx"
copy_from "$tx_host" "$remote_base/tx/" "$outdir/tx/"
copy_from "$rx_host" "$remote_base/rx/" "$outdir/rx/"

grep -q "received     : $count" "$outdir/tx/consumer.log"
grep -q "dropped      : 0" "$outdir/tx/consumer.log"
grep -q "echoed=$total_count" "$outdir/tx/sender.log"
grep -q "echoed=$total_count" "$outdir/rx/receiver.log"

end_ns="$(date +%s%N)"
python3 - "$outdir/run.json" "$rate" "$count" "$warmup" "$slots" "$port" "$batch_size" "$batch_timeout_us" "$source_revision" "$latency_output" "$tx_host" "$rx_host" "$rx_private" "$((end_ns - start_ns))" <<'PY'
import json
import sys

path, rate, count, warmup, slots, port, batch_size, batch_timeout_us, revision, latency_output, tx_host, rx_host, rx_private, duration_ns = sys.argv[1:]
data = {
    "clock_sync": {"method": "same_clock_rtt_half", "scale": 0.5},
    "hostname": tx_host,
    "parameters": {
        "batch_size": int(batch_size),
        "batch_timeout_us": int(batch_timeout_us),
        "count": int(count),
        "latency_output": latency_output,
        "port": int(port),
        "rate": int(rate),
        "slots": int(slots),
        "warmup": int(warmup),
    },
    "rx_host": rx_host,
    "rx_private": rx_private,
    "sample_count": int(count),
    "source_revision": revision,
    "topology": "producer+sender+consumer on tx; echo receiver on rx; reported one-way estimate is RTT/2",
    "tx_host": tx_host,
    "wall_clock_duration_s": int(duration_ns) / 1000000000.0,
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

trap - EXIT INT TERM
cleanup
