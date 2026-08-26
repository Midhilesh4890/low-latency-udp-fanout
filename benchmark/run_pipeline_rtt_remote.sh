#!/usr/bin/env bash
set -euo pipefail

tx_host=""
rx_host=""
tx_private=""
rx_private=""
ssh_user="ubuntu"
ssh_key="${SSH_KEY:-}"
known_hosts="/tmp/transport_known_hosts"
harness_repo="/home/ubuntu/task"
transport_repo="/home/ubuntu/task"
transport_mode="current"
source_revision="HEAD"
harness_revision="HEAD"
outdir=""
rate="100000"
count="3000000"
warmup="100000"
slots="65536"
message_type="mixed"
forward_port="9500"
return_port="9501"
cpu_producer="1"
cpu_forward_sender="2"
cpu_return_receiver="3"
cpu_consumer="4"
cpu_forward_receiver="1"
cpu_relay="2"
cpu_return_sender="3"
tx_isolated="1,2,3,4"
rx_isolated="1,2,3"
sndbuf="67108864"
rcvbuf="67108864"
batch_size="32"
batch_timeout_us="5"
fec_k="0"
fec_timeout_us="200"
test_drop_pct="0"
latency_output="none"
start_delay_ms="1000"
no_build="false"
allow_loss="false"

need() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

quote_one() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\''/g")"
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --tx-host) need "$@"; tx_host="$2"; shift 2 ;;
    --rx-host) need "$@"; rx_host="$2"; shift 2 ;;
    --tx-private) need "$@"; tx_private="$2"; shift 2 ;;
    --rx-private) need "$@"; rx_private="$2"; shift 2 ;;
    --ssh-user) need "$@"; ssh_user="$2"; shift 2 ;;
    --ssh-key) need "$@"; ssh_key="$2"; shift 2 ;;
    --known-hosts) need "$@"; known_hosts="$2"; shift 2 ;;
    --harness-repo) need "$@"; harness_repo="$2"; shift 2 ;;
    --transport-repo) need "$@"; transport_repo="$2"; shift 2 ;;
    --transport-mode) need "$@"; transport_mode="$2"; shift 2 ;;
    --source-revision) need "$@"; source_revision="$2"; shift 2 ;;
    --harness-revision) need "$@"; harness_revision="$2"; shift 2 ;;
    --outdir) need "$@"; outdir="$2"; shift 2 ;;
    --rate) need "$@"; rate="$2"; shift 2 ;;
    --count) need "$@"; count="$2"; shift 2 ;;
    --warmup) need "$@"; warmup="$2"; shift 2 ;;
    --slots) need "$@"; slots="$2"; shift 2 ;;
    --type) need "$@"; message_type="$2"; shift 2 ;;
    --forward-port) need "$@"; forward_port="$2"; shift 2 ;;
    --return-port) need "$@"; return_port="$2"; shift 2 ;;
    --cpu-producer) need "$@"; cpu_producer="$2"; shift 2 ;;
    --cpu-forward-sender) need "$@"; cpu_forward_sender="$2"; shift 2 ;;
    --cpu-return-receiver) need "$@"; cpu_return_receiver="$2"; shift 2 ;;
    --cpu-consumer) need "$@"; cpu_consumer="$2"; shift 2 ;;
    --cpu-forward-receiver) need "$@"; cpu_forward_receiver="$2"; shift 2 ;;
    --cpu-relay) need "$@"; cpu_relay="$2"; shift 2 ;;
    --cpu-return-sender) need "$@"; cpu_return_sender="$2"; shift 2 ;;
    --tx-isolated) need "$@"; tx_isolated="$2"; shift 2 ;;
    --rx-isolated) need "$@"; rx_isolated="$2"; shift 2 ;;
    --sndbuf) need "$@"; sndbuf="$2"; shift 2 ;;
    --rcvbuf) need "$@"; rcvbuf="$2"; shift 2 ;;
    --batch-size) need "$@"; batch_size="$2"; shift 2 ;;
    --batch-timeout-us) need "$@"; batch_timeout_us="$2"; shift 2 ;;
    --fec-k) need "$@"; fec_k="$2"; shift 2 ;;
    --fec-timeout-us) need "$@"; fec_timeout_us="$2"; shift 2 ;;
    --test-drop-pct) need "$@"; test_drop_pct="$2"; shift 2 ;;
    --latency-output) need "$@"; latency_output="$2"; shift 2 ;;
    --start-delay-ms) need "$@"; start_delay_ms="$2"; shift 2 ;;
    --no-build) no_build="true"; shift ;;
    --allow-loss) allow_loss="true"; shift ;;
    *) printf '%s\n' "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$tx_host" || -z "$rx_host" || -z "$tx_private" || -z "$rx_private" || -z "$outdir" ]]; then
  printf '%s\n' "--tx-host, --rx-host, --tx-private, --rx-private, and --outdir are required" >&2
  exit 2
