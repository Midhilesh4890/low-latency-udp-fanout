set -u

rates="25000,50000,100000,150000,200000,300000,400000,600000,800000,1000000"
count=""
warmup=""
warmup_seconds="0.5"
duration_seconds="5.0"
slots="65536"
repeats="3"
outdir=""
remote="false"
tx_host=""
rx_hosts=""
rx_privates=""
ssh_user="ubuntu"
ssh_key=""
remote_repo="~/task"
fanout="1"
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
cpu_producer=""
cpu_sender=""
cpu_receiver=""
cpu_consumer=""
sndbuf=""
rcvbuf=""
min_p9999_samples="10000000"
min_p9999_seconds="30"
p9999_grade="true"
dry_run_order="false"

usage() {
  printf '%s\n' "usage: benchmark/sweep_rate.sh [--remote --tx-host HOST --rx-hosts HOSTS --rx-privates IPS] [--rates A,B,C] [--repeats N] [--duration-seconds S] [--fanout N]"
}

require_value() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

positive_integer() {
  case "$2" in
    ''|*[!0-9]*) printf '%s\n' "$1 must be a positive integer" >&2; exit 2 ;;
  esac
  if (( $2 < 1 )); then
    printf '%s\n' "$1 must be a positive integer" >&2
    exit 2
  fi
}

nonnegative_integer() {
  case "$2" in
    ''|*[!0-9]*) printf '%s\n' "$1 must be a nonnegative integer" >&2; exit 2 ;;
  esac
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --rates) require_value "$@"; rates="$2"; shift 2 ;;
    --count) require_value "$@"; count="$2"; shift 2 ;;
    --warmup) require_value "$@"; warmup="$2"; shift 2 ;;
    --warmup-seconds) require_value "$@"; warmup_seconds="$2"; shift 2 ;;
    --duration-seconds) require_value "$@"; duration_seconds="$2"; shift 2 ;;
    --slots) require_value "$@"; slots="$2"; shift 2 ;;
    --repeats) require_value "$@"; repeats="$2"; shift 2 ;;
    --outdir) require_value "$@"; outdir="$2"; shift 2 ;;
    --remote) remote="true"; shift ;;
    --tx-host) require_value "$@"; tx_host="$2"; shift 2 ;;
    --rx-hosts) require_value "$@"; rx_hosts="$2"; shift 2 ;;
    --rx-privates) require_value "$@"; rx_privates="$2"; shift 2 ;;
    --ssh-user) require_value "$@"; ssh_user="$2"; shift 2 ;;
    --ssh-key) require_value "$@"; ssh_key="$2"; shift 2 ;;
    --remote-repo) require_value "$@"; remote_repo="$2"; shift 2 ;;
    --fanout) require_value "$@"; fanout="$2"; shift 2 ;;
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
    --cpu-producer) require_value "$@"; cpu_producer="$2"; shift 2 ;;
    --cpu-sender) require_value "$@"; cpu_sender="$2"; shift 2 ;;
    --cpu-receiver) require_value "$@"; cpu_receiver="$2"; shift 2 ;;
    --cpu-consumer) require_value "$@"; cpu_consumer="$2"; shift 2 ;;
    --sndbuf) require_value "$@"; sndbuf="$2"; shift 2 ;;
    --rcvbuf) require_value "$@"; rcvbuf="$2"; shift 2 ;;
    --min-p9999-samples) require_value "$@"; min_p9999_samples="$2"; shift 2 ;;
    --min-p9999-seconds) require_value "$@"; min_p9999_seconds="$2"; shift 2 ;;
    --p9999-grade) p9999_grade="true"; shift ;;
    --not-p9999-grade) p9999_grade="false"; shift ;;
    --dry-run-order) dry_run_order="true"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

positive_integer "--repeats" "$repeats"
positive_integer "--slots" "$slots"
positive_integer "--fanout" "$fanout"
positive_integer "--min-p9999-samples" "$min_p9999_samples"
if [[ -n "$count" ]]; then positive_integer "--count" "$count"; fi
if [[ -n "$warmup" ]]; then nonnegative_integer "--warmup" "$warmup"; fi

IFS=',' read -r -a rate_values <<< "$rates"
for rate in "${rate_values[@]}"; do positive_integer "--rates" "$rate"; done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
if [[ -z "$outdir" ]]; then outdir="$repo_root/benchmark/results/rate_sweep"; fi

compute_messages() {
  python3 - "$1" "$2" <<'PY'
import math
import sys
print(int(math.ceil(int(sys.argv[1]) * float(sys.argv[2]))))
PY
}

max_messages() {
  python3 - "$@" <<'PY'
import sys
print(max(int(item) for item in sys.argv[1:]))
PY
}

grade_messages() {
  python3 - "$1" "$2" <<'PY'
import math
import sys
rate = int(sys.argv[1])
seconds = float(sys.argv[2])
print(int(math.ceil(rate * seconds)))
PY
}

timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
root="$(mkdir -p "$outdir/$timestamp" && cd "$outdir/$timestamp" && pwd)"
completed_file="$root/.completed_runs"
failures_file="$root/.failures"
: >"$completed_file"
: >"$failures_file"
printf 'sweep root: %s\n' "$root" >&2
order_index=0

