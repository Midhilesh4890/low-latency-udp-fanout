#!/usr/bin/env bash
set -euo pipefail

role="all"
rate="100000"
count="200000"
slots="65536"
message_type="mixed"
in_shm="/fanout_in"
out_shm="/fanout_out"
port="9000"
host="127.0.0.1"
outdir=""
warmup="20000"
cpu_producer="1"
cpu_sender="2"
cpu_receiver="4"
cpu_consumer="6"
sndbuf="4194304"
rcvbuf="4194304"
no_build="false"
has_destinations="false"
declare -a destinations=()

usage() {
  printf '%s\n' "usage: benchmark/run_once.sh [--role all|rx|tx] [--rate N] [--count N] [--slots N] [--type trade|bbo|book|mixed] [--in-shm NAME] [--out-shm NAME] [--port PORT] [--host HOST] [--outdir DIR] [--warmup N] [--cpu-producer N] [--cpu-sender N] [--cpu-receiver N] [--cpu-consumer N] [--sndbuf BYTES] [--rcvbuf BYTES] [--no-build] [--dst HOST:PORT]"
}

require_value() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --role)
      require_value "$@"
      role="$2"
      shift 2
      ;;
    --rate)
      require_value "$@"
      rate="$2"
      shift 2
      ;;
    --count)
      require_value "$@"
      count="$2"
      shift 2
      ;;
    --slots)
      require_value "$@"
      slots="$2"
      shift 2
      ;;
    --type)
      require_value "$@"
      message_type="$2"
      shift 2
      ;;
    --in-shm)
      require_value "$@"
      in_shm="$2"
      shift 2
      ;;
    --out-shm)
      require_value "$@"
      out_shm="$2"
      shift 2
      ;;
    --port)
      require_value "$@"
      port="$2"
      shift 2
      ;;
    --host)
      require_value "$@"
      host="$2"
      shift 2
      ;;
    --outdir)
      require_value "$@"
      outdir="$2"
      shift 2
      ;;
    --warmup)
      require_value "$@"
      warmup="$2"
      shift 2
      ;;
    --cpu-producer)
      require_value "$@"
      cpu_producer="$2"
      shift 2
      ;;
    --cpu-sender)
      require_value "$@"
      cpu_sender="$2"
      shift 2
      ;;
    --cpu-receiver)
      require_value "$@"
      cpu_receiver="$2"
      shift 2
      ;;
    --cpu-consumer)
      require_value "$@"
      cpu_consumer="$2"
      shift 2
      ;;
    --sndbuf)
      require_value "$@"
      sndbuf="$2"
      shift 2
      ;;
    --rcvbuf)
      require_value "$@"
      rcvbuf="$2"
      shift 2
      ;;
    --no-build)
      no_build="true"
      shift
      ;;
    --dst)
      require_value "$@"
      destinations+=("$2")
      has_destinations="true"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf '%s\n' "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$role" in
  all|rx|tx)
    ;;
  *)
    printf '%s\n' "--role must be all, rx, or tx" >&2
    exit 2
    ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

if [[ -z "$outdir" ]]; then
  outdir="$repo_root/benchmark/results/run_$(date -u +%Y%m%dT%H%M%SZ)"
fi

outdir="$(mkdir -p "$outdir" && cd "$outdir" && pwd)"
rx_dir="$outdir/rx_local"
log_dir="$outdir"
latency_csv="$rx_dir/latency.csv"
latency_bin="$rx_dir/latency.bin"

if [[ "$role" == "all" || "$role" == "rx" ]]; then
  mkdir -p "$rx_dir"
  log_dir="$rx_dir"
fi

if [[ "$no_build" == "false" ]]; then
  make -C "$repo_root/harness"
fi

