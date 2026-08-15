#!/usr/bin/env bash
set -u

rates="25000,50000,100000,150000,200000,300000,400000,600000,800000,1000000"
count=""
warmup=""
warmup_seconds="0.5"
duration_seconds="5.0"
slots="65536"
repeats="3"
outdir=""
cpu_producer=""
cpu_sender=""
cpu_receiver=""
cpu_consumer=""
sndbuf=""
rcvbuf=""

usage() {
  printf '%s\n' "usage: benchmark/sweep_rate.sh [--rates A,B,C] [--count N] [--warmup N] [--warmup-seconds S] [--duration-seconds S] [--slots N] [--repeats N] [--outdir DIR] [--cpu-producer N] [--cpu-sender N] [--cpu-receiver N] [--cpu-consumer N] [--sndbuf BYTES] [--rcvbuf BYTES]"
}

require_value() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

positive_integer() {
  case "$2" in
    ''|*[!0-9]*)
      printf '%s\n' "$1 must be a positive integer" >&2
      exit 2
      ;;
  esac
  if (( $2 < 1 )); then
    printf '%s\n' "$1 must be a positive integer" >&2
    exit 2
  fi
}

nonnegative_integer() {
  case "$2" in
    ''|*[!0-9]*)
      printf '%s\n' "$1 must be a nonnegative integer" >&2
      exit 2
      ;;
  esac
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --rates)
      require_value "$@"
      rates="$2"
      shift 2
      ;;
    --count)
      require_value "$@"
      count="$2"
      shift 2
      ;;
    --warmup)
      require_value "$@"
      warmup="$2"
      shift 2
      ;;
    --warmup-seconds)
      require_value "$@"
      warmup_seconds="$2"
      shift 2
      ;;
    --duration-seconds)
      require_value "$@"
      duration_seconds="$2"
      shift 2
      ;;
    --slots)
      require_value "$@"
      slots="$2"
      shift 2
      ;;
    --repeats)
      require_value "$@"
      repeats="$2"
      shift 2
      ;;
    --outdir)
      require_value "$@"
      outdir="$2"
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

positive_integer "--repeats" "$repeats"
positive_integer "--slots" "$slots"
if [[ -n "$count" ]]; then
  positive_integer "--count" "$count"
fi
if [[ -n "$warmup" ]]; then
  nonnegative_integer "--warmup" "$warmup"
fi

