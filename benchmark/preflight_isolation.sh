set -euo pipefail

cores=""
label="benchmark"

usage() {
  printf '%s\n' "usage: benchmark/preflight_isolation.sh --cores LIST [--label NAME]"
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
    --label) require_value "$@"; label="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$cores" ]]; then
  usage >&2
  exit 2
fi

IFS=',' read -r -a core_values <<< "$cores"
declare -A seen=()
clean_cores=()
for core in "${core_values[@]}"; do
  if [[ -z "$core" ]]; then
    continue
  fi
  case "$core" in
    *[!0-9]*) printf '%s\n' "preflight failed: nonnumeric core $core for $label" >&2; exit 1 ;;
  esac
  if [[ -n "${seen[$core]+x}" ]]; then
    printf '%s\n' "preflight failed: duplicate benchmark core $core for $label" >&2
    exit 1
  fi
  seen[$core]=1
  clean_cores+=("$core")
done

if [[ "${#clean_cores[@]}" == "0" ]]; then
  printf '%s\n' "preflight failed: empty core list for $label" >&2
  exit 1
fi

contains_core() {
  local list="$1"
  local needle="$2"
  IFS=',' read -r -a chunks <<< "$list"
  for chunk in "${chunks[@]}"; do
    if [[ "$chunk" == *-* ]]; then
      local first="${chunk%-*}"
      local last="${chunk#*-}"
      if (( needle >= first && needle <= last )); then
        return 0
      fi
    elif [[ "$chunk" == "$needle" ]]; then
      return 0
    fi
  done
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
    for core in "${clean_cores[@]}"; do
      if contains_core "$affinity" "$core"; then
        printf '%s\n' "preflight failed: stray $name pid=$pid can run on isolated core $core for $label" >&2
        exit 1
      fi
    done
  done < <(pgrep -x "$name" || true)
done

while read -r pid psr stat comm; do
  if [[ -z "$pid" || "$pid" == "$$" || "$comm" == "ps" ]]; then
    continue
  fi
  case "$stat" in
    R*)
      for core in "${clean_cores[@]}"; do
        if [[ "$psr" == "$core" ]]; then
          printf '%s\n' "preflight failed: runnable task pid=$pid comm=$comm already on core $core for $label" >&2
          exit 1
        fi
      done
      ;;
  esac
done < <(ps -eLo pid=,psr=,stat=,comm=)

printf '%s\n' "preflight ok: $label cores=${clean_cores[*]}"