set -euo pipefail
REGION=us-east-1
AZ=us-east-1a
TYPE=m7i.2xlarge
KEY=spectral-key
KEY_PATH=~/.ssh/spectral-key.pem
PREFIX=spectral-ec2-pass
CLUSTER_PG=spectral-ec2-pass-cluster
KNOWN=/tmp/session1a_shared_known_hosts
LOG=reports/session1a/04_orchestration.log
TRAP=reports/session1a/05_trap1.log
STATE=reports/session1a/shared_state.env
mkdir -p reports/session1a/env
created=()
log(){ local f="$1"; shift; printf '\n[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$f"; }
cleanup(){ if ((${#created[@]})); then log "$LOG" "cleanup terminate ${created[*]}"; aws ec2 terminate-instances --region "$REGION" --instance-ids "${created[@]}" >>"$LOG" 2>&1 || true; fi; }
trap cleanup EXIT
ssh_opts=(-i "$KEY_PATH" -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$KNOWN" -o ConnectTimeout=5)
remote(){ local ip="$1"; shift; ssh "${ssh_opts[@]}" "ubuntu@$ip" "$@"; }
wait_ssh(){ local ip="$1" label="$2"; for i in $(seq 1 120); do if remote "$ip" true >/dev/null 2>&1; then sleep 3; if remote "$ip" true >/dev/null 2>&1; then log "$LOG" "ssh ready $label $ip attempt=$i"; return 0; fi; fi; sleep 5; done; return 1; }
AMI="$(aws ssm get-parameter --region "$REGION" --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id --query Parameter.Value --output text)"
VPC="$(aws ec2 describe-vpcs --region "$REGION" --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text)"
SUBNET="$(aws ec2 describe-subnets --region "$REGION" --filters Name=vpc-id,Values="$VPC" Name=availability-zone,Values="$AZ" --query 'Subnets[0].SubnetId' --output text)"
CIDR="$(curl -fsS https://checkip.amazonaws.com | tr -d '\n')/32"
SG="$(aws ec2 create-security-group --region "$REGION" --group-name "${PREFIX}-sg" --description "$PREFIX" --vpc-id "$VPC" --query GroupId --output text 2>/dev/null || aws ec2 describe-security-groups --region "$REGION" --filters Name=group-name,Values="${PREFIX}-sg" Name=vpc-id,Values="$VPC" --query 'SecurityGroups[0].GroupId' --output text)"
aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=tcp,FromPort=22,ToPort=22,IpRanges=[{CidrIp=$CIDR,Description=ssh-access}]" >>"$LOG" 2>&1 || true
aws ec2 authorize-security-group-ingress --region "$REGION" --group-id "$SG" --ip-permissions "IpProtocol=udp,FromPort=0,ToPort=65535,UserIdGroupPairs=[{GroupId=$SG,Description=spectral-udp}]" >>"$LOG" 2>&1 || true
aws ec2 describe-placement-groups --region "$REGION" --group-names "$CLUSTER_PG" >>"$LOG" 2>&1 || aws ec2 create-placement-group --region "$REGION" --group-name "$CLUSTER_PG" --strategy cluster >>"$LOG" 2>&1
ud="$(mktemp)"; printf '%s\n' '#!/bin/sh' 'shutdown -h +180' >"$ud"
log "$LOG" "launch shared-clock fallback host"
IID="$(aws ec2 run-instances --region "$REGION" --image-id "$AMI" --instance-type "$TYPE" --key-name "$KEY" --subnet-id "$SUBNET" --security-group-ids "$SG" --placement "AvailabilityZone=$AZ,GroupName=$CLUSTER_PG" --instance-initiated-shutdown-behavior terminate --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true}' --associate-public-ip-address --user-data "file://$ud" --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=${PREFIX}-shared-clock},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a},{Key=Case,Value=shared-clock}]" "ResourceType=volume,Tags=[{Key=Name,Value=${PREFIX}-shared-clock},{Key=Project,Value=spectral-ec2-pass},{Key=Session,Value=session1a}]" --query 'Instances[0].InstanceId' --output text)"
rm -f "$ud"
created+=("$IID")
aws ec2 wait instance-running --region "$REGION" --instance-ids "$IID"
PUBLIC="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$IID" --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)"
PRIVATE="$(aws ec2 describe-instances --region "$REGION" --instance-ids "$IID" --query 'Reservations[0].Instances[0].PrivateIpAddress' --output text)"
log "$LOG" "shared iid=$IID public=$PUBLIC private=$PRIVATE"
wait_ssh "$PUBLIC" shared
rsync -az --exclude .git --exclude benchmark/results --exclude reports -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" ./ "ubuntu@$PUBLIC:~/task/" >>"$LOG" 2>&1
log "$LOG" "bootstrap first pass shared"
remote "$PUBLIC" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG" 2>&1 || true
sleep 10
aws ec2 wait instance-status-ok --region "$REGION" --instance-ids "$IID" || true
wait_ssh "$PUBLIC" shared-after-reboot
log "$LOG" "bootstrap second pass shared"
remote "$PUBLIC" 'cd ~/task && bash infra/bootstrap.sh' >>"$LOG" 2>&1 || true
sleep 10
wait_ssh "$PUBLIC" shared-post-bootstrap
log "$LOG" "build/test shared"
remote "$PUBLIC" 'cd ~/task && make -C harness clean && make -C harness && make -C harness test' >>"$LOG" 2>&1
HOST="$(remote "$PUBLIC" hostname | tr -d '\r')"
ISO="$(remote "$PUBLIC" 'cd ~/task && h=$(hostname); sed -n s/^chosen=//p env/$h/thread_sibling_choice.txt 2>/dev/null || true' | tr -d '\r')"
if [[ -z "$ISO" ]]; then ISO=1,5,2,6,3,7; fi
rsync -az -e "ssh -i $KEY_PATH -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$KNOWN" "ubuntu@$PUBLIC:~/task/env/" reports/session1a/env/ >>"$LOG" 2>&1 || true
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
run_one(){ local label="$1" mode="$2" port="$3" out="$4" logf="$5"; log "$logf" "run $label mode=$mode out=$out"; bash benchmark/run_remote.sh --tx-host "$PUBLIC" --rx-hosts "$PUBLIC" --rx-privates 127.0.0.1 --ssh-key "$KEY_PATH" --outdir "$out" --rate 300000 --count 300000 --warmup 20000 --slots 65536 --base-port "$port" --fanout 1 --cpu-producer 1 --cpu-sender 2 --cpu-receiver 3 --cpu-consumer 5 --isolated-cores "$ISO" --clock-method shared_clock --clock-residual-bound-ns 0 --instance-type "$TYPE" --az "$AZ" --placement-group-type cluster_shared_host --mtu 9001 --run-order-index "$port" --not-p9999-grade --run-label "$label" --latency-output "$mode" --no-build >>"$logf" 2>&1; }
summarize(){ local out="$1" logf="$2"; if find "$out" -path '*/latency.csv' -type f | grep -q .; then python3 benchmark/summarize.py "$out" | tee -a "$logf"; fi; }
SMOKE="benchmark/results/session1a_${STAMP}_shared_smoke_disk"
run_one shared_smoke disk 9200 "$SMOKE" "$LOG"
summarize "$SMOKE" "$LOG"
if grep -q CLOCK_INVALID "$SMOKE/summary.csv"; then log "$LOG" "shared smoke unexpectedly clock invalid"; exit 1; fi
DISK="benchmark/results/session1a_${STAMP}_shared_trap1_disk"
TMPFS="benchmark/results/session1a_${STAMP}_shared_trap1_tmpfs"
NONE="benchmark/results/session1a_${STAMP}_shared_trap1_none"
run_one shared_trap1_disk disk 9210 "$DISK" "$TRAP"; summarize "$DISK" "$TRAP"
run_one shared_trap1_tmpfs tmpfs 9220 "$TMPFS" "$TRAP"; summarize "$TMPFS" "$TRAP"
run_one shared_trap1_none none 9230 "$NONE" "$TRAP"
printf '%s\n' "shared_host=$HOST instance=$IID public=$PUBLIC private=$PRIVATE isolated=$ISO smoke=$SMOKE disk=$DISK tmpfs=$TMPFS none=$NONE" >"$STATE"
log "$LOG" "terminate shared host $IID"
aws ec2 terminate-instances --region "$REGION" --instance-ids "$IID" >>"$LOG" 2>&1 || true
aws ec2 wait instance-terminated --region "$REGION" --instance-ids "$IID" || true
created=()
trap - EXIT