IFS=',' read -r -a rate_values <<< "$rates"
if (( ${#rate_values[@]} == 0 )); then
  printf '%s\n' "--rates must contain at least one rate" >&2
  exit 2
fi

for rate in "${rate_values[@]}"; do
  positive_integer "--rates" "$rate"
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

if [[ -z "$outdir" ]]; then
  outdir="$repo_root/benchmark/results/rate_sweep"
fi

compute_messages() {
  python3 - "$1" "$2" <<'PY'
import math
import sys
rate = int(sys.argv[1])
seconds = float(sys.argv[2])
if seconds < 0.0:
    raise SystemExit(2)
print(int(math.ceil(rate * seconds)))
PY
}

python3 - "$warmup_seconds" "$duration_seconds" <<'PY'
import sys
for label, value in (("--warmup-seconds", sys.argv[1]), ("--duration-seconds", sys.argv[2])):
    try:
        number = float(value)
    except ValueError:
        raise SystemExit(f"{label} must be a nonnegative number")
    if number < 0.0:
        raise SystemExit(f"{label} must be a nonnegative number")
PY

plan_file="$(mktemp)"
trap 'rm -f "$plan_file"' EXIT
printf 'computed run plan:\n' >&2
for rate in "${rate_values[@]}"; do
  run_count="$count"
  run_warmup="$warmup"
  if [[ -z "$run_count" ]]; then
    run_count="$(compute_messages "$rate" "$duration_seconds")"
  fi
  if [[ -z "$run_warmup" ]]; then
    run_warmup="$(compute_messages "$rate" "$warmup_seconds")"
  fi
  printf '%s\t%s\t%s\n' "$rate" "$run_count" "$run_warmup" >>"$plan_file"
  printf 'rate=%s count=%s warmup=%s\n' "$rate" "$run_count" "$run_warmup" >&2
done

if [[ -z "$outdir" ]]; then
  outdir="$repo_root/benchmark/results/rate_sweep"
fi

timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
root="$(mkdir -p "$outdir/$timestamp" && cd "$outdir/$timestamp" && pwd)"
started_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
completed_file="$root/.completed_runs"
failures_file="$root/.failures"
: >"$completed_file"
: >"$failures_file"

printf 'sweep root: %s\n' "$root" >&2
printf 'rates: %s repeats: %s slots: %s warmup_seconds: %s duration_seconds: %s\n' "$rates" "$repeats" "$slots" "$warmup_seconds" "$duration_seconds" >&2

while IFS=$'\t' read -r rate run_count run_warmup; do
  for repeat in $(seq 1 "$repeats"); do
    run_dir="$root/rate_$rate/rep_$repeat"
    mkdir -p "$run_dir"
    printf 'starting rate=%s repeat=%s count=%s warmup=%s outdir=%s\n' "$rate" "$repeat" "$run_count" "$run_warmup" "$run_dir" >&2
    command=("$script_dir/run_once.sh" --rate "$rate" --count "$run_count" --warmup "$run_warmup" --slots "$slots" --outdir "$run_dir")
    if [[ -n "$cpu_producer" ]]; then
      command+=(--cpu-producer "$cpu_producer")
    fi
    if [[ -n "$cpu_sender" ]]; then
      command+=(--cpu-sender "$cpu_sender")
    fi
    if [[ -n "$cpu_receiver" ]]; then
      command+=(--cpu-receiver "$cpu_receiver")
    fi
    if [[ -n "$cpu_consumer" ]]; then
      command+=(--cpu-consumer "$cpu_consumer")
    fi
    if [[ -n "$sndbuf" ]]; then
      command+=(--sndbuf "$sndbuf")
    fi
    if [[ -n "$rcvbuf" ]]; then
      command+=(--rcvbuf "$rcvbuf")
    fi
    exit_code=0
    "${command[@]}" || exit_code="$?"
    if [[ "$exit_code" == "0" ]]; then
      printf '%s\n' "$run_dir" >>"$completed_file"
      printf 'completed rate=%s repeat=%s\n' "$rate" "$repeat" >&2
    else
      printf '%s\t%s\t%s\t%s\n' "$run_dir" "$rate" "$repeat" "$exit_code" >>"$failures_file"
      printf 'failed rate=%s repeat=%s exit_code=%s\n' "$rate" "$repeat" "$exit_code" >&2
    fi
    sleep 2
  done
done <"$plan_file"

finished_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

export SWEEP_ROOT="$root"
export SWEEP_RATES="$rates"
export SWEEP_COUNT="$count"
export SWEEP_WARMUP="$warmup"
export SWEEP_WARMUP_SECONDS="$warmup_seconds"
export SWEEP_DURATION_SECONDS="$duration_seconds"
export SWEEP_SLOTS="$slots"
export SWEEP_REPEATS="$repeats"
export SWEEP_OUTDIR="$outdir"
export SWEEP_TIMESTAMP="$timestamp"
export SWEEP_STARTED_AT="$started_at"
export SWEEP_FINISHED_AT="$finished_at"
export SWEEP_CPU_PRODUCER="$cpu_producer"
export SWEEP_CPU_SENDER="$cpu_sender"
export SWEEP_CPU_RECEIVER="$cpu_receiver"
export SWEEP_CPU_CONSUMER="$cpu_consumer"
export SWEEP_SNDBUF="$sndbuf"
export SWEEP_RCVBUF="$rcvbuf"
export SWEEP_COMPLETED_FILE="$completed_file"
export SWEEP_FAILURES_FILE="$failures_file"
export SWEEP_PLAN_FILE="$plan_file"

if ! python3 - "$root/sweep.json" <<'PY'
import json
import os
import sys

def optional_integer(name):
    value = os.environ[name]
    return None if value == "" else int(value)

with open(os.environ["SWEEP_COMPLETED_FILE"], "r", encoding="utf-8") as handle:
    completed = [line.rstrip("\n") for line in handle if line.rstrip("\n")]

failures = []
with open(os.environ["SWEEP_FAILURES_FILE"], "r", encoding="utf-8") as handle:
    for line in handle:
        line = line.rstrip("\n")
        if not line:
            continue
        run_dir, rate, repeat, exit_code = line.split("\t")
        failures.append(
            {
                "run_dir": run_dir,
                "rate": int(rate),
                "repeat": int(repeat),
                "exit_code": int(exit_code),
            }
        )

computed = []
with open(os.environ["SWEEP_PLAN_FILE"], "r", encoding="utf-8") as handle:
    for line in handle:
        rate, count, warmup = line.rstrip("\n").split("\t")
        computed.append({"rate": int(rate), "count": int(count), "warmup": int(warmup)})

data = {
    "root": os.environ["SWEEP_ROOT"],
    "timestamp": os.environ["SWEEP_TIMESTAMP"],
    "started_at": os.environ["SWEEP_STARTED_AT"],
    "finished_at": os.environ["SWEEP_FINISHED_AT"],
    "parameters": {
        "rates": [int(rate) for rate in os.environ["SWEEP_RATES"].split(",")],
        "count": optional_integer("SWEEP_COUNT"),
        "warmup": optional_integer("SWEEP_WARMUP"),
        "warmup_seconds": float(os.environ["SWEEP_WARMUP_SECONDS"]),
        "duration_seconds": float(os.environ["SWEEP_DURATION_SECONDS"]),
        "slots": int(os.environ["SWEEP_SLOTS"]),
        "repeats": int(os.environ["SWEEP_REPEATS"]),
        "outdir": os.environ["SWEEP_OUTDIR"],
        "cpu_producer": optional_integer("SWEEP_CPU_PRODUCER"),
        "cpu_sender": optional_integer("SWEEP_CPU_SENDER"),
        "cpu_receiver": optional_integer("SWEEP_CPU_RECEIVER"),
        "cpu_consumer": optional_integer("SWEEP_CPU_CONSUMER"),
        "sndbuf": optional_integer("SWEEP_SNDBUF"),
        "rcvbuf": optional_integer("SWEEP_RCVBUF"),
        "computed_counts": computed,
    },
    "completed_run_dirs": completed,
    "failures": failures,
}

with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY
then
  printf '%s\n' "failed to write $root/sweep.json" >&2
  exit 1
fi

rm -f "$completed_file" "$failures_file" "$plan_file"
trap - EXIT
printf 'wrote %s\n' "$root/sweep.json" >&2
printf '%s\n' "$root"