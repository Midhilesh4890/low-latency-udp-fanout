#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
declare -A group_members=()
group_order=()

for cpu_path in /sys/devices/system/cpu/cpu[0-9]*; do
  cpu="${cpu_path##*cpu}"
  package="$(<"$cpu_path/topology/physical_package_id")"
  core="$(<"$cpu_path/topology/core_id")"
  key="$package:$core"
  if [[ -z "${group_members[$key]+x}" ]]; then
    group_order+=("$key")
    group_members[$key]="$cpu"
  else
    group_members[$key]="${group_members[$key]},$cpu"
  fi
done

if (( ${#group_order[@]} < 3 )); then
  printf '%s\n' "preflight test skipped: fewer than three physical cores"
  exit 0
fi

first_key="${group_order[0]}"
IFS=',' read -r first_cpu first_sibling _ <<< "${group_members[$first_key]}"
if [[ -z "${first_sibling:-}" ]]; then
  printf '%s\n' "preflight test skipped: no SMT sibling pair"
  exit 0
fi

second_key="${group_order[1]}"
third_key="${group_order[2]}"
IFS=',' read -r second_cpu _ <<< "${group_members[$second_key]}"
IFS=',' read -r third_cpu _ <<< "${group_members[$third_key]}"
isolated="${group_members[$first_key]},${group_members[$second_key]},${group_members[$third_key]}"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

set +e
bash "$repo_root/benchmark/preflight_isolation.sh" --cores "$first_cpu,$first_sibling" --isolated-cores "$isolated" --label direct >"$tmp/direct.log" 2>&1
direct_status="$?"
set -e
if (( direct_status == 0 )) || ! grep -q "share physical core $first_key for direct" "$tmp/direct.log"; then
  cat "$tmp/direct.log" >&2
  printf '%s\n' "direct physical-core rejection test failed" >&2
  exit 1
fi

printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' 'command_text="${!#}"' 'if [[ "$command_text" == *benchmark/preflight_isolation.sh* ]]; then' '  bash -lc "$command_text"' 'fi' >"$tmp/ssh"
chmod +x "$tmp/ssh"

set +e
PATH="$tmp:$PATH" bash "$repo_root/benchmark/run_remote.sh" \
  --no-build \
  --tx-host test-host \
  --rx-hosts test-host \
  --rx-privates 127.0.0.1 \
  --remote-repo "$repo_root" \
  --cpu-producer "$first_cpu" \
  --cpu-sender "$second_cpu" \
  --cpu-receiver "$third_cpu" \
  --cpu-consumer "$first_sibling" \
  --isolated-cores "$isolated" \
  --outdir "$tmp/out" >"$tmp/aggregate.log" 2>&1
aggregate_status="$?"
set -e
if (( aggregate_status == 0 )) || ! grep -q "share physical core $first_key for tx+rx" "$tmp/aggregate.log"; then
  cat "$tmp/aggregate.log" >&2
  printf '%s\n' "same-host aggregate rejection test failed" >&2
  exit 1
fi

printf '%s\n' "preflight physical-core tests passed"