fi
case "$transport_mode" in
  current|baseline) ;;
  *) printf '%s\n' "--transport-mode must be current or baseline" >&2; exit 2 ;;
esac
case "$latency_output" in
  none|disk) ;;
  *) printf '%s\n' "--latency-output must be none or disk" >&2; exit 2 ;;
esac
if [[ -e "$outdir" ]]; then
  printf '%s\n' "result path already exists: $outdir" >&2
  exit 2
fi
mkdir -p "$(dirname "$outdir")"
outdir="$(cd "$(dirname "$outdir")" && pwd)/$(basename "$outdir")"
mkdir "$outdir"

run_id="pipeline_rtt_$(date -u +%Y%m%dT%H%M%SZ)_$$"
remote_base="/tmp/$run_id"
total_count="$((count + warmup))"
stream_seconds="$(((total_count + rate - 1) / rate))"
completion_seconds="$((stream_seconds + 75))"
start_ns="$(date +%s%N)"
ssh_opts=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$known_hosts" -o ServerAliveInterval=10)
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
  local ssh_command="ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$known_hosts"
  if [[ -n "$ssh_key" ]]; then
    ssh_command+=" -i $ssh_key"
  fi
  rsync -az -e "$ssh_command" "$ssh_user@$host:$source" "$target"
}

