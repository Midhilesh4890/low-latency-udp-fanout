set -u
cd /home/midhilesh/projects/task
log=reports/ec2_pass/acceptance_checks.log
: >"$log"
run() {
  printf 'command: %s\n' "$*" >>"$log"
  "$@" >>"$log" 2>&1
  rc=$?
  printf 'exit_code=%s\n' "$rc" >>"$log"
  return 0
}
printf 'command: no comment/docstring marker grep\n' >>"$log"
{
  grep -RInF '//' infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh tools/clock_probe.cpp harness/include/fec.h harness/include/dedupe_window.h harness/src/receiver.cpp harness/src/sender.cpp benchmark/summarize.py benchmark/sweep_rate.sh || true
  grep -RInF '/*' infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh tools/clock_probe.cpp harness/include/fec.h harness/include/dedupe_window.h harness/src/receiver.cpp harness/src/sender.cpp benchmark/summarize.py benchmark/sweep_rate.sh || true
  grep -RInF '*/' infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh tools/clock_probe.cpp harness/include/fec.h harness/include/dedupe_window.h harness/src/receiver.cpp harness/src/sender.cpp benchmark/summarize.py benchmark/sweep_rate.sh || true
  grep -RInE '^[[:space:]]*#[[:space:]]|^[[:space:]]*#$' infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh tools/clock_probe.cpp harness/include/fec.h harness/include/dedupe_window.h harness/src/receiver.cpp harness/src/sender.cpp benchmark/summarize.py benchmark/sweep_rate.sh || true
} > reports/ec2_pass/comment_grep.txt
cat reports/ec2_pass/comment_grep.txt >>"$log"
printf 'exit_code=0\n' >>"$log"
run bash -n infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh benchmark/sweep_rate.sh reports/ec2_pass/teardown_commands.sh reports/ec2_pass/verify_nonempty.sh
run python3 -m py_compile benchmark/summarize.py
rm -rf benchmark/__pycache__
printf 'command: required log non-empty check\n' >>"$log"
bash reports/ec2_pass/verify_nonempty.sh >>"$log" 2>&1
printf 'exit_code=%s\n' "$?" >>"$log"
run git log --oneline origin/main..main
run git reflog show origin/main
printf 'command: no EC2 build/test result\nNOT_RUN: make -C harness clean && make -C harness && make -C harness test did not run because no EC2 instance launched.\nexit_code=0\n' >>"$log"
printf 'command: no local latency measurement\nOK: no benchmark command was run locally in this pass; provisioning failed before EC2 hosts existed.\nexit_code=0\n' >>"$log"
printf 'command: teardown evidence\n' >>"$log"
cat reports/ec2_pass/10_teardown.log >>"$log"
printf 'exit_code=0\n' >>"$log"
find reports/ec2_pass -type f -printf '%p\t%s bytes\n' | sort > reports/ec2_pass/file_sizes.txt
cat "$log"