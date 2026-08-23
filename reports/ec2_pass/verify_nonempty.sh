for f in reports/ec2_pass/00_manifest.log reports/ec2_pass/01_provision.log reports/ec2_pass/02_bootstrap.log reports/ec2_pass/03_clock.log reports/ec2_pass/04_instrumentation.log reports/ec2_pass/05_baseline.log reports/ec2_pass/06_rate_sweep.log reports/ec2_pass/07_fec_verdict.log reports/ec2_pass/08_fanout.log reports/ec2_pass/09_batch_tuning.log reports/ec2_pass/10_teardown.log reports/ec2_pass/11_final_report.md; do
  if [[ -s "$f" ]]; then
    printf 'OK %s %s bytes\n' "$f" "$(wc -c < "$f")"
  else
    printf 'MISSING %s\n' "$f"
  fi
done