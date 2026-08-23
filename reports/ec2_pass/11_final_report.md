# EC2 measurement pass final report

## What ran

Provisioning preflight ran in us-east-1. The quota check returned 16.0 vCPUs and STS identified arn:aws:iam::133889911304:user/aws-cleanup-script.

The required scaffolding from e21493f was verified present before provisioning: infra/provision.sh, infra/bootstrap.sh, benchmark/run_remote.sh, tools/clock_probe.cpp, the FEC/dedupe counters, and benchmark/summarize.py. The current infra/provision.sh placement-group fix was preserved.

The first required instance launch did not run to completion. The command REGION=us-east-1 AZ=us-east-1a INSTANCE_COUNT=1 INSTANCE_TYPE=m7i.2xlarge NAME_PREFIX=spectral-ec2-pass bash infra/provision.sh failed during aws ec2 run-instances with InvalidParameterCombination: The specified instance type is not eligible for Free Tier.

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

NOT_RUN. The requested counters exist from e21493f and were verified present, but no FEC performance verdict can be drawn from this pass. See reports/ec2_pass/04_instrumentation.log and reports/ec2_pass/07_fec_verdict.log.

## Fan-out

NOT_RUN. Quota was 16.0 vCPUs, so the planned path after successful two-host provisioning would have used the same-host receiver fallback and labeled it as sender-side fan-out only. Provisioning failed before that point.

## Batch tuning

NOT_RUN. No measurement-backed recommendation can be made.

## Validation Notes

1. AWS account restriction: reports/ec2_pass/01_provision.log contains the exact m7i.2xlarge rejection after the placement-group fix. The provisioning limit appears to be an account or launch policy restriction to Free Tier eligible instance types, not the placement group creation path.
2. Dry-run discrepancy: the user reported m7i.2xlarge dry-run success into spectral-ec2-pass-cluster, but the real run-instances call still failed. Verify whether DryRun bypassed this account restriction or whether a different AWS profile, region, or request shape was used.
3. EC2 compile risk: instrumentation changes from e21493f were not compiled on EC2 because no instance launched. First rerun step after account access permits the target instance type should be make -C harness clean && make -C harness && make -C harness test on Ubuntu 24.04.
4. Clock placement group decision: no PHC evidence was collected. Rerun Part B before accepting any one-way latency number.
5. Remote orchestration: benchmark/run_remote.sh and benchmark/sweep_rate.sh passed bash syntax checks but were not exercised against EC2 hosts.
6. Resource cleanup: reports/ec2_pass/10_teardown.log shows all five required independent verification queries returning empty arrays.
7. No tc impairment was applied, so there was no qdisc to remove. The final tc qdisc show is NOT_RUN because no host existed.

## Spend and teardown confirmation

Actual elapsed instance-hours: 0. No instance ID was returned by run-instances. Estimated instance spend: 0 USD. All five required independent teardown queries returned empty arrays in reports/ec2_pass/10_teardown.log.