for repeat in $(seq 1 "$repeats"); do
  for rate in "${rate_values[@]}"; do
    run_count="$count"
    run_warmup="$warmup"
    if [[ -z "$run_count" ]]; then
      run_count="$(compute_messages "$rate" "$duration_seconds")"
    fi
    if [[ "$p9999_grade" == "true" ]]; then
      floor_by_seconds="$(grade_messages "$rate" "$min_p9999_seconds")"
      run_count="$(max_messages "$run_count" "$floor_by_seconds" "$min_p9999_samples")"
    fi
    if [[ -z "$run_warmup" ]]; then run_warmup="$(compute_messages "$rate" "$warmup_seconds")"; fi
    run_dir="$root/rate_$rate/rep_$repeat"
    mkdir -p "$run_dir"
    order_index=$((order_index + 1))
    if [[ "$dry_run_order" == "true" ]]; then
      printf '%s\t%s\t%s\t%s\t%s\n' "$order_index" "$rate" "$repeat" "$run_count" "$p9999_grade" >>"$root/planned_order.tsv"
      python3 - "$run_dir/run.json" "$rate" "$run_count" "$run_warmup" "$order_index" "$p9999_grade" <<'PY'
import json
import sys

path, rate, count, warmup, order, grade = sys.argv[1:]
data = {
    "parameters": {
        "rate": int(rate),
        "count": int(count),
        "warmup": int(warmup),
        "total_count": int(count) + int(warmup),
        "sample_count": int(count),
        "p9999_grade": grade == "true"
    },
    "sample_count": int(count),
    "run_order_index": int(order),
    "p9999_grade": grade == "true",
    "run_label": "dry-run-order"
}
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY
      continue
    fi
    if [[ "$remote" == "true" ]]; then
      command=("$script_dir/run_remote.sh" --tx-host "$tx_host" --rx-hosts "$rx_hosts" --rx-privates "$rx_privates" --ssh-user "$ssh_user" --remote-repo "$remote_repo" --rate "$rate" --count "$run_count" --warmup "$run_warmup" --slots "$slots" --outdir "$run_dir" --fanout "$fanout" --fec-k "$fec_k" --fec-timeout-us "$fec_timeout_us" --batch-size "$batch_size" --batch-timeout-us "$batch_timeout_us" --clock-method "$clock_method" --run-order-index "$order_index")
      if [[ "$p9999_grade" == "true" ]]; then command+=(--p9999-grade); else command+=(--not-p9999-grade --run-label "not-p99.99-grade"); fi
      if [[ -n "$ssh_key" ]]; then command+=(--ssh-key "$ssh_key"); fi
      if [[ -n "$clock_residual_bound_ns" ]]; then command+=(--clock-residual-bound-ns "$clock_residual_bound_ns"); fi
      if [[ -n "$instance_type" ]]; then command+=(--instance-type "$instance_type"); fi
      if [[ -n "$az" ]]; then command+=(--az "$az"); fi
      if [[ -n "$placement_group_type" ]]; then command+=(--placement-group-type "$placement_group_type"); fi
      if [[ -n "$nic_driver_version" ]]; then command+=(--nic-driver-version "$nic_driver_version"); fi
      if [[ -n "$isolated_cores" ]]; then command+=(--isolated-cores "$isolated_cores"); fi
      if [[ -n "$mtu" ]]; then command+=(--mtu "$mtu"); fi
    else
      command=("$script_dir/run_once.sh" --rate "$rate" --count "$run_count" --warmup "$run_warmup" --slots "$slots" --outdir "$run_dir")
    fi
    if [[ -n "$cpu_producer" ]]; then command+=(--cpu-producer "$cpu_producer"); fi
    if [[ -n "$cpu_sender" ]]; then command+=(--cpu-sender "$cpu_sender"); fi
    if [[ -n "$cpu_receiver" ]]; then command+=(--cpu-receiver "$cpu_receiver"); fi
    if [[ -n "$cpu_consumer" ]]; then command+=(--cpu-consumer "$cpu_consumer"); fi
    if [[ -n "$sndbuf" ]]; then command+=(--sndbuf "$sndbuf"); fi
    if [[ -n "$rcvbuf" ]]; then command+=(--rcvbuf "$rcvbuf"); fi
    printf 'starting order=%s rate=%s repeat=%s outdir=%s sample_count=%s p9999_grade=%s\n' "$order_index" "$rate" "$repeat" "$run_dir" "$run_count" "$p9999_grade" >&2
    exit_code=0
    "${command[@]}" || exit_code="$?"
    if [[ "$exit_code" == "0" ]]; then
      printf '%s\n' "$run_dir" >>"$completed_file"
    else
      printf '%s\t%s\t%s\t%s\n' "$run_dir" "$rate" "$repeat" "$exit_code" >>"$failures_file"
    fi
  done
done

if [[ "$dry_run_order" == "true" ]]; then
  cat "$root/planned_order.tsv"
  exit 0
fi

python3 "$script_dir/summarize.py" "$root" >"$root/summary.txt" 2>"$root/summary.err" || true
cat "$root/summary.txt"
