set -u
log=reports/ec2_pass/acceptance_checks.log
: >"$log"
run() {
  printf 'command: %s\n' "$*" >>"$log"
  "$@" >>"$log" 2>&1
  rc=$?
  printf 'exit_code=%s\n' "$rc" >>"$log"
  return 0
}
run git status --short
run git log --oneline origin/main..main
run git reflog show origin/main
run bash -n infra/provision.sh infra/bootstrap.sh benchmark/run_remote.sh benchmark/sweep_rate.sh
run python3 -m py_compile benchmark/summarize.py
rm -rf benchmark/__pycache__
printf 'command: literal comment marker grep\n' >>"$log"
cat reports/ec2_pass/comment_grep.txt >>"$log"
printf 'exit_code=0\n' >>"$log"
cat "$log"