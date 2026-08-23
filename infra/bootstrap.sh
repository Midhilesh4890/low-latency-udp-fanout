set -euo pipefail

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential chrony linuxptp ethtool numactl iproute2 python3-pip jq git linux-tools-common
host="$(hostname)"
base="${1:-env/$host}"
mkdir -p "$base"

capture() {
  dir="$1"
  mkdir -p "$dir"
  uname -a >"$dir/uname.txt" 2>&1 || true
  lscpu >"$dir/lscpu.txt" 2>&1 || true
  lscpu -e >"$dir/lscpu_e.txt" 2>&1 || true
  ethtool -i ens5 >"$dir/ethtool_i_ens5.txt" 2>&1 || true
  ethtool -T ens5 >"$dir/ethtool_T_ens5.txt" 2>&1 || true
  ethtool -g ens5 >"$dir/ethtool_g_ens5.txt" 2>&1 || true
  ethtool -c ens5 >"$dir/ethtool_c_ens5.txt" 2>&1 || true
  ip -d link show ens5 >"$dir/ip_d_link_ens5.txt" 2>&1 || true
  cat /proc/cmdline >"$dir/proc_cmdline.txt" 2>&1 || true
  sysctl -a 2>/dev/null | grep -E 'net.core|net.ipv4.udp' >"$dir/sysctl_net.txt" 2>&1 || true
  ls -la /dev/ptp* >"$dir/dev_ptp.txt" 2>&1 || true
  modinfo ena 2>&1 | head -20 >"$dir/modinfo_ena_head.txt" || true
  cat /proc/interrupts >"$dir/proc_interrupts.txt" 2>&1 || true
  find /sys/devices/system/cpu -name thread_siblings_list -print -exec cat {} \; >"$dir/thread_siblings.txt" 2>&1 || true
  chronyc sources -v >"$dir/chronyc_sources.txt" 2>&1 || true
  chronyc tracking >"$dir/chronyc_tracking.txt" 2>&1 || true
  tc qdisc show >"$dir/tc_qdisc_show.txt" 2>&1 || true
}

capture "$base/pre"

sudo sysctl -w net.core.rmem_max=67108864
sudo sysctl -w net.core.wmem_max=67108864
sudo sysctl -w net.core.netdev_max_backlog=250000
sudo systemctl stop irqbalance 2>/dev/null || true
sudo systemctl disable irqbalance 2>/dev/null || true

map_file="$base/thread_sibling_choice.txt"
python3 - <<'PY' >"$map_file"
from pathlib import Path
pairs=[]
for p in sorted(Path('/sys/devices/system/cpu').glob('cpu[0-9]*')):
    q=p / 'topology' / 'thread_siblings_list'
    if not q.exists():
        continue
    cpu=int(p.name[3:])
    text=q.read_text().strip()
    first=text.split(',')[0].split('-')[0]
    pairs.append((cpu,text,int(first)))
seen=set()
chosen=[]
for cpu,text,first in pairs:
    if first in seen:
        continue
    seen.add(first)
    if cpu != 0:
        chosen.append(cpu)
    if len(chosen) == 4:
        break
print('map=' + ';'.join(f'{cpu}:{text}' for cpu,text,_ in pairs))
print('chosen=' + ','.join(str(x) for x in chosen))
PY
isolated="$(sed -n 's@^chosen=@@p' "$map_file")"
if [[ -n "$isolated" ]] && ! grep -q "isolcpus=$isolated" /proc/cmdline; then
  sudo sed -i "s/GRUB_CMDLINE_LINUX=\"/GRUB_CMDLINE_LINUX=\"isolcpus=$isolated nohz_full=$isolated rcu_nocbs=$isolated /" /etc/default/grub
  sudo update-grub
  touch "$base/reboot_required"
fi

capture "$base/post_tuning_pre_reboot"

if [[ -e "$base/reboot_required" ]]; then
  sudo reboot
fi