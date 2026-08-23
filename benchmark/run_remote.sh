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
no_build="false"

usage() {
  printf '%s\n' "usage: bash benchmark/run_remote.sh --tx-host HOST --rx-hosts HOSTS --rx-privates IPS [--outdir DIR] [--rate N] [--count N] [--warmup N] [--fanout N]"
}

require_value() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
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
    --no-build) no_build="true"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$tx_host" || -z "$rx_hosts" || -z "$rx_privates" ]]; then
  usage >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
if [[ -z "$outdir" ]]; then
  outdir="$repo_root/benchmark/results/remote_$(date -u +%Y%m%dT%H%M%SZ)"
fi
outdir="$(mkdir -p "$outdir" && cd "$outdir" && pwd)"
run_id="run_$(date -u +%Y%m%dT%H%M%SZ)_$$"
remote_base="/tmp/$run_id"
total_count=$((count + warmup))
start_ns="$(date +%s%N)"
IFS=',' read -r -a rx_host_array <<< "$rx_hosts"
IFS=',' read -r -a rx_private_array <<< "$rx_privates"

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
  for item in "${remote_pid_files[@]}"; do
    host="${item%%:*}"
    pid_file="${item#*:}"
    ssh_host "$host" "if [[ -s '$pid_file' ]]; then kill \$(cat '$pid_file') 2>/dev/null || true; fi" >/dev/null 2>&1 || true
  done
}
trap cleanup_remote EXIT INT TERM

if [[ "$no_build" == "false" ]]; then
  ssh_host "$tx_host" "cd $remote_repo && make -C harness clean && make -C harness && make -C harness test"
  for host in "${rx_host_array[@]}"; do
    ssh_host "$host" "cd $remote_repo && make -C harness clean && make -C harness && make -C harness test"
  done
fi

for host in "$tx_host" "${rx_host_array[@]}"; do
  ssh_host "$host" "mkdir -p '$remote_base'"
done

for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  port=$((base_port + i))
  rx_name="rx_$((i + 1))"
  remote_rx="$remote_base/$rx_name"
  out_shm="/fanout_out_$((i + 1))_$run_id"
  ssh_host "$host" "mkdir -p '$remote_rx' && rm -f /dev/shm/${out_shm#/} && cd $remote_repo && taskset -c '$cpu_receiver' harness/bin/receiver --out-shm '$out_shm' --slots '$slots' --port '$port' --count '$total_count' --rcvbuf '$rcvbuf' --fec-recovery-csv '$remote_rx/fec_recovery.csv' >'$remote_rx/receiver.log' 2>&1 & echo \$! >'$remote_rx/receiver.pid'"
  remote_pid_files+=("$host:$remote_rx/receiver.pid")
  for attempt in $(seq 1 100); do
    if ssh_host "$host" "ss -lun sport = :$port | grep -q ':$port'" >/dev/null 2>&1; then
      break
    fi
    if [[ "$attempt" == "100" ]]; then
      printf '%s\n' "receiver bind timeout host=$host port=$port" >&2
      exit 1
    fi
    sleep 0.05
  done
  ssh_host "$host" "cd $remote_repo && taskset -c '$cpu_consumer' harness/bin/consumer --shm '$out_shm' --slots '$slots' --from-edge --count '$total_count' --csv '$remote_rx/latency.bin' >'$remote_rx/consumer.log' 2>&1 & echo \$! >'$remote_rx/consumer.pid'"
  remote_pid_files+=("$host:$remote_rx/consumer.pid")
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
  sender_args_text+=" '$(printf '%s' "$value" | sed "s/'/'\\''/g")'"
done

ssh_host "$tx_host" "mkdir -p '$remote_base/tx' && rm -f /dev/shm/fanout_in_$run_id && cd $remote_repo && taskset -c '$cpu_producer' harness/bin/producer --shm '/fanout_in_$run_id' --slots '$slots' --count '$total_count' --rate '$rate' --type '$message_type' >'$remote_base/tx/producer.log' 2>&1 & echo \$! >'$remote_base/tx/producer.pid'"
remote_pid_files+=("$tx_host:$remote_base/tx/producer.pid")
sleep 0.05
ssh_host "$tx_host" "cd $remote_repo && taskset -c '$cpu_sender' harness/bin/sender --in-shm '/fanout_in_$run_id' --slots '$slots' --count '$total_count' --sndbuf '$sndbuf' --fec-k '$fec_k' --fec-timeout-us '$fec_timeout_us' --batch-size '$batch_size' --batch-timeout-us '$batch_timeout_us' $sender_args_text >'$remote_base/tx/sender.log' 2>&1 & echo \$! >'$remote_base/tx/sender.pid'"
remote_pid_files+=("$tx_host:$remote_base/tx/sender.pid")

