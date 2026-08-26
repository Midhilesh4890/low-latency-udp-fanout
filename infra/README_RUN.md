# EC2 host preparation

`provision.sh` creates Ubuntu EC2 instances, a cluster placement group, and a security group. Root EBS volumes use `DeleteOnTermination=true`, and instance-initiated shutdown terminates the instance.

Required environment variables depend on the AWS account and target Availability Zone. A report-compatible launch uses:

```bash
REGION=us-east-1 \
AZ=us-east-1d \
INSTANCE_TYPE=m7i.4xlarge \
INSTANCE_COUNT=2 \
CORE_COUNT=8 \
THREADS_PER_CORE=1 \
NAME_PREFIX=spectral-transport \
bash infra/provision.sh
```

Copy the repository to both hosts, then run `infra/bootstrap.sh`. The script installs build and timing utilities, raises UDP socket limits, disables `irqbalance`, selects distinct physical CPUs, adds `isolcpus`, `nohz_full`, and `rcu_nocbs` kernel parameters, moves IRQ affinity to CPU 0, and reboots when required.

After reboot, verify the kernel command line and CPU topology before running a benchmark:

```bash
cat /proc/cmdline
lscpu -e
bash benchmark/preflight_isolation.sh \
  --cores 1,2,3,4 \
  --isolated-cores 1,2,3,4 \
  --label transport-tx
```

Terminate instances and delete the placement group and security group after measurement. Confirm that no tagged instances, EBS volumes, Elastic IPs, or network interfaces remain.
