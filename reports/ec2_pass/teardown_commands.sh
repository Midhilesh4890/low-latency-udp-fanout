set -u
log=reports/ec2_pass/10_teardown.log
mkdir -p reports/ec2_pass
: >"$log"
run() {
  printf 'command: %s\n' "$*" >>"$log"
  "$@" >>"$log" 2>&1
  rc=$?
  printf 'exit_code=%s\n' "$rc" >>"$log"
  return 0
}
ids="$(aws ec2 describe-instances --region us-east-1 --filters Name=tag:Project,Values=spectral-ec2-pass Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output text 2>>"$log")"
if [[ -n "$ids" ]]; then
  run aws ec2 terminate-instances --region us-east-1 --instance-ids $ids
fi
pg_exists="$(aws ec2 describe-placement-groups --region us-east-1 --group-names spectral-ec2-pass-cluster --query 'PlacementGroups[].GroupName' --output text 2>/dev/null)"
if [[ "$pg_exists" == "spectral-ec2-pass-cluster" ]]; then
  run aws ec2 delete-placement-group --region us-east-1 --group-name spectral-ec2-pass-cluster
fi
sg_id="$(aws ec2 describe-security-groups --region us-east-1 --filters Name=group-name,Values=spectral-ec2-pass-sg --query 'SecurityGroups[].GroupId' --output text 2>/dev/null)"
if [[ -n "$sg_id" ]]; then
  run aws ec2 delete-security-group --region us-east-1 --group-id "$sg_id"
fi
run aws ec2 describe-instances --region us-east-1 --filters Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId'
run aws ec2 describe-volumes --region us-east-1 --filters Name=status,Values=available,in-use --query 'Volumes[].VolumeId'
run aws ec2 describe-addresses --region us-east-1 --query 'Addresses[].AllocationId'
run aws ec2 describe-placement-groups --region us-east-1 --query 'PlacementGroups[].GroupName'
run aws ec2 describe-security-groups --region us-east-1 --filters Name=group-name,Values='spectral*' --query 'SecurityGroups[].GroupId'
cat "$log"