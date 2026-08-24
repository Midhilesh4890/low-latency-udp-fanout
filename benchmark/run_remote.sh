set -euo pipefail

tx_host=""
rx_hosts=""
rx_privates=""
ssh_user="ubuntu"
ssh_key="${SSH_KEY:-}"
remote_repo="${REMOTE_REPO:-~/task}"
outdir=""
rate="100000"
count="500000"
warmup="20000"
slots="65536"
message_type="mixed"
base_port="9000"
fanout="1"
cpu_producer="2"
cpu_sender="4"
cpu_receiver="2"
cpu_consumer="4"
cpu_receivers=""
cpu_consumers=""
sndbuf="67108864"
rcvbuf="67108864"
fec_k="0"
fec_timeout_us="200"
batch_size="32"
batch_timeout_us="50"
clock_method="none"
clock_residual_bound_ns=""
instance_type=""
az=""
placement_group_type=""
nic_driver_version=""
isolated_cores=""
mtu=""
latency_output="disk"
no_build="false"
run_order_index=""
p9999_grade="true"
run_label=""

usage() {
  printf '%s\n' "usage: bash benchmark/run_remote.sh --tx-host HOST --rx-hosts HOSTS --rx-privates IPS [--outdir DIR] [--rate N] [--count N] [--warmup N] [--fanout N]"
}

require_value() {
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
    --tx-host) require_value "$@"; tx_host="$2"; shift 2 ;;
    --rx-hosts) require_value "$@"; rx_hosts="$2"; shift 2 ;;
    --rx-privates) require_value "$@"; rx_privates="$2"; shift 2 ;;
    --ssh-user) require_value "$@"; ssh_user="$2"; shift 2 ;;
    --ssh-key) require_value "$@"; ssh_key="$2"; shift 2 ;;
    --remote-repo) require_value "$@"; remote_repo="$2"; shift 2 ;;
    --outdir) require_value "$@"; outdir="$2"; shift 2 ;;
    --rate) require_value "$@"; rate="$2"; shift 2 ;;
    --count) require_value "$@"; count="$2"; shift 2 ;;
    --warmup) require_value "$@"; warmup="$2"; shift 2 ;;
    --slots) require_value "$@"; slots="$2"; shift 2 ;;
    --type) require_value "$@"; message_type="$2"; shift 2 ;;
    --base-port) require_value "$@"; base_port="$2"; shift 2 ;;
    --fanout) require_value "$@"; fanout="$2"; shift 2 ;;
    --cpu-producer) require_value "$@"; cpu_producer="$2"; shift 2 ;;
    --cpu-sender) require_value "$@"; cpu_sender="$2"; shift 2 ;;
    --cpu-receiver) require_value "$@"; cpu_receiver="$2"; shift 2 ;;
    --cpu-consumer) require_value "$@"; cpu_consumer="$2"; shift 2 ;;
    --cpu-receivers) require_value "$@"; cpu_receivers="$2"; shift 2 ;;
    --cpu-consumers) require_value "$@"; cpu_consumers="$2"; shift 2 ;;
    --sndbuf) require_value "$@"; sndbuf="$2"; shift 2 ;;
    --rcvbuf) require_value "$@"; rcvbuf="$2"; shift 2 ;;
    --fec-k) require_value "$@"; fec_k="$2"; shift 2 ;;
    --fec-timeout-us) require_value "$@"; fec_timeout_us="$2"; shift 2 ;;
    --batch-size) require_value "$@"; batch_size="$2"; shift 2 ;;
    --batch-timeout-us) require_value "$@"; batch_timeout_us="$2"; shift 2 ;;
    --clock-method) require_value "$@"; clock_method="$2"; shift 2 ;;
    --clock-residual-bound-ns) require_value "$@"; clock_residual_bound_ns="$2"; shift 2 ;;
    --instance-type) require_value "$@"; instance_type="$2"; shift 2 ;;
    --az) require_value "$@"; az="$2"; shift 2 ;;
    --placement-group-type) require_value "$@"; placement_group_type="$2"; shift 2 ;;
    --nic-driver-version) require_value "$@"; nic_driver_version="$2"; shift 2 ;;
    --isolated-cores) require_value "$@"; isolated_cores="$2"; shift 2 ;;
    --mtu) require_value "$@"; mtu="$2"; shift 2 ;;
    --latency-output) require_value "$@"; latency_output="$2"; shift 2 ;;
    --run-order-index) require_value "$@"; run_order_index="$2"; shift 2 ;;
    --p9999-grade) p9999_grade="true"; shift ;;
    --not-p9999-grade) p9999_grade="false"; shift ;;
    --run-label) require_value "$@"; run_label="$2"; shift 2 ;;
    --no-build) no_build="true"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$tx_host" || -z "$rx_hosts" || -z "$rx_privates" ]]; then
  usage >&2
  exit 2
