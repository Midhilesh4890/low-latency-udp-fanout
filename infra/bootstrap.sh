set -euo pipefail

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential chrony linuxptp ethtool numactl iproute2 python3-pip jq git linux-tools-common
host="$(hostname)"
base="${1:-env/$host}"
mkdir -p "$base"

primary_nic() {
  while read -r name; do
    if [[ "$name" != "lo" ]]; then
      printf '%s\n' "$name"
      return 0
    fi
  done < <(find /sys/class/net -mindepth 1 -maxdepth 1 -printf '%f\n')
  printf '%s\n' ens5
}

capture() {
  dir="$1"
  nic="$(primary_nic)"
  mkdir -p "$dir"
  printf '%s\n' "$nic" >"$dir/nic.txt" 2>&1 || true
  uname -a >"$dir/uname.txt" 2>&1 || true
  lscpu >"$dir/lscpu.txt" 2>&1 || true
  lscpu -e >"$dir/lscpu_e.txt" 2>&1 || true
  ethtool -i "$nic" >"$dir/ethtool_i_${nic}.txt" 2>&1 || true
  ethtool -T "$nic" >"$dir/ethtool_T_${nic}.txt" 2>&1 || true
  ethtool -g "$nic" >"$dir/ethtool_g_${nic}.txt" 2>&1 || true
  ethtool -c "$nic" >"$dir/ethtool_c_${nic}.txt" 2>&1 || true
  ip -d link show "$nic" >"$dir/ip_d_link_${nic}.txt" 2>&1 || true
  cat /proc/cmdline >"$dir/proc_cmdline.txt" 2>&1 || true
  sysctl -a 2>/dev/null | grep -E 'net.core|net.ipv4.udp' >"$dir/sysctl_net.txt" 2>&1 || true
  ls -la /dev/ptp* >"$dir/dev_ptp.txt" 2>&1 || true
  modinfo ena 2>&1 | head -20 >"$dir/modinfo_ena_head.txt" || true
  cat /proc/interrupts >"$dir/proc_interrupts.txt" 2>&1 || true
  find /sys/devices/system/cpu -name thread_siblings_list -print -exec cat {} \; >"$dir/thread_siblings.txt" 2>&1 || true
  cat /sys/devices/system/cpu/smt/control >"$dir/smt_control.txt" 2>&1 || true
  chronyc sources -v >"$dir/chronyc_sources.txt" 2>&1 || true
  chronyc tracking >"$dir/chronyc_tracking.txt" 2>&1 || true
  tc qdisc show >"$dir/tc_qdisc_show.txt" 2>&1 || true
  while read -r f; do printf '%s ' "$f"; cat "$f"; done < <(find /proc/irq -mindepth 2 -maxdepth 2 -name smp_affinity_list) >"$dir/irq_affinity_list.txt" 2>&1 || true
}

capture "$base/pre"

sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
sudo sysctl -w net.core.netdev_max_backlog=250000
sudo systemctl stop irqbalance 2>/dev/null || true
sudo systemctl disable irqbalance 2>/dev/null || true

smt_state="unknown"
if [[ -w /sys/devices/system/cpu/smt/control ]]; then
  if printf '%s\n' off | sudo tee /sys/devices/system/cpu/smt/control >/dev/null 2>&1; then
    smt_state="$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || printf unknown)"
  else
    smt_state="$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || printf unavailable)"
  fi
else
  smt_state="$(cat /sys/devices/system/cpu/smt/control 2>/dev/null || printf unavailable)"
fi
printf '%s\n' "$smt_state" >"$base/smt_choice.txt"

map_file="$base/thread_sibling_choice.txt"
python3 - "$smt_state" <<'PY' >"$map_file"
from pathlib import Path
import sys
smt_state=sys.argv[1]
pairs=[]
for p in sorted(Path('/sys/devices/system/cpu').glob('cpu[0-9]*'), key=lambda x: int(x.name[3:])):
    q=p / 'topology' / 'thread_siblings_list'
    if not q.exists():
        continue
    cpu=int(p.name[3:])
    text=q.read_text().strip()
    first=text.split(',')[0].split('-')[0]
    pairs.append((cpu,text,int(first)))
seen=set()
chosen=[]
fallback=[]
for cpu,text,first in pairs:
    if first in seen:
        continue
    seen.add(first)
    members=[]
    for part in text.split(','):
        if '-' in part:
            a,b=part.split('-',1)
            members.extend(range(int(a), int(b)+1))
        else:
            members.append(int(part))
    if 0 in members:
        continue
    if smt_state in ('off','forceoff','notsupported'):
        chosen.append(str(min(members)))
    else:
        fallback.extend(str(x) for x in members)
    if len(chosen) >= 3 or len(set(fallback)) >= 6:
        break
print('map=' + ';'.join(f'{cpu}:{text}' for cpu,text,_ in pairs))
if chosen:
    print('chosen=' + ','.join(chosen[:3]))
    print('smt_path=nosmt')
else:
    ordered=[]
    for value in fallback:
        if value not in ordered:
            ordered.append(value)
    print('chosen=' + ','.join(ordered))
    print('smt_path=isolate_siblings')
PY
isolated="$(sed -n 's@^chosen=@@p' "$map_file")"
smt_path="$(sed -n 's@^smt_path=@@p' "$map_file")"
cmdline_tokens="isolcpus=$isolated nohz_full=$isolated rcu_nocbs=$isolated"
need_cmdline="false"
if [[ -n "$isolated" ]] && ! grep -q "isolcpus=$isolated" /proc/cmdline; then
  need_cmdline="true"
fi
if [[ "$smt_path" == "nosmt" ]]; then
  cmdline_tokens="nosmt $cmdline_tokens"
  if ! grep -qw nosmt /proc/cmdline; then
    need_cmdline="true"
  fi
fi
if [[ "$need_cmdline" == "true" ]]; then
  sudo sed -i "s/GRUB_CMDLINE_LINUX=\"/GRUB_CMDLINE_LINUX=\"$cmdline_tokens /" /etc/default/grub
  sudo update-grub
  touch "$base/reboot_required"
fi

housekeeping="0"
while read -r f; do
  irq="$(basename "$(dirname "$f")")"
  if [[ "$irq" == "0" ]]; then
    continue
  fi
  printf '%s\n' "$housekeeping" | sudo tee "$f" >/dev/null 2>&1 || true
done < <(find /proc/irq -mindepth 2 -maxdepth 2 -name smp_affinity_list)
printf '%s\n' "$housekeeping" >"$base/irq_housekeeping_choice.txt"

capture "$base/post_tuning_pre_reboot"

if [[ -e "$base/reboot_required" ]]; then
  sudo reboot
fi