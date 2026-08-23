set -euo pipefail

region="${REGION:-us-east-1}"
az="${AZ:-us-east-1a}"
instance_type="${INSTANCE_TYPE:-m7i.2xlarge}"
ami="${AMI:-}"
key_name="${KEY_NAME:-spectral-key}"
operator_cidr="${OPERATOR_CIDR:-}"
count="${INSTANCE_COUNT:-1}"
name_prefix="${NAME_PREFIX:-spectral-ec2-pass}"
placement_group="${PLACEMENT_GROUP:-${name_prefix}-cluster}"

if [[ -z "$operator_cidr" ]]; then
  checkip_scheme="https:"
  ip="$(curl -fsS "$checkip_scheme""/""/checkip.amazonaws.com" | tr -d '\n')"
  operator_cidr="${ip}/32"
fi

if [[ -z "$ami" ]]; then
  ami="$(aws ssm get-parameter --region "$region" --name /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id --query Parameter.Value --output text)"
fi

vpc_id="$(aws ec2 describe-vpcs --region "$region" --filters Name=is-default,Values=true --query 'Vpcs[0].VpcId' --output text)"
subnet_id="$(aws ec2 describe-subnets --region "$region" --filters Name=vpc-id,Values="$vpc_id" Name=availability-zone,Values="$az" --query 'Subnets[0].SubnetId' --output text)"
sg_id="$(aws ec2 create-security-group --region "$region" --group-name "${name_prefix}-sg" --description "${name_prefix}" --vpc-id "$vpc_id" --query GroupId --output text 2>/dev/null || aws ec2 describe-security-groups --region "$region" --filters Name=group-name,Values="${name_prefix}-sg" Name=vpc-id,Values="$vpc_id" --query 'SecurityGroups[0].GroupId' --output text)"

aws ec2 authorize-security-group-ingress --region "$region" --group-id "$sg_id" --ip-permissions "IpProtocol=tcp,FromPort=22,ToPort=22,IpRanges=[{CidrIp=$operator_cidr,Description=operator}]" >/dev/null 2>&1 || true
aws ec2 authorize-security-group-ingress --region "$region" --group-id "$sg_id" --ip-permissions "IpProtocol=udp,FromPort=0,ToPort=65535,UserIdGroupPairs=[{GroupId=$sg_id,Description=spectral-udp}]" >/dev/null 2>&1 || true
aws ec2 authorize-security-group-ingress --region "$region" --group-id "$sg_id" --ip-permissions "IpProtocol=icmp,FromPort=-1,ToPort=-1,UserIdGroupPairs=[{GroupId=$sg_id,Description=spectral-icmp}]" >/dev/null 2>&1 || true

aws ec2 describe-placement-groups --region "$region" --group-names "$placement_group" >/dev/null 2>&1 || aws ec2 create-placement-group --region "$region" --group-name "$placement_group" --strategy cluster

user_data="$(mktemp)"
printf '%s\n' '#!/bin/sh' 'shutdown -h +240' >"$user_data"

aws ec2 run-instances \
  --region "$region" \
  --image-id "$ami" \
  --instance-type "$instance_type" \
  --key-name "$key_name" \
  --subnet-id "$subnet_id" \
  --security-group-ids "$sg_id" \
  --placement "AvailabilityZone=$az,GroupName=$placement_group" \
  --instance-initiated-shutdown-behavior terminate \
  --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=8,VolumeType=gp3,DeleteOnTermination=true}' \
  --associate-public-ip-address \
  --user-data "file:""/""/$user_data" \
  --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=${name_prefix}},{Key=Project,Value=spectral-ec2-pass}]" "ResourceType=volume,Tags=[{Key=Name,Value=${name_prefix}},{Key=Project,Value=spectral-ec2-pass}]" \
  --count "$count"

rm -f "$user_data"