# EC2 run commands

Provision one sender host:

bash infra/provision.sh

Provision the receiver host when needed:

INSTANCE_COUNT=1 INSTANCE_TYPE=m7i.2xlarge bash infra/provision.sh

Destroy all pass resources:

aws ec2 terminate-instances --region us-east-1 --instance-ids $(aws ec2 describe-instances --region us-east-1 --filters Name=tag:Project,Values=spectral-ec2-pass Name=instance-state-name,Values=pending,running,stopping,stopped --query 'Reservations[].Instances[].InstanceId' --output text)
aws ec2 delete-placement-group --region us-east-1 --group-name spectral-ec2-pass-cluster

Estimated hourly cost path used with 16 vCPU quota: two m7i.2xlarge instances for two-host work, then one m7i.2xlarge sender plus receiver processes colocated on the receiver host for fan-out fallback. On-demand us-east-1 m7i.2xlarge is about 0.4032 USD/hour, so the active two-host phase is about 0.8064 USD/hour before EBS pennies.