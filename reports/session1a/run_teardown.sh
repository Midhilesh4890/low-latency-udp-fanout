set -euo pipefail
{
set -x
aws ec2 describe-instances --region us-east-1 --filters Name=tag:Session,Values=session1a Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output json
aws ec2 describe-instances --region us-east-1 --filters Name=tag:Project,Values=spectral-ec2-pass Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output json
aws ec2 describe-volumes --region us-east-1 --filters Name=tag:Session,Values=session1a Name=status,Values=creating,available,in-use --query 'Volumes[].VolumeId' --output json
aws ec2 describe-network-interfaces --region us-east-1 --filters Name=tag:Session,Values=session1a --query 'NetworkInterfaces[].NetworkInterfaceId' --output json
VPC=$(aws ec2 describe-vpcs --region us-east-1 --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text)
SG=$(aws ec2 describe-security-groups --region us-east-1 --filters Name=group-name,Values=spectral-ec2-pass-sg Name=vpc-id,Values="$VPC" --query 'SecurityGroups[0].GroupId' --output text)
if [ "$SG" != None ] && [ -n "$SG" ]; then aws ec2 delete-security-group --region us-east-1 --group-id "$SG" || true; fi
aws ec2 describe-security-groups --region us-east-1 --filters Name=group-name,Values=spectral-ec2-pass-sg Name=vpc-id,Values="$VPC" --query 'SecurityGroups[].GroupId' --output json
aws ec2 describe-placement-groups --region us-east-1 --query 'PlacementGroups[?starts_with(GroupName, `spectral-ec2-pass`)].{Name:GroupName,Strategy:Strategy,State:State,Id:GroupId}' --output json
} > reports/session1a/06_teardown.log 2>&1
cat reports/session1a/06_teardown.log