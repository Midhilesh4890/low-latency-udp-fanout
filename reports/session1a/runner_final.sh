set -euo pipefail
REGION=us-east-1
AZ=us-east-1a
TYPE=m7i.2xlarge
KEY=spectral-key
KEY_PATH=~/.ssh/spectral-key.pem
PREFIX=spectral-ec2-pass
CLUSTER_PG=spectral-ec2-pass-cluster
KNOWN=/tmp/session1a_final_known_hosts
ISO_DEFAULT=1,5,2,6,3,7
CPU_A=1
CPU_B=2
BASE_PORT=9000
CLOCK_PORT=9100
LOG_CLOCK=reports/session1a/03_clock_probe.log
LOG_RUN=reports/session1a/04_orchestration.log
LOG_TRAP=reports/session1a/05_trap1.log
mkdir -p reports/session1a/env
: > "$LOG_CLOCK"
: > "$LOG_RUN"
: > "$LOG_TRAP"
created_instances=()
runlog() {
  local file="$1"
  shift
  printf '\n[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$file"
}
cleanup() {
  if ((${#created_instances[@]})); then
    runlog "$LOG_RUN" "cleanup terminate ${created_instances[*]}"
    aws ec2 terminate-instances --region "$REGION" --instance-ids "${created_instances[@]}" >>"$LOG_RUN" 2>&1 || true
  fi
}
trap cleanup EXIT
ssh_opts=(-i "$KEY_PATH" -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$KNOWN" -o ConnectTimeout=5)
remote() {
  local ip="$1"
  shift
  ssh "${ssh_opts[@]}" "ubuntu@$ip" "$@"
}
wait_ssh() {
  local ip="$1"
  local label="$2"
  for i in $(seq 1 120); do
    if remote "$ip" true >/dev/null 2>&1; then
      sleep 3
      if remote "$ip" true >/dev/null 2>&1; then
        runlog "$LOG_RUN" "ssh ready $label $ip attempt=$i"
        return 0
      fi
    fi
    sleep 5
  done
  runlog "$LOG_RUN" "ssh failed $label $ip"
  return 1
}
wait_remote() {
  local ip="$1"
  local timeout_s="$2"
  local label="$3"
  local command="$4"
  local end=$((SECONDS + timeout_s))
  while (( SECONDS < end )); do
    if remote "$ip" "$command" >/dev/null 2>&1; then
      runlog "$LOG_CLOCK" "confirmed $label"
      return 0
    fi
    sleep 0.1
  done
  runlog "$LOG_CLOCK" "confirmation failed $label"
  return 1
}
field() {
  aws ec2 describe-instances --region "$REGION" --instance-ids "$1" --query "$2" --output text
}
ensure_common() {
  runlog "$LOG_RUN" "resolve common AWS resources"
  AMI="$(aws ssm get-parameter --region "$REGION" --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id --query Parameter.Value --output text)"
  VPC="$(aws ec2 describe-vpcs --region "$REGION" --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text)"
  SUBNET="$(aws ec2 describe-subnets --region "$REGION" --filters Name=vpc-id,Values="$VPC" Name=availability-zone,Values="$AZ" --query 'Subnets[0].SubnetId' --output text)"
  CIDR="$(curl -fsS https://checkip.amazonaws.com | tr -d '\n')/32"
  SG="$(aws ec2 create-security-group --region "$REGION" --group-name "${PREFIX}-sg" --description "$PREFIX" --vpc-id "$VPC" --query GroupId --output text 2>/dev/null || aws ec2 describe-security-groups --region "$REGION" --filters Name=group-name,Values="${PREFIX}-sg" Name=vpc-id,Values="$VPC" --query 'SecurityGroups[0].GroupId' --output text)"
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=tcp,FromPort=22,ToPort=22,IpRanges=[{CidrIp=$CIDR,Description=ssh-access}]" >>"$LOG_RUN" 2>&1 || true
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=udp,FromPort=0,ToPort=65535,UserIdGroupPairs=[{GroupId=$SG,Description=spectral-udp}]" >>"$LOG_RUN" 2>&1 || true
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=icmp,FromPort=-1,ToPort=-1,UserIdGroupPairs=[{GroupId=$SG,Description=spectral-icmp}]" >>"$LOG_RUN" 2>&1 || true
  aws ec2 describe-placement-groups --region "$REGION" --group-names "$CLUSTER_PG" >>"$LOG_RUN" 2>&1 || aws ec2 create-placement-group --region "$REGION" --group-name "$CLUSTER_PG" --strategy cluster >>"$LOG_RUN" 2>&1
  runlog "$LOG_RUN" "ami=$AMI subnet=$SUBNET sg=$SG"
}
launch_pair() {
  local user_data
  user_data="$(mktemp)"
  printf '%s\n' '#!/bin/sh' 'shutdown -h +180' >"$user_data"
  runlog "$LOG_RUN" "launch final pair"
  ids_text="$(aws ec2 run-instances --region "$REGION" --image-id "$AMI" --instance-type "$TYPE" --key-name "$KEY" --subnet-id "$SUBNET" --security-group-ids "$SG" --placement "AvailabilityZone=$AZ,GroupName=$CLUSTER_PG" --instance-initiated-shutdown-behavior terminate --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true}' --associate-public-ip-address --user-data "file://$user_data" --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=${PREFIX}-final},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a},{Key=Case,Value=final}]" "ResourceType=volume,Tags=[{Key=Name,Value=${PREFIX}-final},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a}]" --count 2 --query 'Instances[].InstanceId' --output text)"
  read -r -a ids <<< "$ids_text"
  rm -f "$user_data"
  TX_ID="${ids[0]}"
  RX_ID="${ids[1]}"
  created_instances+=("$TX_ID" "$RX_ID")
  aws ec2 wait instance-running --region "$REGION" --instance-ids "$TX_ID" "$RX_ID"
  TX_PUBLIC="$(field "$TX_ID" 'Reservations[0].Instances[0].PublicIpAddress')"
  RX_PUBLIC="$(field "$RX_ID" 'Reservations[0].Instances[0].PublicIpAddress')"
  TX_PRIVATE="$(field "$TX_ID" 'Reservations[0].Instances[0].PrivateIpAddress')"
  RX_PRIVATE="$(field "$RX_ID" 'Reservations[0].Instances[0].PrivateIpAddress')"
  runlog "$LOG_RUN" "tx=$TX_ID public=$TX_PUBLIC private=$TX_PRIVATE rx=$RX_ID public=$RX_PUBLIC private=$RX_PRIVATE"
}
bootstrap_host() {
  local ip="$1"
  local label="$2"
  wait_ssh "$ip" "$label"
  runlog "$LOG_RUN" "sync repo $label"
  rsync -az --exclude .git --exclude benchmark/results --exclude reports -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" ./ "ubuntu@$ip:~/task/" >>"$LOG_RUN" 2>&1
  runlog "$LOG_RUN" "bootstrap first pass $label"
  remote "$ip" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG_RUN" 2>&1 || true
  sleep 10
  aws ec2 wait instance-status-ok --region "$REGION" --instance-ids "$(if [[ "$label" == tx ]]; then printf '%s' "$TX_ID"; else printf '%s' "$RX_ID"; fi)" || true
  wait_ssh "$ip" "$label-after-reboot"
  runlog "$LOG_RUN" "bootstrap second pass $label"
  for boot_try in $(seq 1 8); do
    if remote "$ip" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG_RUN" 2>&1; then
      runlog "$LOG_RUN" "bootstrap second pass ok $label attempt=$boot_try"
      break
    fi
    runlog "$LOG_RUN" "bootstrap second pass retry $label attempt=$boot_try"
    sleep 10
    wait_ssh "$ip" "$label-bootstrap-retry-$boot_try"
    if [[ "$boot_try" == "8" ]]; then
      return 1
    fi
  done
  sleep 10
  wait_ssh "$ip" "$label-post-bootstrap"
}
build_host() {
  local ip="$1"
  local label="$2"
  runlog "$LOG_RUN" "build/test $label"
  remote "$ip" 'cd ~/task && make -C harness clean && make -C harness && make -C harness test && g++ -std=c++17 -O2 -Wall -Wextra -I. tools/clock_probe.cpp -o tools/clock_probe -lrt -lpthread' >>"$LOG_RUN" 2>&1
}
collect_env() {
  local ip="$1"
  rsync -az -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" "ubuntu@$ip:~/task/env/" reports/session1a/env/ >>"$LOG_RUN" 2>&1 || true
}
chrony_120() {
  local ip="$1"
  local label="$2"
  runlog "$LOG_CLOCK" "chrony 120s $label"
  remote "$ip" 'cd ~/task && h=$(hostname); mkdir -p env/$h; for i in $(seq 1 120); do date -u +%Y-%m-%dT%H:%M:%SZ; chronyc tracking; sleep 1; done > env/$h/chrony_tracking_120s.txt' >>"$LOG_CLOCK" 2>&1
}
run_clock_probe() {
  ISOLATED="$(remote "$TX_PUBLIC" 'cd ~/task && h=$(hostname); sed -n s/^chosen=//p env/$h/thread_sibling_choice.txt 2>/dev/null || true' | tr -d '\r')"
  if [[ -z "$ISOLATED" ]]; then
    ISOLATED="$ISO_DEFAULT"
  fi
  TX_HOSTNAME="$(remote "$TX_PUBLIC" hostname | tr -d '\r')"
  RX_HOSTNAME="$(remote "$RX_PUBLIC" hostname | tr -d '\r')"
  NIC_DRIVER_VERSION="$(remote "$TX_PUBLIC" 'modinfo ena 2>/dev/null | awk "/^version:/{print \$2; exit}" || true' | tr -d '\r')"
  runlog "$LOG_CLOCK" "tx_hostname=$TX_HOSTNAME rx_hostname=$RX_HOSTNAME isolated=$ISOLATED nic_driver=$NIC_DRIVER_VERSION"
  remote "$TX_PUBLIC" "cd ~/task && bash benchmark/preflight_isolation.sh --cores '$CPU_A' --isolated-cores '$ISOLATED' --label clock-tx" >>"$LOG_CLOCK" 2>&1
  remote "$RX_PUBLIC" "cd ~/task && bash benchmark/preflight_isolation.sh --cores '$CPU_A' --isolated-cores '$ISOLATED' --label clock-rx" >>"$LOG_CLOCK" 2>&1
  remote "$RX_PUBLIC" "cd ~/task && rm -f /tmp/clock_probe.pid /tmp/clock_probe.log && (nohup taskset -c '$CPU_A' tools/clock_probe server '$CLOCK_PORT' < /dev/null >/tmp/clock_probe.log 2>&1 & echo \$! >/tmp/clock_probe.pid)" >>"$LOG_CLOCK" 2>&1
  wait_remote "$RX_PUBLIC" 10 "clock probe udp bind" "ss -H -lun | grep -q ':$CLOCK_PORT'"
  CLOCK_OUTPUT="$(remote "$TX_PUBLIC" "cd ~/task && taskset -c '$CPU_A' tools/clock_probe client '$RX_PRIVATE' '$CLOCK_PORT' 100000" 2>>"$LOG_CLOCK")"
  printf '%s\n' "$CLOCK_OUTPUT" | tee -a "$LOG_CLOCK"
  remote "$RX_PUBLIC" "if [[ -s /tmp/clock_probe.pid ]]; then kill \$(cat /tmp/clock_probe.pid) 2>/dev/null || true; fi" >>"$LOG_CLOCK" 2>&1 || true
  CLOCK_OFFSET="$(printf '%s\n' "$CLOCK_OUTPUT" | sed -n 's/.*min_rtt_offset_ns=\([-0-9]*\).*/\1/p')"
  CLOCK_BOUND="${CLOCK_OFFSET#-}"
  if [[ -z "$CLOCK_BOUND" ]]; then
    CLOCK_BOUND=0
  fi
  printf '%s\n' "clock_residual_bound_ns=$CLOCK_BOUND" | tee -a "$LOG_CLOCK"
}
mtu_sweep() {
  runlog "$LOG_CLOCK" "path mtu df sweep tx_to_rx $RX_PRIVATE"
  local lo=0
  local hi=8973
  local mid
  while (( lo < hi )); do
    mid=$(((lo + hi + 1) / 2))
    if remote "$TX_PUBLIC" "ping -M do -s '$mid' -c 2 -W 1 '$RX_PRIVATE'" >>"$LOG_CLOCK" 2>&1; then
      lo="$mid"
    else
      hi=$((mid - 1))
    fi
  done
  MTU=$((lo + 28))
  printf '%s\n' "path_mtu_payload=$lo path_mtu=$MTU" | tee -a "$LOG_CLOCK"
}
run_benchmark() {
  local label="$1"
  local latency_mode="$2"
  local port="$3"
  local out="$4"
  local log_file="$5"
  runlog "$log_file" "run $label latency_output=$latency_mode out=$out"
  bash benchmark/run_remote.sh --tx-host "$TX_PUBLIC" --rx-hosts "$RX_PUBLIC" --rx-privates "$RX_PRIVATE" --ssh-key "$KEY_PATH" --outdir "$out" --rate 300000 --count 300000 --warmup 20000 --slots 65536 --base-port "$port" --fanout 1 --cpu-producer "$CPU_A" --cpu-sender "$CPU_B" --cpu-receiver "$CPU_A" --cpu-consumer "$CPU_B" --isolated-cores "$ISOLATED" --clock-method chrony_ntp_clock_probe --clock-residual-bound-ns "$CLOCK_BOUND" --instance-type "$TYPE" --az "$AZ" --placement-group-type cluster --nic-driver-version "$NIC_DRIVER_VERSION" --mtu "$MTU" --run-order-index "$port" --not-p9999-grade --run-label "$label" --latency-output "$latency_mode" --no-build >>"$log_file" 2>&1
}
summarize_run() {
  local out="$1"
  local log_file="$2"
  if find "$out" -path '*/latency.csv' -type f | grep -q .; then
    python3 benchmark/summarize.py "$out" | tee -a "$log_file"
  fi
}
parse_no_write() {
  local out="$1"
  local log_file="$2"
  printf '%s\n' "no_write_consumer_metrics" | tee -a "$log_file"
  for f in "$out"/rx_*/consumer.log; do
    printf '%s\n' "$f" | tee -a "$log_file"
    sed -n '/---- delivery metrics ----/,$p' "$f" | tee -a "$log_file"
  done
}
ensure_common
launch_pair
bootstrap_host "$TX_PUBLIC" tx &
pid_tx=$!
bootstrap_host "$RX_PUBLIC" rx &
pid_rx=$!
wait "$pid_tx"
wait "$pid_rx"
build_host "$TX_PUBLIC" tx &
build_tx=$!
build_host "$RX_PUBLIC" rx &
build_rx=$!
wait "$build_tx"
wait "$build_rx"
chrony_120 "$TX_PUBLIC" tx &
chrony_tx=$!
chrony_120 "$RX_PUBLIC" rx &
chrony_rx=$!
wait "$chrony_tx"
wait "$chrony_rx"
collect_env "$TX_PUBLIC"
collect_env "$RX_PUBLIC"
run_clock_probe
mtu_sweep
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SMOKE_OUT="benchmark/results/session1a_${STAMP}_smoke_disk"
run_benchmark smoke disk "$BASE_PORT" "$SMOKE_OUT" "$LOG_RUN"
summarize_run "$SMOKE_OUT" "$LOG_RUN"
if grep -q CLOCK_INVALID "$LOG_RUN"; then
  runlog "$LOG_RUN" "smoke summary contained CLOCK_INVALID"
  exit 1
fi
DISK_OUT="benchmark/results/session1a_${STAMP}_trap1_disk"
TMPFS_OUT="benchmark/results/session1a_${STAMP}_trap1_tmpfs"
NONE_OUT="benchmark/results/session1a_${STAMP}_trap1_none"
run_benchmark trap1_disk disk 9010 "$DISK_OUT" "$LOG_TRAP"
summarize_run "$DISK_OUT" "$LOG_TRAP"
run_benchmark trap1_tmpfs tmpfs 9020 "$TMPFS_OUT" "$LOG_TRAP"
summarize_run "$TMPFS_OUT" "$LOG_TRAP"
run_benchmark trap1_none none 9030 "$NONE_OUT" "$LOG_TRAP"
parse_no_write "$NONE_OUT" "$LOG_TRAP"
collect_env "$TX_PUBLIC"
collect_env "$RX_PUBLIC"
printf '%s\n' "tx_id=$TX_ID rx_id=$RX_ID tx_public=$TX_PUBLIC rx_public=$RX_PUBLIC tx_private=$TX_PRIVATE rx_private=$RX_PRIVATE isolated=$ISOLATED mtu=$MTU clock_bound=$CLOCK_BOUND smoke_out=$SMOKE_OUT disk_out=$DISK_OUT tmpfs_out=$TMPFS_OUT none_out=$NONE_OUT" > reports/session1a/final_state.env
runlog "$LOG_RUN" "terminate final pair $TX_ID $RX_ID"
aws ec2 terminate-instances --region "$REGION" --instance-ids "$TX_ID" "$RX_ID" >>"$LOG_RUN" 2>&1 || true
aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$TX_ID" "$RX_ID" || true
created_instances=()
trap - EXIT