# EC2 measurement pass final report

## What ran

Provisioning preflight ran in us-east-1. The quota check returned 16.0 vCPUs and STS identified arn:aws:iam::133889911304:user/aws-cleanup-script.

The first required instance launch did not run to completion. The command REGION=us-east-1 AZ=us-east-1a INSTANCE_COUNT=1 INSTANCE_TYPE=m7i.2xlarge bash infra/provision.sh failed during aws ec2 run-instances with InvalidParameterCombination: The specified instance type is not eligible for Free Tier.

No EC2 instance was created. No build, unit test, clock probe, latency benchmark, netem matrix, fan-out run, or batch sweep executed. No WSL2 latency measurement was run.

## Environment

NOT_RUN. No EC2 host existed, so there is no AMI, kernel, ENA driver, MTU, isolated core map, sysctl state, or chrony state to report. The intended provisioning path is captured in infra/provision.sh and infra/README_RUN.md.

## Clock

clock_method=NOT_RUN. clock_residual_bound_ns=NOT_RUN. The PHC decision tree, chrony convergence, and independent UDP ping-pong probe did not execute because provisioning failed. Because no residual bound exists, no p50 one-way latency figure from this pass would be credible. None was reported.

## 5dc3d3c vs HEAD A/B

NOT_RUN. See reports/ec2_pass/05_baseline.log.

## Rate sweep

NOT_RUN. See reports/ec2_pass/06_rate_sweep.log.

## FEC verdict

NOT_RUN. The code now exposes the requested counters for a future EC2 run, but no FEC performance verdict can be drawn from this pass. See reports/ec2_pass/04_instrumentation.log and reports/ec2_pass/07_fec_verdict.log.

## Fan-out

NOT_RUN. Quota was 16 vCPUs, so the planned path would have used the same-host receiver fallback and labeled it as sender-side fan-out only. Provisioning failed before that point.

## Batch tuning

NOT_RUN. The sender now preserves true --batch-timeout-us 0 for the requested sweep, but no measurement-backed recommendation can be made.

## Open items for Midhilesh to verify

1. AWS account restriction: reports/ec2_pass/01_provision.log contains the exact m7i.2xlarge rejection. Verify whether the account is intentionally restricted to Free Tier eligible instance types, or whether a service/account setting must be changed before rerunning.
2. EC2 compile risk: instrumentation changes were not compiled on EC2 because no instance launched. First rerun step after account unblock should be make -C harness clean && make -C harness && make -C harness test on Ubuntu 24.04.
3. Clock placement group decision: no PHC evidence was collected. Rerun Part B before accepting any one-way latency number.
4. Remote orchestration: benchmark/run_remote.sh and benchmark/sweep_rate.sh passed bash syntax checks but were not exercised against EC2 hosts.
5. Provisioning cleanup: reports/ec2_pass/10_teardown.log shows the exact placement group and security group created before run-instances failed, then deleted.
6. Exit-code capture: the initial 01_provision.log preflight lines recorded PowerShell boolean strings for some exit codes. The provisioning error text itself is captured verbatim; future reruns should invoke a bash script file for numeric exit-code capture.

## Spend and teardown confirmation

Actual elapsed instance-hours: 0. No instance ID was returned by run-instances. Estimated instance spend: 0 USD. The transient security group and placement group were deleted. All five required independent teardown queries returned empty arrays in reports/ec2_pass/10_teardown.log.