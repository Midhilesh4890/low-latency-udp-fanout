set -euo pipefail
REGION=us-east-1
AZ=us-east-1a
TYPE=m7i.2xlarge
KEY=spectral-key
KEY_PATH=~/.ssh/spectral-key.pem
PREFIX=spectral-ec2-pass
CLUSTER_PG=spectral-ec2-pass-cluster
PRECISION_PG=spectral-ec2-pass-precision-time-session1a
LOG=reports/session1a/02_clock_phc.log
SUMMARY=reports/session1a/phc_summary.tsv
KNOWN=/tmp/session1a_known_hosts
created_instances=()
mkdir -p reports/session1a/env
if [[ ! -e "$LOG" ]]; then
  : > "$LOG"
fi
printf 'case\tinstance\tpublic\tprivate\thostname\tnic\tptp_count\thw_timestamp\tsmt\tisolated\n' > "$SUMMARY"
runlog() {
  printf '\n[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$LOG"
}
cleanup() {
  if ((${#created_instances[@]})); then
    runlog "cleanup terminate ${created_instances[*]}"
    aws ec2 terminate-instances --region "$REGION" --instance-ids "${created_instances[@]}" >>"$LOG" 2>&1 || true
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
        runlog "ssh ready $label $ip attempt=$i"
        return 0
      fi
    fi
    sleep 5
  done
  runlog "ssh failed $label $ip"
  return 1
}
instance_field() {
  local iid="$1"
  local query="$2"
  aws ec2 describe-instances --region "$REGION" --instance-ids "$iid" --query "$query" --output text
}
ensure_common() {
  runlog "resolve common AWS resources"
  AMI="$(aws ssm get-parameter --region "$REGION" --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id --query Parameter.Value --output text)"
  VPC="$(aws ec2 describe-vpcs --region "$REGION" --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text)"
  SUBNET="$(aws ec2 describe-subnets --region "$REGION" --filters Name=vpc-id,Values="$VPC" Name=availability-zone,Values="$AZ" --query 'Subnets[0].SubnetId' --output text)"
  CIDR="$(curl -fsS https://checkip.amazonaws.com | tr -d '\n')/32"
  SG="$(aws ec2 create-security-group --region "$REGION" --group-name "${PREFIX}-sg" --description "$PREFIX" --vpc-id "$VPC" --query GroupId --output text 2>/dev/null || aws ec2 describe-security-groups --region "$REGION" --filters Name=group-name,Values="${PREFIX}-sg" Name=vpc-id,Values="$VPC" --query 'SecurityGroups[0].GroupId' --output text)"
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=tcp,FromPort=22,ToPort=22,IpRanges=[{CidrIp=$CIDR,Description=ssh-access}]" >>"$LOG" 2>&1 || true
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=udp,FromPort=0,ToPort=65535,UserIdGroupPairs=[{GroupId=$SG,Description=spectral-udp}]" >>"$LOG" 2>&1 || true
  aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=icmp,FromPort=-1,ToPort=-1,UserIdGroupPairs=[{GroupId=$SG,Description=spectral-icmp}]" >>"$LOG" 2>&1 || true
  aws ec2 describe-placement-groups --region "$REGION" --group-names "$CLUSTER_PG" >>"$LOG" 2>&1 || aws ec2 create-placement-group --region "$REGION" --group-name "$CLUSTER_PG" --strategy cluster >>"$LOG" 2>&1
  aws ec2 create-placement-group --region "$REGION" --group-name "$PRECISION_PG" --strategy precision-time >>"$LOG" 2>&1 || true
  PRECISION_PG_ID="$(aws ec2 describe-placement-groups --region "$REGION" --group-names "$PRECISION_PG" --query 'PlacementGroups[0].GroupId' --output text)"
  runlog "ami=$AMI vpc=$VPC subnet=$SUBNET sg=$SG precision_pg_id=$PRECISION_PG_ID"
}
launch_case() {
  local case_name="$1"
  local placement="$2"
  local tag_name="${PREFIX}-${case_name}"
  local user_data
  user_data="$(mktemp)"
  printf '%s\n' '#!/bin/sh' 'shutdown -h +180' >"$user_data"
  runlog "launch $case_name placement=$placement"
  if [[ -n "$placement" ]]; then
    iid="$(aws ec2 run-instances --region "$REGION" --image-id "$AMI" --instance-type "$TYPE" --key-name "$KEY" --subnet-id "$SUBNET" --security-group-ids "$SG" --placement "$placement" --instance-initiated-shutdown-behavior terminate --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true}' --associate-public-ip-address --user-data "file://$user_data" --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$tag_name},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a},{Key=Case,Value=$case_name}]" "ResourceType=volume,Tags=[{Key=Name,Value=$tag_name},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a}]" --query 'Instances[0].InstanceId' --output text)"
  else
    iid="$(aws ec2 run-instances --region "$REGION" --image-id "$AMI" --instance-type "$TYPE" --key-name "$KEY" --subnet-id "$SUBNET" --security-group-ids "$SG" --placement "AvailabilityZone=$AZ" --instance-initiated-shutdown-behavior terminate --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true}' --associate-public-ip-address --user-data "file://$user_data" --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$tag_name},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a},{Key=Case,Value=$case_name}]" "ResourceType=volume,Tags=[{Key=Name,Value=$tag_name},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a}]" --query 'Instances[0].InstanceId' --output text)"
  fi
  rm -f "$user_data"
  created_instances+=("$iid")
  aws ec2 wait instance-running --region "$REGION" --instance-ids "$iid"
  public="$(instance_field "$iid" 'Reservations[0].Instances[0].PublicIpAddress')"
  private="$(instance_field "$iid" 'Reservations[0].Instances[0].PrivateIpAddress')"
  runlog "running $case_name iid=$iid public=$public private=$private"
  wait_ssh "$public" "$case_name"
  runlog "sync repo $case_name"
  rsync -az --exclude .git --exclude benchmark/results --exclude reports -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" ./ "ubuntu@$public:~/task/" >>"$LOG" 2>&1
  runlog "bootstrap first pass $case_name"
  remote "$public" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG" 2>&1 || true
  sleep 10
  aws ec2 wait instance-status-ok --region "$REGION" --instance-ids "$iid" || true
  wait_ssh "$public" "$case_name-after-reboot"
  runlog "bootstrap second pass $case_name"
  for boot_try in $(seq 1 8); do
    if remote "$public" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG" 2>&1; then
      runlog "bootstrap second pass ok $case_name attempt=$boot_try"
      break
    fi
    runlog "bootstrap second pass retry $case_name attempt=$boot_try"
    sleep 10
    wait_ssh "$public" "$case_name-bootstrap-retry-$boot_try"
    if [[ "$boot_try" == "8" ]]; then
      runlog "bootstrap second pass failed $case_name"
      return 1
    fi
  done
  sleep 10
  wait_ssh "$public" "$case_name-post-bootstrap"
  host="$(remote "$public" hostname | tr -d '\r')"
  runlog "collect phc $case_name host=$host"
  remote "$public" 'cd ~/task && h=$(hostname); d=env/$h; nic=$(cat "$d/post_tuning_pre_reboot/nic.txt" 2>/dev/null || cat "$d/pre/nic.txt" 2>/dev/null || ip -o link show | awk -F": " "$2 != \"lo\" {print \$2; exit}"); printf "hostname=%s\n" "$h"; printf "nic=%s\n" "$nic"; printf "cmdline="; cat /proc/cmdline; printf "smt="; cat /sys/devices/system/cpu/smt/control 2>/dev/null || true; printf "thread_choice\n"; cat "$d/thread_sibling_choice.txt" 2>/dev/null || true; printf "ptp\n"; ls -la /dev/ptp* 2>&1 || true; printf "ethtool_T\n"; ethtool -T "$nic" 2>&1 || true; printf "chronyc_sources\n"; chronyc sources -v 2>&1 || true; printf "chronyc_tracking\n"; chronyc tracking 2>&1 || true' >>"$LOG" 2>&1
  rsync -az -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" "ubuntu@$public:~/task/env/" reports/session1a/env/ >>"$LOG" 2>&1 || true
  nic="$(remote "$public" 'cd ~/task && h=$(hostname); cat env/$h/post_tuning_pre_reboot/nic.txt 2>/dev/null || cat env/$h/pre/nic.txt 2>/dev/null || true' | tr -d '\r' | tail -1)"
  ptp_count="$(remote "$public" 'ls /dev/ptp* 2>/dev/null | wc -l' | tr -d '\r')"
  hw_ts="$(remote "$public" "ethtool -T '$nic' 2>/dev/null | grep -c 'hardware-' || true" | tr -d '\r')"
  smt="$(remote "$public" 'cat /sys/devices/system/cpu/smt/control 2>/dev/null || true' | tr -d '\r')"
  isolated="$(remote "$public" 'cd ~/task && h=$(hostname); sed -n s/^chosen=//p env/$h/thread_sibling_choice.txt 2>/dev/null || true' | tr -d '\r')"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$case_name" "$iid" "$public" "$private" "$host" "$nic" "$ptp_count" "$hw_ts" "$smt" "$isolated" >> "$SUMMARY"
  runlog "terminate $case_name $iid"
  aws ec2 terminate-instances --region "$REGION" --instance-ids "$iid" >>"$LOG" 2>&1 || true
  aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$iid" || true
  remaining=()
  for value in "${created_instances[@]}"; do
    if [[ "$value" != "$iid" ]]; then
      remaining+=("$value")
    fi
  done
  created_instances=("${remaining[@]}")
}
ensure_common
launch_case cluster "AvailabilityZone=$AZ,GroupName=$CLUSTER_PG"
launch_case precision "AvailabilityZone=$AZ,GroupName=$PRECISION_PG"
launch_case none ""
runlog "phc summary"
cat "$SUMMARY" | tee -a "$LOG"
trap - EXIT