cleanup() {
  remote "$tx_host" "pkill -f '$run_id' 2>/dev/null || true; rm -f /dev/shm/*'$run_id'*; rm -rf -- '$remote_base'" >/dev/null 2>&1 || true
  remote "$rx_host" "pkill -f '$run_id' 2>/dev/null || true; rm -f /dev/shm/*'$run_id'*; rm -rf -- '$remote_base'" >/dev/null 2>&1 || true
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

if [[ "$no_build" == "false" ]]; then
  remote "$tx_host" "cd '$harness_repo' && make -C harness clean all test"
  remote "$rx_host" "cd '$harness_repo' && make -C harness clean all test"
  if [[ "$transport_repo" != "$harness_repo" ]]; then
    remote "$tx_host" "cd '$transport_repo' && make -C harness clean all test"
    remote "$rx_host" "cd '$transport_repo' && make -C harness clean all test"
  fi
fi

remote "$tx_host" "cd '$harness_repo' && bash benchmark/preflight_isolation.sh --cores '$cpu_producer,$cpu_forward_sender,$cpu_return_receiver,$cpu_consumer' --isolated-cores '$tx_isolated' --label pipeline-rtt-tx"
remote "$rx_host" "cd '$harness_repo' && bash benchmark/preflight_isolation.sh --cores '$cpu_forward_receiver,$cpu_relay,$cpu_return_sender' --isolated-cores '$rx_isolated' --label pipeline-rtt-rx"
remote "$tx_host" "mkdir -p '$remote_base/tx'"
remote "$rx_host" "mkdir -p '$remote_base/rx'"

common_env=(
  "HARNESS_REPO=$harness_repo"
  "TRANSPORT_REPO=$transport_repo"
  "TRANSPORT_MODE=$transport_mode"
  "RUN_ID=$run_id"
  "TOTAL_COUNT=$total_count"
  "MEASURED_COUNT=$count"
  "WARMUP=$warmup"
  "SLOTS=$slots"
  "RATE=$rate"
  "MESSAGE_TYPE=$message_type"
  "FORWARD_PORT=$forward_port"
  "RETURN_PORT=$return_port"
  "SNDBUF=$sndbuf"
  "RCVBUF=$rcvbuf"
  "BATCH_SIZE=$batch_size"
  "BATCH_TIMEOUT_US=$batch_timeout_us"
  "FEC_K=$fec_k"
  "FEC_TIMEOUT_US=$fec_timeout_us"
  "TEST_DROP_PCT=$test_drop_pct"
  "START_DELAY_MS=$start_delay_ms"
  "LATENCY_OUTPUT=$latency_output"
)
common_text=""
for item in "${common_env[@]}"; do
  common_text+=" $(quote_one "$item")"
done

rx_text="$common_text"
for item in "ROLE=rx" "RUN_DIR=$remote_base/rx" "PEER_PRIVATE=$tx_private" "CPU_FORWARD_RECEIVER=$cpu_forward_receiver" "CPU_RELAY=$cpu_relay" "CPU_RETURN_SENDER=$cpu_return_sender"; do
  rx_text+=" $(quote_one "$item")"
done
remote "$rx_host" "cd '$harness_repo' && setsid -f env $rx_text bash benchmark/run_pipeline_host.sh </dev/null >'$remote_base/rx/host.log' 2>&1"
wait_for "$rx_host" 10 "forward receiver" "grep -q '^receiver:' '$remote_base/rx/forward_receiver.log'"

tx_text="$common_text"
for item in "ROLE=tx" "RUN_DIR=$remote_base/tx" "PEER_PRIVATE=$rx_private" "CPU_PRODUCER=$cpu_producer" "CPU_FORWARD_SENDER=$cpu_forward_sender" "CPU_RETURN_RECEIVER=$cpu_return_receiver" "CPU_CONSUMER=$cpu_consumer"; do
  tx_text+=" $(quote_one "$item")"
done
remote "$tx_host" "cd '$harness_repo' && setsid -f env $tx_text bash benchmark/run_pipeline_host.sh </dev/null >'$remote_base/tx/host.log' 2>&1"
wait_for "$tx_host" 10 "return receiver" "grep -q '^receiver:' '$remote_base/tx/return_receiver.log'"

status=0
wait_for "$tx_host" "$completion_seconds" "TX pipeline completion" "[[ -s '$remote_base/tx/done' ]]" || status=1
wait_for "$rx_host" 30 "RX pipeline completion" "[[ -s '$remote_base/rx/done' ]]" || status=1
if [[ "$latency_output" == "disk" && "$status" == "0" ]]; then
  remote "$tx_host" "cd '$harness_repo' && python3 benchmark/bin_to_csv.py '$remote_base/tx/latency.bin' '$remote_base/tx/latency.csv'"
fi
mkdir -p "$outdir/tx" "$outdir/rx"
copy_from "$tx_host" "$remote_base/tx/" "$outdir/tx/" || true
copy_from "$rx_host" "$remote_base/rx/" "$outdir/rx/" || true
if (( status != 0 )); then
  exit "$status"
fi

if [[ "$allow_loss" == "false" ]]; then
  grep -q "received     : $count" "$outdir/tx/consumer.log"
  grep -q "dropped      : 0" "$outdir/tx/consumer.log"
  grep -q "sent=$total_count .*lapped=0" "$outdir/tx/forward_sender.log"
  grep -q "published=$total_count" "$outdir/rx/forward_receiver.log"
  grep -q "forwarded=$total_count lapped=0" "$outdir/rx/relay.log"
  grep -q "sent=$total_count .*lapped=0" "$outdir/rx/return_sender.log"
  grep -q "published=$total_count" "$outdir/tx/return_receiver.log"
else
  grep -q "lapped=0" "$outdir/tx/forward_sender.log"
  grep -q "lapped=0" "$outdir/rx/relay.log"
  grep -q "lapped=0" "$outdir/rx/return_sender.log"
  grep -q "rejected=0" "$outdir/rx/forward_receiver.log"
  grep -q "rejected=0" "$outdir/tx/return_receiver.log"
fi

end_ns="$(date +%s%N)"
python3 - "$outdir/run.json" "$rate" "$count" "$warmup" "$slots" "$forward_port" "$return_port" "$batch_size" "$batch_timeout_us" "$fec_k" "$fec_timeout_us" "$test_drop_pct" "$allow_loss" "$source_revision" "$harness_revision" "$transport_mode" "$latency_output" "$tx_host" "$rx_host" "$tx_private" "$rx_private" "$((end_ns - start_ns))" <<'PY'
import json
import sys

path, rate, count, warmup, slots, forward_port, return_port, batch_size, batch_timeout_us, fec_k, fec_timeout_us, test_drop_pct, allow_loss, revision, harness_revision, transport_mode, latency_output, tx_host, rx_host, tx_private, rx_private, duration_ns = sys.argv[1:]
data = {
    "clock_sync": {"method": "same_clock_symmetric_pipeline_rtt_half", "scale": 0.5},
    "estimator": {
        "directional_result": False,
        "receiver_stages_per_round_trip": 2,
        "shared_memory_hops_per_round_trip": 4,
        "statement": "RTT/2 averages two full sender, receiver decode/dedupe, and endpoint shared-memory paths",
    },
    "parameters": {
        "batch_size": int(batch_size),
        "batch_timeout_us": int(batch_timeout_us),
        "count": int(count),
        "fec_k": int(fec_k),
        "fec_timeout_us": int(fec_timeout_us),
        "test_drop_pct": float(test_drop_pct),
        "allow_loss": allow_loss == "true",
        "impairment_method": "sender_test_drop" if float(test_drop_pct) > 0 else "none",
        "forward_port": int(forward_port),
        "latency_output": latency_output,
        "rate": int(rate),
        "return_port": int(return_port),
        "slots": int(slots),
        "warmup": int(warmup),
    },
    "rx_host": rx_host,
    "rx_private": rx_private,
    "sample_count": int(count),
    "harness_revision": harness_revision,
    "source_revision": revision,
    "topology": "producer->sender->receiver->relay->sender->receiver->consumer",
    "transport_mode": transport_mode,
    "tx_host": tx_host,
    "tx_private": tx_private,
    "wall_clock_duration_s": int(duration_ns) / 1000000000.0,
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

trap - EXIT INT TERM
cleanup