fi
case "$latency_output" in
  disk|tmpfs|none) ;;
  *) printf '%s\n' "unknown --latency-output: $latency_output" >&2; exit 2 ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
if [[ -z "$outdir" ]]; then
  outdir="$repo_root/benchmark/results/remote_$(date -u +%Y%m%dT%H%M%SZ)"
fi
outdir="$(mkdir -p "$outdir" && cd "$outdir" && pwd)"
run_id="run_$(date -u +%Y%m%dT%H%M%SZ)_$$"
remote_base="/tmp/$run_id"
total_count=$((count + warmup))
stream_seconds=$(((total_count + rate - 1) / rate))
completion_seconds=$((stream_seconds + 60))
start_ns="$(date +%s%N)"
IFS=',' read -r -a rx_host_array <<< "$rx_hosts"
IFS=',' read -r -a rx_private_array <<< "$rx_privates"
if [[ -z "$cpu_receivers" ]]; then
  cpu_receivers="$cpu_receiver"
fi
if [[ -z "$cpu_consumers" ]]; then
  cpu_consumers="$cpu_consumer"
fi
IFS=',' read -r -a cpu_receiver_array <<< "$cpu_receivers"
IFS=',' read -r -a cpu_consumer_array <<< "$cpu_consumers"
if (( ${#cpu_receiver_array[@]} < fanout || ${#cpu_consumer_array[@]} < fanout )); then
  printf '%s\n' "--cpu-receivers and --cpu-consumers need one core per fan-out receiver" >&2
  exit 2
fi

ssh_opts=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=10)
if [[ -n "$ssh_key" ]]; then
  ssh_opts+=(-i "$ssh_key")
fi

ssh_host() {
  local host="$1"
  shift
  ssh "${ssh_opts[@]}" "$ssh_user@$host" "$@"
}

rsync_from() {
  local host="$1"
  local source="$2"
  local target="$3"
  if [[ -n "$ssh_key" ]]; then
    rsync -az -e "ssh -i $ssh_key -o BatchMode=yes -o StrictHostKeyChecking=accept-new" "$ssh_user@$host:$source" "$target"
  else
    rsync -az -e "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new" "$ssh_user@$host:$source" "$target"
  fi
}

remote_pid_files=()
cleanup_remote() {
  local idx item host rest pid_file label
  for ((idx=${#remote_pid_files[@]}-1; idx>=0; --idx)); do
    item="${remote_pid_files[$idx]}"
    host="${item%%|*}"
    rest="${item#*|}"
    pid_file="${rest%%|*}"
    label="${rest#*|}"
    ssh_host "$host" "if [[ -s '$pid_file' ]]; then pid=\$(cat '$pid_file'); kill \$pid 2>/dev/null || true; for i in \$(seq 1 40); do kill -0 \$pid 2>/dev/null || exit 0; sleep 0.05; done; kill -9 \$pid 2>/dev/null || true; fi" >/dev/null 2>&1 || true
    printf '%s\n' "cleanup attempted: $label on $host" >&2 || true
  done
}
trap cleanup_remote EXIT INT TERM

wait_remote() {
  local host="$1"
  local timeout_s="$2"
  local label="$3"
  local command="$4"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    if ssh_host "$host" "$command" >/dev/null 2>&1; then
      printf '%s\n' "confirmed: $label"
      return 0
    fi
    sleep 0.05
  done
  printf '%s\n' "confirmation failed after ${timeout_s}s: $label" >&2
  return 1
}

wait_remote_pid_exit() {
  local host="$1"
  local pid_file="$2"
  local label="$3"
  local timeout_s="$4"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    if ssh_host "$host" "if [[ -s '$pid_file' ]]; then ! kill -0 \$(cat '$pid_file') 2>/dev/null; else false; fi" >/dev/null 2>&1; then
      printf '%s\n' "exited: $label"
      return 0
    fi
    sleep 0.2
  done
  printf '%s\n' "process did not exit after ${timeout_s}s: $label" >&2
  return 1
}

register_pid() {
  remote_pid_files+=("$1|$2|$3")
}

if [[ "$no_build" == "false" ]]; then
  ssh_host "$tx_host" "cd $remote_repo && make -C harness clean && make -C harness && make -C harness test"
  for host in "${rx_host_array[@]}"; do
    ssh_host "$host" "cd $remote_repo && make -C harness clean && make -C harness && make -C harness test"
  done
fi

for host in "$tx_host" "${rx_host_array[@]}"; do
  ssh_host "$host" "mkdir -p '$remote_base'"
done

preflight_remote() {
  local host="$1"
  local cores_arg="$2"
  local label_arg="$3"
  local iso_arg="${isolated_cores:-$cores_arg}"
  ssh_host "$host" "cd $remote_repo && bash benchmark/preflight_isolation.sh --cores '$cores_arg' --isolated-cores '$iso_arg' --label '$label_arg'"
}

declare -A preflight_core_lists=()
declare -A preflight_labels=()
preflight_core_lists[$tx_host]="$cpu_producer,$cpu_sender"
preflight_labels[$tx_host]="tx"
declare -A rx_core_lists=()
for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  value="${cpu_receiver_array[$i]},${cpu_consumer_array[$i]}"
  if [[ -n "${rx_core_lists[$host]+x}" ]]; then
    rx_core_lists[$host]="${rx_core_lists[$host]},$value"
  else
    rx_core_lists[$host]="$value"
  fi
done
for host in "${!rx_core_lists[@]}"; do
  if [[ -n "${preflight_core_lists[$host]+x}" ]]; then
    preflight_core_lists[$host]="${preflight_core_lists[$host]},${rx_core_lists[$host]}"
    preflight_labels[$host]="${preflight_labels[$host]}+rx"
  else
    preflight_core_lists[$host]="${rx_core_lists[$host]}"
    preflight_labels[$host]="rx"
  fi
done
for host in "${!preflight_core_lists[@]}"; do
  preflight_remote "$host" "${preflight_core_lists[$host]}" "${preflight_labels[$host]}"
done

rx_remote_dirs=()
for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  port=$((base_port + i))
  rx_name="rx_$((i + 1))"
  remote_rx="$remote_base/$rx_name"
  rx_remote_dirs+=("$host|$rx_name|$remote_rx")
  out_shm="/fanout_out_$((i + 1))_$run_id"
  rx_cpu="${cpu_receiver_array[$i]}"
  ssh_host "$host" "mkdir -p '$remote_rx' && rm -f /dev/shm/${out_shm#/} && cd $remote_repo && (nohup taskset -c '$rx_cpu' harness/bin/receiver --out-shm '$out_shm' --slots '$slots' --port '$port' --count '$total_count' --rcvbuf '$rcvbuf' --idle-ms 30000 --fec-recovery-csv '$remote_rx/fec_recovery.csv' < /dev/null >'$remote_rx/receiver.log' 2>&1 & echo \$! >'$remote_rx/receiver.pid')"
  register_pid "$host" "$remote_rx/receiver.pid" "$rx_name receiver"
  wait_remote "$host" 10 "$rx_name receiver pid" "[[ -s '$remote_rx/receiver.pid' ]]"
  wait_remote "$host" 10 "$rx_name receiver alive" "kill -0 \$(cat '$remote_rx/receiver.pid') 2>/dev/null"
  wait_remote "$host" 10 "$rx_name udp bind port $port" "ss -H -lun | grep -q ':$port'"
  wait_remote "$host" 10 "$rx_name output shm" "[[ -e '/dev/shm/${out_shm#/}' ]]"
done

for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  rx_name="rx_$((i + 1))"
  remote_rx="$remote_base/$rx_name"
  out_shm="/fanout_out_$((i + 1))_$run_id"
  consumer_cpu="${cpu_consumer_array[$i]}"
  csv_args=""
  if [[ "$latency_output" == "disk" ]]; then
    csv_args="--csv '$remote_rx/latency.bin'"
  elif [[ "$latency_output" == "tmpfs" ]]; then
    csv_args="--csv '/dev/shm/${run_id}_${rx_name}_latency.bin'"
    ssh_host "$host" "rm -f '/dev/shm/${run_id}_${rx_name}_latency.bin'"
  fi
  ssh_host "$host" "cd $remote_repo && (nohup taskset -c '$consumer_cpu' harness/bin/consumer --shm '$out_shm' --slots '$slots' --count '$count' --skip '$warmup' --idle-ms 30000 $csv_args < /dev/null >'$remote_rx/consumer.log' 2>&1 & echo \$! >'$remote_rx/consumer.pid')"
  register_pid "$host" "$remote_rx/consumer.pid" "$rx_name consumer"
  wait_remote "$host" 10 "$rx_name consumer pid" "[[ -s '$remote_rx/consumer.pid' ]]"
  wait_remote "$host" 10 "$rx_name consumer alive" "kill -0 \$(cat '$remote_rx/consumer.pid') 2>/dev/null"
done

sender_args=()
for ((i=0; i<fanout; ++i)); do
  private_index="$i"
  if (( private_index >= ${#rx_private_array[@]} )); then
    private_index=0
  fi
  sender_args+=(--dst "${rx_private_array[$private_index]}:$((base_port + i))")
done
sender_args_text=""
for value in "${sender_args[@]}"; do
  sender_args_text+=" $(quote_one "$value")"
done

ssh_host "$tx_host" "mkdir -p '$remote_base/tx' && rm -f /dev/shm/fanout_in_$run_id && cd $remote_repo && (nohup taskset -c '$cpu_producer' harness/bin/producer --shm '/fanout_in_$run_id' --slots '$slots' --count '$total_count' --rate '$rate' --type '$message_type' < /dev/null >'$remote_base/tx/producer.log' 2>&1 & echo \$! >'$remote_base/tx/producer.pid') && for i in \$(seq 1 10000); do [[ -e '/dev/shm/fanout_in_$run_id' ]] && break; sleep 0.001; done && [[ -e '/dev/shm/fanout_in_$run_id' ]] && (nohup taskset -c '$cpu_sender' harness/bin/sender --in-shm '/fanout_in_$run_id' --slots '$slots' --count '$total_count' --sndbuf '$sndbuf' --fec-k '$fec_k' --fec-timeout-us '$fec_timeout_us' --batch-size '$batch_size' --batch-timeout-us '$batch_timeout_us' $sender_args_text < /dev/null >'$remote_base/tx/sender.log' 2>&1 & echo \$! >'$remote_base/tx/sender.pid')"
register_pid "$tx_host" "$remote_base/tx/producer.pid" "tx producer"
register_pid "$tx_host" "$remote_base/tx/sender.pid" "tx sender"
wait_remote "$tx_host" 10 "producer pid" "[[ -s '$remote_base/tx/producer.pid' ]]"
wait_remote "$tx_host" 10 "sender pid" "[[ -s '$remote_base/tx/sender.pid' ]]"

wait_remote_pid_exit "$tx_host" "$remote_base/tx/sender.pid" "sender" "$completion_seconds"
wait_remote_pid_exit "$tx_host" "$remote_base/tx/producer.pid" "producer" 30
for entry in "${rx_remote_dirs[@]}"; do
  host="${entry%%|*}"
  rest="${entry#*|}"
  rx_name="${rest%%|*}"
  remote_rx="${rest#*|}"
  wait_remote_pid_exit "$host" "$remote_rx/receiver.pid" "$rx_name receiver" 30 || true
  wait_remote_pid_exit "$host" "$remote_rx/consumer.pid" "$rx_name consumer" 30 || true
  if [[ "$latency_output" == "disk" ]]; then
    ssh_host "$host" "cd $remote_repo && if [[ -s '$remote_rx/latency.bin' ]]; then python3 benchmark/bin_to_csv.py '$remote_rx/latency.bin' '$remote_rx/latency.csv'; fi"
  elif [[ "$latency_output" == "tmpfs" ]]; then
    ssh_host "$host" "cd $remote_repo && if [[ -s '/dev/shm/${run_id}_${rx_name}_latency.bin' ]]; then python3 benchmark/bin_to_csv.py '/dev/shm/${run_id}_${rx_name}_latency.bin' '$remote_rx/latency.csv' && cp '/dev/shm/${run_id}_${rx_name}_latency.bin' '$remote_rx/latency.bin'; fi"
  else
    ssh_host "$host" "printf '%s\n' 'latency_output=none' >'$remote_rx/latency_output.txt'"
  fi
done

mkdir -p "$outdir/tx"
rsync_from "$tx_host" "$remote_base/tx/" "$outdir/tx/"
for entry in "${rx_remote_dirs[@]}"; do
  host="${entry%%|*}"
  rest="${entry#*|}"
  rx_name="${rest%%|*}"
  remote_rx="${rest#*|}"
  mkdir -p "$outdir/$rx_name"
  rsync_from "$host" "$remote_rx/" "$outdir/$rx_name/"
  grep -q "received     : $count" "$outdir/$rx_name/consumer.log"
  grep -q "dropped      : 0" "$outdir/$rx_name/consumer.log"
  grep -q "published=$total_count" "$outdir/$rx_name/receiver.log"
  if [[ "$latency_output" != "none" ]]; then
    if [[ ! -s "$outdir/$rx_name/latency.csv" ]] || [[ "$(wc -l < "$outdir/$rx_name/latency.csv")" -le 1 ]]; then
      printf '%s\n' "consumer produced no rows for $rx_name" >"$outdir/$rx_name/FAILURE"
      exit 1
    fi
  fi
done

end_ns="$(date +%s%N)"
export RUN_RATE="$rate" RUN_COUNT="$count" RUN_WARMUP="$warmup" RUN_SLOTS="$slots" RUN_TYPE="$message_type" RUN_PORT="$base_port" RUN_FANOUT="$fanout" RUN_SNDBUF="$sndbuf" RUN_RCVBUF="$rcvbuf" RUN_FEC_K="$fec_k" RUN_FEC_TIMEOUT_US="$fec_timeout_us" RUN_BATCH_SIZE="$batch_size" RUN_BATCH_TIMEOUT_US="$batch_timeout_us" RUN_TX_HOST="$tx_host" RUN_RX_HOSTS="$rx_hosts" RUN_INSTANCE_TYPE="$instance_type" RUN_AZ="$az" RUN_PLACEMENT_GROUP_TYPE="$placement_group_type" RUN_CLOCK_METHOD="$clock_method" RUN_CLOCK_RESIDUAL_BOUND_NS="$clock_residual_bound_ns" RUN_NIC_DRIVER_VERSION="$nic_driver_version" RUN_ISOLATED_CORES="$isolated_cores" RUN_MTU="$mtu" RUN_OUTDIR="$outdir" RUN_DURATION_NS="$((end_ns - start_ns))" RUN_CPU_PRODUCER="$cpu_producer" RUN_CPU_SENDER="$cpu_sender" RUN_CPU_RECEIVER="$cpu_receiver" RUN_CPU_CONSUMER="$cpu_consumer" RUN_CPU_RECEIVERS="$cpu_receivers" RUN_CPU_CONSUMERS="$cpu_consumers" RUN_ORDER_INDEX="$run_order_index" RUN_P9999_GRADE="$p9999_grade" RUN_LABEL="$run_label" RUN_LATENCY_OUTPUT="$latency_output"
python3 - "$outdir/run.json" <<'PY'
import json
import os
import sys

def integer(name, default=0):
    value = os.environ.get(name, "")
    return default if value == "" else int(value)

def optional_integer(name):
    value = os.environ.get(name, "")
    return None if value == "" else int(value)

data = {
    "parameters": {
        "rate": integer("RUN_RATE"),
        "count": integer("RUN_COUNT"),
        "warmup": integer("RUN_WARMUP"),
        "total_count": integer("RUN_COUNT") + integer("RUN_WARMUP"),
        "sample_count": integer("RUN_COUNT"),
        "slots": integer("RUN_SLOTS"),
        "type": os.environ["RUN_TYPE"],
        "port": os.environ["RUN_PORT"],
        "fanout": integer("RUN_FANOUT"),
        "cpu_producer": integer("RUN_CPU_PRODUCER"),
        "cpu_sender": integer("RUN_CPU_SENDER"),
        "cpu_receiver": integer("RUN_CPU_RECEIVER"),
        "cpu_consumer": integer("RUN_CPU_CONSUMER"),
        "cpu_receivers": [int(value) for value in os.environ["RUN_CPU_RECEIVERS"].split(",")],
        "cpu_consumers": [int(value) for value in os.environ["RUN_CPU_CONSUMERS"].split(",")],
        "sndbuf": integer("RUN_SNDBUF"),
        "rcvbuf": integer("RUN_RCVBUF"),
        "fec_k": integer("RUN_FEC_K"),
        "fec_timeout_us": integer("RUN_FEC_TIMEOUT_US"),
        "batch_size": integer("RUN_BATCH_SIZE"),
        "batch_timeout_us": integer("RUN_BATCH_TIMEOUT_US"),
        "latency_output": os.environ["RUN_LATENCY_OUTPUT"],
        "p9999_grade": os.environ["RUN_P9999_GRADE"] == "true",
        "outdir": os.environ["RUN_OUTDIR"]
    },
    "sample_count": integer("RUN_COUNT"),
    "run_order_index": optional_integer("RUN_ORDER_INDEX"),
    "p9999_grade": os.environ["RUN_P9999_GRADE"] == "true",
    "run_label": os.environ["RUN_LABEL"],
    "hostname": os.environ["RUN_TX_HOST"],
    "tx_host": os.environ["RUN_TX_HOST"],
    "rx_host": os.environ["RUN_RX_HOSTS"],
    "instance_type": os.environ["RUN_INSTANCE_TYPE"],
    "az": os.environ["RUN_AZ"],
    "placement_group_type": os.environ["RUN_PLACEMENT_GROUP_TYPE"],
    "nic_driver_version": os.environ["RUN_NIC_DRIVER_VERSION"],
    "isolated_cores": os.environ["RUN_ISOLATED_CORES"],
    "mtu": os.environ["RUN_MTU"],
    "wall_clock_duration_s": integer("RUN_DURATION_NS") / 1000000000.0,
    "clock_sync": {
        "method": os.environ["RUN_CLOCK_METHOD"],
        "max_drift_ns": integer("RUN_CLOCK_RESIDUAL_BOUND_NS", 0) if os.environ.get("RUN_CLOCK_RESIDUAL_BOUND_NS") else None
    },
    "exit_code": 0
}
with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

trap - EXIT INT TERM
cleanup_remote