ssh_host "$tx_host" "tail --pid=\$(cat '$remote_base/tx/producer.pid') -f /dev/null; tail --pid=\$(cat '$remote_base/tx/sender.pid') -f /dev/null"
for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  rx_name="rx_$((i + 1))"
  ssh_host "$host" "tail --pid=\$(cat '$remote_base/$rx_name/receiver.pid') -f /dev/null || true; tail --pid=\$(cat '$remote_base/$rx_name/consumer.pid') -f /dev/null || true; cd $remote_repo && if [[ -s '$remote_base/$rx_name/latency.bin' ]]; then python3 benchmark/bin_to_csv.py '$remote_base/$rx_name/latency.bin' '$remote_base/$rx_name/latency.csv'; fi"
done

mkdir -p "$outdir/tx"
rsync_from "$tx_host" "$remote_base/tx/" "$outdir/tx/"
for ((i=0; i<fanout; ++i)); do
  host_index="$i"
  if (( host_index >= ${#rx_host_array[@]} )); then
    host_index=0
  fi
  host="${rx_host_array[$host_index]}"
  rx_name="rx_$((i + 1))"
  mkdir -p "$outdir/$rx_name"
  rsync_from "$host" "$remote_base/$rx_name/" "$outdir/$rx_name/"
  if [[ ! -s "$outdir/$rx_name/latency.csv" ]] || [[ "$(wc -l < "$outdir/$rx_name/latency.csv")" -le 1 ]]; then
    printf '%s\n' "consumer produced no rows for $rx_name" >"$outdir/$rx_name/FAILURE"
    exit 1
  fi
done

end_ns="$(date +%s%N)"
export RUN_RATE="$rate" RUN_COUNT="$count" RUN_WARMUP="$warmup" RUN_SLOTS="$slots" RUN_TYPE="$message_type" RUN_PORT="$base_port" RUN_FANOUT="$fanout" RUN_SNDBUF="$sndbuf" RUN_RCVBUF="$rcvbuf" RUN_FEC_K="$fec_k" RUN_FEC_TIMEOUT_US="$fec_timeout_us" RUN_BATCH_SIZE="$batch_size" RUN_BATCH_TIMEOUT_US="$batch_timeout_us" RUN_TX_HOST="$tx_host" RUN_RX_HOSTS="$rx_hosts" RUN_INSTANCE_TYPE="$instance_type" RUN_AZ="$az" RUN_PLACEMENT_GROUP_TYPE="$placement_group_type" RUN_CLOCK_METHOD="$clock_method" RUN_CLOCK_RESIDUAL_BOUND_NS="$clock_residual_bound_ns" RUN_NIC_DRIVER_VERSION="$nic_driver_version" RUN_ISOLATED_CORES="$isolated_cores" RUN_MTU="$mtu" RUN_OUTDIR="$outdir" RUN_DURATION_NS="$((end_ns - start_ns))"
python3 - "$outdir/run.json" <<'PY'
import json
import os
import sys

def integer(name, default=0):
    value = os.environ.get(name, "")
    return default if value == "" else int(value)

data = {
    "parameters": {
        "rate": integer("RUN_RATE"),
        "count": integer("RUN_COUNT"),
        "warmup": integer("RUN_WARMUP"),
        "total_count": integer("RUN_COUNT") + integer("RUN_WARMUP"),
        "slots": integer("RUN_SLOTS"),
        "type": os.environ["RUN_TYPE"],
        "port": os.environ["RUN_PORT"],
        "fanout": integer("RUN_FANOUT"),
        "sndbuf": integer("RUN_SNDBUF"),
        "rcvbuf": integer("RUN_RCVBUF"),
        "fec_k": integer("RUN_FEC_K"),
        "fec_timeout_us": integer("RUN_FEC_TIMEOUT_US"),
        "batch_size": integer("RUN_BATCH_SIZE"),
        "batch_timeout_us": integer("RUN_BATCH_TIMEOUT_US"),
        "outdir": os.environ["RUN_OUTDIR"]
    },
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