shm_path() {
  local name="$1"
  if [[ "$name" == /* ]]; then
    name="${name:1}"
  fi
  printf '/dev/shm/%s\n' "$name"
}

if [[ -d /dev/shm ]]; then
  rm -f "$(shm_path "$in_shm")" "$(shm_path "$out_shm")"
fi

total_count=$((count + warmup))
start_ns="$(date +%s%N)"
run_hostname="$(hostname)"
settle_seconds="0.05"
declare -a process_names=()
declare -a process_pids=()
declare -A process_exit_codes=()
script_exit_code=0

terminate_processes() {
  trap - EXIT INT TERM
  for pid in "${process_pids[@]}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  for pid in "${process_pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
}

trap terminate_processes EXIT INT TERM

launch_process() {
  local name="$1"
  local cpu="$2"
  local log_path="$3"
  shift 3
  local -a command=("$@")
  if [[ -n "$cpu" ]]; then
    command=(taskset -c "$cpu" "${command[@]}")
  fi
  "${command[@]}" >"$log_path" 2>&1 &
  local pid="$!"
  process_names+=("$name")
  process_pids+=("$pid")
  process_exit_codes["$name"]="running"
}

receiver_command=("$repo_root/harness/bin/receiver" --out-shm "$out_shm" --slots "$slots" --port "$port" --count "$total_count" --rcvbuf "$rcvbuf")
consumer_command=("$repo_root/harness/bin/consumer" --shm "$out_shm" --slots "$slots" --from-edge --count "$total_count" --csv "$latency_bin")
producer_command=("$repo_root/harness/bin/producer" --shm "$in_shm" --slots "$slots" --count "$total_count" --rate "$rate" --type "$message_type")
sender_command=("$repo_root/harness/bin/sender" --in-shm "$in_shm" --slots "$slots" --count "$total_count" --sndbuf "$sndbuf")

if [[ "$has_destinations" == "true" ]]; then
  for destination in "${destinations[@]}"; do
    sender_command+=(--dst "$destination")
  done
else
  sender_command+=(--host "$host" --port "$port")
fi

if [[ "$role" == "all" || "$role" == "rx" ]]; then
  launch_process "receiver" "$cpu_receiver" "$log_dir/receiver.log" "${receiver_command[@]}"
  sleep "$settle_seconds"
  launch_process "consumer" "$cpu_consumer" "$log_dir/consumer.log" "${consumer_command[@]}"
  sleep "$settle_seconds"
fi

if [[ "$role" == "all" || "$role" == "tx" ]]; then
  launch_process "producer" "$cpu_producer" "$log_dir/producer.log" "${producer_command[@]}"
  sleep "$settle_seconds"
  launch_process "sender" "$cpu_sender" "$log_dir/sender.log" "${sender_command[@]}"
fi

for index in "${!process_pids[@]}"; do
  name="${process_names[$index]}"
  pid="${process_pids[$index]}"
  if wait "$pid"; then
    process_exit_codes["$name"]="0"
  else
    process_exit_codes["$name"]="$?"
    script_exit_code=1
  fi
done

if [[ "$role" == "all" || "$role" == "rx" ]]; then
  if [[ -s "$latency_bin" ]]; then
    if ! python3 "$script_dir/bin_to_csv.py" "$latency_bin" "$latency_csv"; then
      script_exit_code=1
    fi
  else
    script_exit_code=1
  fi

  if [[ ! -s "$latency_csv" ]]; then
    script_exit_code=1
  elif [[ "$(wc -l < "$latency_csv")" -le 1 ]]; then
    script_exit_code=1
  fi
fi

end_ns="$(date +%s%N)"
duration_ns=$((end_ns - start_ns))

exit_code_lines=""
for name in "${process_names[@]}"; do
  exit_code_lines+="${name}=${process_exit_codes[$name]}"$'\n'
done

destination_lines=""
for destination in "${destinations[@]}"; do
  destination_lines+="${destination}"$'\n'
done

export RUN_ROLE="$role"
export RUN_RATE="$rate"
export RUN_COUNT="$count"
export RUN_SLOTS="$slots"
export RUN_TYPE="$message_type"
export RUN_IN_SHM="$in_shm"
export RUN_OUT_SHM="$out_shm"
export RUN_PORT="$port"
export RUN_HOST="$host"
export RUN_OUTDIR="$outdir"
export RUN_WARMUP="$warmup"
export RUN_CPU_PRODUCER="$cpu_producer"
export RUN_CPU_SENDER="$cpu_sender"
export RUN_CPU_RECEIVER="$cpu_receiver"
export RUN_CPU_CONSUMER="$cpu_consumer"
export RUN_SNDBUF="$sndbuf"
export RUN_RCVBUF="$rcvbuf"
export RUN_NO_BUILD="$no_build"
export RUN_TOTAL_COUNT="$total_count"
export RUN_DURATION_NS="$duration_ns"
export RUN_HOSTNAME="$run_hostname"
export RUN_EXIT_CODE="$script_exit_code"
export RUN_PROCESS_EXIT_CODES="$exit_code_lines"
export RUN_DESTINATIONS="$destination_lines"

python3 - "$outdir/run.json" <<'PY'
import json
import os
import sys

def integer(name):
    return int(os.environ[name])

def number(name):
    value = os.environ[name]
    try:
        return int(value)
    except ValueError:
        return float(value)

def optional_integer(name):
    value = os.environ[name]
    return None if value == "" else int(value)

exit_codes = {}
for line in os.environ["RUN_PROCESS_EXIT_CODES"].splitlines():
    if line:
        key, value = line.split("=", 1)
        exit_codes[key] = int(value)

data = {
    "role": os.environ["RUN_ROLE"],
    "parameters": {
        "rate": number("RUN_RATE"),
        "count": integer("RUN_COUNT"),
        "slots": integer("RUN_SLOTS"),
        "type": os.environ["RUN_TYPE"],
        "in_shm": os.environ["RUN_IN_SHM"],
        "out_shm": os.environ["RUN_OUT_SHM"],
        "port": os.environ["RUN_PORT"],
        "host": os.environ["RUN_HOST"],
        "outdir": os.environ["RUN_OUTDIR"],
        "warmup": integer("RUN_WARMUP"),
        "cpu_producer": optional_integer("RUN_CPU_PRODUCER"),
        "cpu_sender": optional_integer("RUN_CPU_SENDER"),
        "cpu_receiver": optional_integer("RUN_CPU_RECEIVER"),
        "cpu_consumer": optional_integer("RUN_CPU_CONSUMER"),
        "sndbuf": integer("RUN_SNDBUF"),
        "rcvbuf": integer("RUN_RCVBUF"),
        "no_build": os.environ["RUN_NO_BUILD"] == "true",
        "dst": os.environ["RUN_DESTINATIONS"].splitlines(),
        "total_count": integer("RUN_TOTAL_COUNT")
    },
    "wall_clock_duration_s": integer("RUN_DURATION_NS") / 1000000000.0,
    "exit_codes": exit_codes,
    "hostname": os.environ["RUN_HOSTNAME"],
    "clock_sync": {
        "method": "shared_clock" if os.environ["RUN_ROLE"] == "all" else "none",
        "offset_ns_start": 0,
        "offset_ns_end": 0,
        "max_drift_ns": 0
    },
    "exit_code": integer("RUN_EXIT_CODE")
}

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

trap - EXIT INT TERM
terminate_processes

exit "$script_exit_code"
