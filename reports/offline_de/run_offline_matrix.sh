set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ts="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
root="$repo_root/benchmark/results/offline_de/$ts"
mkdir -p "$root"
make -C "$repo_root/harness"

quote_command() {
  local out=""
  for item in "$@"; do
    printf -v quoted "%q" "$item"
    out+="$quoted "
  done
  printf '%s\n' "${out% }"
}

wait_one() {
  local name="$1"
  local pid="$2"
  local code=0
  if wait "$pid"; then
    code=0
  else
    code=$?
  fi
  printf '%s=%s\n' "$name" "$code" >> "$3"
  return 0
}

run_case() {
  local name="$1"
  local rate="$2"
  local loss="$3"
  local fec_k="$4"
  local rep="$5"
  local reorder_pct="$6"
  local reorder_delay_us="$7"
  local dedupe_mode="$8"
  local port="$9"
  local count=$((rate * 5))
  local slots="65536"
  local in_shm="/offline_de_in_${ts}_${port}"
  local out_shm="/offline_de_out_${ts}_${port}"
  local run_dir="$root/$name/rep_${rep}"
  local rx_dir="$run_dir/rx_local"
  local latency_bin="$rx_dir/latency.bin"
  local latency_csv="$rx_dir/latency.csv"
  mkdir -p "$rx_dir"
  rm -f "/dev/shm/${in_shm#/}" "/dev/shm/${out_shm#/}"

  local -a receiver=("$repo_root/harness/bin/receiver" --out-shm "$out_shm" --slots "$slots" --port "$port" --idle-ms 2000 --rcvbuf 4194304)
  if [[ "$dedupe_mode" == "old" ]]; then
    receiver+=(--dedupe-disable)
  fi
  local -a consumer=("$repo_root/harness/bin/consumer" --shm "$out_shm" --slots "$slots" --count 0 --idle-ms 2000 --csv "$latency_bin")
  local -a producer=("$repo_root/harness/bin/producer" --shm "$in_shm" --slots "$slots" --count "$count" --rate "$rate" --type mixed)
  local -a sender=("$repo_root/harness/bin/sender" --in-shm "$in_shm" --slots "$slots" --count "$count" --host 127.0.0.1 --port "$port" --fec-k "$fec_k" --test-seed "$((100000 + port + rep))")
  if [[ "$loss" != "0" ]]; then
    sender+=(--test-drop-pct "$loss")
  fi
  if [[ "$reorder_pct" != "0" ]]; then
    sender+=(--test-reorder-pct "$reorder_pct" --test-reorder-delay-us "$reorder_delay_us")
  fi

  {
    printf 'receiver: '; quote_command "${receiver[@]}"
    printf 'consumer: '; quote_command "${consumer[@]}"
    printf 'producer: '; quote_command "${producer[@]}"
    printf 'sender: '; quote_command "${sender[@]}"
  } > "$run_dir/commands.log"

  "${receiver[@]}" > "$rx_dir/receiver.log" 2>&1 & local receiver_pid=$!
  sleep 0.05
  "${consumer[@]}" > "$rx_dir/consumer.log" 2>&1 & local consumer_pid=$!
  sleep 0.05
  "${producer[@]}" > "$rx_dir/producer.log" 2>&1 & local producer_pid=$!
  sleep 0.05
  "${sender[@]}" > "$rx_dir/sender.log" 2>&1 & local sender_pid=$!

  : > "$run_dir/exit_codes.log"
  wait_one receiver "$receiver_pid" "$run_dir/exit_codes.log"
  wait_one consumer "$consumer_pid" "$run_dir/exit_codes.log"
  wait_one producer "$producer_pid" "$run_dir/exit_codes.log"
  wait_one sender "$sender_pid" "$run_dir/exit_codes.log"

  local exit_code=0
  if grep -qv '=0$' "$run_dir/exit_codes.log"; then
    exit_code=1
  fi
  if [[ -s "$latency_bin" ]]; then
    python3 "$repo_root/benchmark/bin_to_csv.py" "$latency_bin" "$latency_csv" || exit_code=1
  else
    printf '%s\n' "NOT_RUN latency_bin_missing_or_empty" > "$rx_dir/not_run.log"
    exit_code=1
  fi

  RUN_PATH="$run_dir/run.json" RUN_RATE="$rate" RUN_COUNT="$count" RUN_SLOTS="$slots" RUN_LOSS="$loss" RUN_FEC_K="$fec_k" RUN_REP="$rep" RUN_REORDER_PCT="$reorder_pct" RUN_REORDER_DELAY_US="$reorder_delay_us" RUN_DEDUPE_MODE="$dedupe_mode" RUN_PORT="$port" RUN_IN_SHM="$in_shm" RUN_OUT_SHM="$out_shm" RUN_EXIT_CODE="$exit_code" python3 - <<'PY'
import json
import os
from pathlib import Path
path = Path(os.environ["RUN_PATH"])
data = {
    "role": "all",
    "parameters": {
        "rate": int(os.environ["RUN_RATE"]),
        "count": int(os.environ["RUN_COUNT"]),
        "slots": int(os.environ["RUN_SLOTS"]),
        "type": "mixed",
        "warmup": 0,
        "total_count": int(os.environ["RUN_COUNT"]),
        "loss_pct": float(os.environ["RUN_LOSS"]),
        "fec_k": int(os.environ["RUN_FEC_K"]),
        "repeat": int(os.environ["RUN_REP"]),
        "test_reorder_pct": float(os.environ["RUN_REORDER_PCT"]),
        "test_reorder_delay_us": int(os.environ["RUN_REORDER_DELAY_US"]),
        "dedupe_mode": os.environ["RUN_DEDUPE_MODE"],
        "port": int(os.environ["RUN_PORT"]),
        "in_shm": os.environ["RUN_IN_SHM"],
        "out_shm": os.environ["RUN_OUT_SHM"]
    },
    "clock_sync": {
        "method": "shared_clock",
        "offset_ns_start": 0,
        "offset_ns_end": 0,
        "max_drift_ns": 0
    },
    "wall_clock_duration_s": 5.0,
    "hostname": os.uname().nodename,
    "exit_code": int(os.environ["RUN_EXIT_CODE"])
}
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

port=9300
for rate in 100000 300000; do
  for loss in 0 0.1 1; do
    for fec_k in 0 8; do
      for rep in 1 2 3; do
        label_loss="${loss//./p}"
        label_fec="off"
        if [[ "$fec_k" != "0" ]]; then
          label_fec="k${fec_k}"
        fi
        run_case "rate_${rate}/loss_${label_loss}/fec_${label_fec}" "$rate" "$loss" "$fec_k" "$rep" 0 0 new "$port"
        port=$((port + 1))
      done
    done
  done
done

run_case "reorder_probe/old" 300000 0 0 1 1 100 old "$port"
port=$((port + 1))
run_case "reorder_probe/new" 300000 0 0 1 1 100 new "$port"

python3 "$repo_root/benchmark/summarize.py" "$root" --skip-warmup 0 > "$root/summarize.txt" 2>&1 || true
printf '%s\n' "$root"
