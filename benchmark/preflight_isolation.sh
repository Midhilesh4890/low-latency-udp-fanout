set -euo pipefail

cores=""
isolated_cores=""
label="benchmark"

usage() {
  printf '%s\n' "usage: benchmark/preflight_isolation.sh --cores LIST [--isolated-cores LIST] [--label NAME]"
}

require_value() {
  if [[ -z "${2+x}" ]]; then
    printf '%s\n' "missing value for $1" >&2
    exit 2
  fi
}

while [[ -n "${1+x}" ]]; do
  case "$1" in
    --cores) require_value "$@"; cores="$2"; shift 2 ;;
    --isolated-cores) require_value "$@"; isolated_cores="$2"; shift 2 ;;
    --label) require_value "$@"; label="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$cores" ]]; then
  usage >&2
  exit 2
fi

expand_list() {
  local list="$1"
  local chunk first last value
  IFS=',' read -r -a parts <<< "$list"
  for chunk in "${parts[@]}"; do
    if [[ -z "$chunk" ]]; then
      continue
    fi
    case "$chunk" in
      *-*)
        first="${chunk%-*}"
        last="${chunk#*-}"
        case "$first$last" in
          *[!0-9]*) printf '%s\n' "preflight failed: nonnumeric core range $chunk for $label" >&2; exit 1 ;;
        esac
        if (( first > last )); then
          printf '%s\n' "preflight failed: descending core range $chunk for $label" >&2
          exit 1
        fi
        for ((value=first; value<=last; ++value)); do
          printf '%s\n' "$value"
        done
        ;;
      *)
        case "$chunk" in
          *[!0-9]*) printf '%s\n' "preflight failed: nonnumeric core $chunk for $label" >&2; exit 1 ;;
        esac
        printf '%s\n' "$chunk"
        ;;
    esac
  done
}

core_key() {
  local core="$1"
  local topo="/sys/devices/system/cpu/cpu$core/topology"
  if [[ -r "$topo/physical_package_id" && -r "$topo/core_id" ]]; then
    printf '%s:%s\n' "$(cat "$topo/physical_package_id")" "$(cat "$topo/core_id")"
  elif [[ -r "$topo/thread_siblings_list" ]]; then
    printf 'siblings:%s\n' "$(cat "$topo/thread_siblings_list")"
  else
    printf 'cpu:%s\n' "$core"
  fi
}

sibling_cores() {
  local core="$1"
  local topo="/sys/devices/system/cpu/cpu$core/topology/thread_siblings_list"
  if [[ -r "$topo" ]]; then
    expand_list "$(cat "$topo")"
  else
    printf '%s\n' "$core"
  fi
}

mapfile -t clean_cores < <(expand_list "$cores")
declare -A seen=()
declare -A physical_seen=()
declare -A assigned_keys=()
unique_cores=()
for core in "${clean_cores[@]}"; do
  if [[ -n "${seen[$core]+x}" ]]; then
    printf '%s\n' "preflight failed: duplicate benchmark core $core for $label" >&2
    exit 1
  fi
  if [[ ! -d "/sys/devices/system/cpu/cpu$core" ]]; then
    printf '%s\n' "preflight failed: core $core does not exist for $label" >&2
    exit 1
  fi
  seen[$core]=1
  key="$(core_key "$core")"
  if [[ -n "${physical_seen[$key]+x}" ]]; then
    printf '%s\n' "preflight failed: benchmark cores $core and ${physical_seen[$key]} share physical core $key for $label" >&2
    exit 1
  fi
  physical_seen[$key]="$core"
  assigned_keys[$key]=1
  unique_cores+=("$core")
done

if [[ "${#unique_cores[@]}" == "0" ]]; then
  printf '%s\n' "preflight failed: empty core list for $label" >&2
  exit 1
fi

declare -A isolated_set=()
if [[ -n "$isolated_cores" ]]; then
  mapfile -t isolated_values < <(expand_list "$isolated_cores")
  for core in "${isolated_values[@]}"; do
    isolated_set[$core]=1
  done
fi

smt_state="unknown"
if [[ -r /sys/devices/system/cpu/smt/control ]]; then
  smt_state="$(cat /sys/devices/system/cpu/smt/control)"
fi
if [[ "$smt_state" == "on" || "$smt_state" == "active" ]]; then
  if [[ -z "$isolated_cores" ]]; then
    printf '%s\n' "preflight failed: SMT is $smt_state and --isolated-cores was not provided for $label" >&2
    exit 1
  fi
  for core in "${unique_cores[@]}"; do
    while read -r sibling; do
      if [[ -z "${isolated_set[$sibling]+x}" ]]; then
        printf '%s\n' "preflight failed: SMT is $smt_state and sibling core $sibling for benchmark core $core is not isolated for $label" >&2
        exit 1
      fi
    done < <(sibling_cores "$core")
  done
fi

affinity_has_assigned_physical() {
  local affinity="$1"
  local value key
  while read -r value; do
    if [[ ! -d "/sys/devices/system/cpu/cpu$value" ]]; then
      continue
    fi
    key="$(core_key "$value")"
    if [[ -n "${assigned_keys[$key]+x}" ]]; then
      return 0
    fi
  done < <(expand_list "$affinity")
  return 1
}

for name in producer sender receiver consumer clock_probe; do
  while read -r pid; do
    if [[ -z "$pid" || "$pid" == "$$" ]]; then
      continue
    fi
    affinity="$(taskset -pc "$pid" 2>/dev/null | awk -F': ' '{print $2}' || true)"
    if [[ -z "$affinity" ]]; then
      continue
    fi
    if affinity_has_assigned_physical "$affinity"; then
      printf '%s\n' "preflight failed: stray $name pid=$pid can run on benchmark physical core for $label" >&2
      exit 1
    fi
  done < <(pgrep -x "$name" || true)
done

while read -r pid psr stat comm; do
  if [[ -z "$pid" || "$pid" == "$$" || "$comm" == "ps" ]]; then
    continue
  fi
  case "$stat" in
    R*)
      case "$psr" in
        *[!0-9]*|'') continue ;;
      esac
      if [[ -d "/sys/devices/system/cpu/cpu$psr" ]]; then
        key="$(core_key "$psr")"
        if [[ -n "${assigned_keys[$key]+x}" ]]; then
          printf '%s\n' "preflight failed: runnable task pid=$pid comm=$comm already on benchmark physical core $key for $label" >&2
          exit 1
        fi
      fi
      ;;
  esac
done < <(ps -eLo pid=,psr=,stat=,comm=)

printf '%s\n' "preflight ok: $label cores=${unique_cores[*]} isolated_cores=${isolated_cores:-unset} smt=$smt_state"