# Stage 7 Baseline

## Consumer CSV Change

consumer.cpp was changed only to add send_ts_ns to the CSV header and to write hdr->send_ts_ns as the third CSV field. sender.cpp, receiver.cpp and harness/include were not modified.

Complete diff:

~~~~diff
diff --git a/benchmark/summarize.py b/benchmark/summarize.py
index 9abc7cf..321b885 100755
--- a/benchmark/summarize.py
+++ b/benchmark/summarize.py
@@ -47,6 +47,22 @@ def receiver_for(latency_csv):
     return latency_csv.parent.name
 
 
+def read_lapped_count(path, pattern):
+    try:
+        text = path.read_text(encoding="utf-8", errors="replace")
+    except OSError:
+        return ""
+    match = re.search(pattern, text)
+    return "" if match is None else int(match.group(1))
+
+
+def lapped_counts_for(log_dir):
+    return {
+        "consumer_lapped": read_lapped_count(log_dir / "consumer.log", r"consumer: lapped ([0-9]+) times"),
+        "sender_lapped": read_lapped_count(log_dir / "sender.log", r"sender: sent=[0-9]+ packets=[0-9]+ lapped=([0-9]+)"),
+    }
+
+
 def discover_latency_csvs(root):
     if not root.exists():
         fail(f"input root does not exist: {root}")
@@ -234,6 +250,7 @@ def summarize_receiver(root, latency_csv, skip_warmup):
     run_dir = run_dir_for(latency_csv)
     data = read_run_json(run_dir / "run.json")
     parameters = data.get("parameters", {})
+    lapped_counts = lapped_counts_for(latency_csv.parent)
     recorded_warmup = warmup_from(data)
     warmup = recorded_warmup if skip_warmup is None else int(skip_warmup)
     samples = read_samples(latency_csv, warmup)
@@ -279,6 +296,8 @@ def summarize_receiver(root, latency_csv, skip_warmup):
         "cpu_consumer": parameters.get("cpu_consumer", ""),
         "sndbuf": parameters.get("sndbuf", ""),
         "rcvbuf": parameters.get("rcvbuf", ""),
+        "consumer_lapped": lapped_counts["consumer_lapped"],
+        "sender_lapped": lapped_counts["sender_lapped"],
         "hostname": data.get("hostname", ""),
         "wall_clock_received_rate": wall_clock_received_rate,
         "achieved_rate": achieved_rate,
@@ -376,6 +395,8 @@ def aggregate_rows(root, rows):
         aggregate["receiver"] = "all"
         aggregate["aggregate_repeats"] = len(repeat_numbers)
         aggregate["freeze_count"] = int(sum(1 for row in group_rows if row["freeze_events"]))
+        aggregate["consumer_lapped"] = sum(int(row["consumer_lapped"]) for row in group_rows if row["consumer_lapped"] not in (None, ""))
+        aggregate["sender_lapped"] = sum(int(row["sender_lapped"]) for row in group_rows if row["sender_lapped"] not in (None, ""))
         aggregate["high_loss"] = any(row["high_loss"] for row in group_rows)
         aggregate["clock_invalid"] = any(row["clock_invalid"] for row in group_rows)
         aggregate["void"] = any(row["void"] for row in group_rows)
@@ -415,7 +436,7 @@ def aggregate_rows(root, rows):
 
 
 def public_keys():
-    return ["row_type", "config", "repeat", "run_dir", "receiver", "offered_rate", "rate", "count", "slots", "type", "cpu_producer", "cpu_sender", "cpu_receiver", "cpu_consumer", "sndbuf", "rcvbuf", "warmup", "skip_warmup", "hostname", "clock_method", "max_drift_ns", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "p50", "p90", "p99", "p99_9", "p99_99", "min", "mean", "max", "fanout_spread_p99", "ramp_ratio", "ramp_slope_ns", "saturated", "saturated_by_ramp", "saturated_by_rate", "saturation_triggers", "high_loss", "freeze_events", "clock_invalid", "void", "valid_latency", "aggregate_repeats", "p50_median", "p50_min", "p50_max", "p99_median", "p99_min", "p99_max", "max_median", "max_min", "max_max", "freeze_count", "aggregate_note"]
+    return ["row_type", "config", "repeat", "run_dir", "receiver", "offered_rate", "rate", "count", "slots", "type", "cpu_producer", "cpu_sender", "cpu_receiver", "cpu_consumer", "sndbuf", "rcvbuf", "consumer_lapped", "sender_lapped", "warmup", "skip_warmup", "hostname", "clock_method", "max_drift_ns", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "p50", "p90", "p99", "p99_9", "p99_99", "min", "mean", "max", "fanout_spread_p99", "ramp_ratio", "ramp_slope_ns", "saturated", "saturated_by_ramp", "saturated_by_rate", "saturation_triggers", "high_loss", "freeze_events", "clock_invalid", "void", "valid_latency", "aggregate_repeats", "p50_median", "p50_min", "p50_max", "p99_median", "p99_min", "p99_max", "max_median", "max_min", "max_max", "freeze_count", "aggregate_note"]
 
 
 def public_row(row):
@@ -441,7 +462,7 @@ def print_table(rows, warnings):
         item = public_row(row)
         item["flags"] = flags_for(row)
         display.append(item)
-    columns = ["row_type", "config", "repeat", "receiver", "rate", "count", "warmup", "skip_warmup", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "p50", "p99", "p99_9", "max", "ramp_ratio", "ramp_slope_ns", "saturation_triggers", "flags", "freeze_count"]
+    columns = ["row_type", "config", "repeat", "receiver", "rate", "count", "warmup", "skip_warmup", "wall_clock_received_rate", "achieved_rate", "received", "expected", "dropped", "drop_rate", "consumer_lapped", "sender_lapped", "p50", "p99", "p99_9", "max", "ramp_ratio", "ramp_slope_ns", "saturation_triggers", "flags", "freeze_count"]
     widths = {column: len(column) for column in columns}
     lines = []
     for row in display:
diff --git a/harness/src/consumer.cpp b/harness/src/consumer.cpp
index fcd0590..edef6d3 100644
--- a/harness/src/consumer.cpp
+++ b/harness/src/consumer.cpp
@@ -84,7 +84,7 @@ int main(int argc, char** argv) {
   FILE* csv = nullptr;
   if (!cfg.csv.empty()) {
     csv = std::fopen(cfg.csv.c_str(), "w");
-    if (csv) std::fprintf(csv, "seq,latency_ns\n");
+    if (csv) std::fprintf(csv, "seq,latency_ns,send_ts_ns\n");
   }
 
   uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
@@ -105,9 +105,10 @@ int main(int argc, char** argv) {
       const uint64_t latency =
           recv_ts > hdr->send_ts_ns ? recv_ts - hdr->send_ts_ns : 0;
       acc.record(hdr->seq_id, latency);
-      if (csv) std::fprintf(csv, "%llu,%llu\n",
+      if (csv) std::fprintf(csv, "%llu,%llu,%llu\n",
                             (unsigned long long)hdr->seq_id,
-                            (unsigned long long)latency);
+                            (unsigned long long)latency,
+                            (unsigned long long)hdr->send_ts_ns);
       ++received;
       ++read_index;
       last_progress = recv_ts;
~~~~

## Prior Lapped Comparison

Lapped counts from the previous 20260815T205658Z sweep show 800000 lapped heavily in the sender while 600000 did not lap:

~~~~text
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_1/rx_local/sender.log:2:sender: sent=4220702 packets=4220702 lapped=154870
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_1/rx_local/consumer.log:1:consumer: lapped 0 times
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_2/rx_local/sender.log:2:sender: sent=4163525 packets=4163525 lapped=226964
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_2/rx_local/consumer.log:1:consumer: lapped 0 times
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_3/rx_local/sender.log:2:sender: sent=3995596 packets=3995596 lapped=321703
benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_3/rx_local/consumer.log:1:consumer: lapped 0 times
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_1/rx_local/sender.log:2:sender: sent=3300000 packets=3300000 lapped=0
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_1/rx_local/consumer.log:1:consumer: lapped 0 times
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_2/rx_local/sender.log:2:sender: sent=3300000 packets=3300000 lapped=0
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_2/rx_local/consumer.log:1:consumer: lapped 0 times
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_3/rx_local/sender.log:2:sender: sent=3300000 packets=3300000 lapped=0
benchmark/results/rate_sweep/20260815T205658Z/rate_600000/rep_3/rx_local/consumer.log:1:consumer: lapped 0 times
~~~~

Conclusion: the two boundary-adjacent samples in the excluded 800000 run are consistent with shared-memory ring lapping and corrupted reads from overwritten slots, not instrumentation arithmetic wraparound.

## Re-Sweep

Command:

~~~~text
benchmark/sweep_rate.sh --rates 100000,200000,300000,400000,500000,600000
~~~~

Run root:

~~~~text
benchmark/results/rate_sweep/20260815T213507Z
~~~~

summarize.py output:

~~~~text
 row_type      config repeat receiver   rate   count warmup skip_warmup wall_clock_received_rate achieved_rate received expected dropped  drop_rate consumer_lapped sender_lapped      p50       p99     p99_9       max  ramp_ratio ramp_slope_ns saturation_triggers                 flags freeze_count
      run rate_100000      1 rx_local 100000  500000  50000       50000                  64083.4       99036.2   495180   500000    4820    0.00964               0             0     1905  45480585  82217415  86981868    0.891692       4.33531                                      LOSS             
      run rate_100000      2 rx_local 100000  500000  50000       50000                  87356.7        100001   500000   500000       0          0               0             0     2346    892542   2758138   4615554     1.36585     0.0830716                                        OK             
      run rate_100000      3 rx_local 100000  500000  50000       50000                  65119.2         99996   499979   500000      21    4.2e-05               0             0     2628   1085778   3159912   4351511     1.00452    0.00905858                                        OK             
      run rate_200000      1 rx_local 200000 1000000 100000      100000                   128471        199231   996152  1000000    3848   0.003848               0             0     2774   7112627  12013756  13134373     64.5132       1.07132                ramp             SATURATED             
      run rate_200000      2 rx_local 200000 1000000 100000      100000                   128355        199544   997720  1000000    2280    0.00228               0             0     2702   7925021  31677152  35694536     1.07019     -0.781032                                        OK             
      run rate_200000      3 rx_local 200000 1000000 100000      100000                   128994        199453   997262  1000000    2738   0.002738               0             0     2446   2131631   4474283   6161097     1.28169      0.114277                                        OK             
      run rate_300000      1 rx_local 300000 1500000 150000      150000                   190029        297444  1486778  1500000   13222 0.00881467               0             0     3106  23107621  27419295  30337544     1.23864       1.22962                                      LOSS             
      run rate_300000      2 rx_local 300000 1500000 150000      150000                   188244        299097  1494797  1500000    5203 0.00346867               0             0     3028   3692975   7472762  10315267     1.89238      0.130391                                        OK             
      run rate_300000      3 rx_local 300000 1500000 150000      150000                   183825        296068  1479893  1500000   20107  0.0134047               3             2    51417 208498215 217100024 262587140     366.612       29.1635                ramp SATURATED|LOSS|FREEZE             
      run rate_400000      1 rx_local 400000 2000000 200000      200000                   196851        315390  1578900  2000000  421100    0.21055               7         10328    85569 166737790 199343963 199826429     22302.8       34.6251           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_400000      2 rx_local 400000 2000000 200000      200000                   243556        386526  1931731  2000000   68269  0.0341345               6             0    27136 192673973 231123892 235387073     3935.33       23.6256                ramp SATURATED|LOSS|FREEZE             
      run rate_400000      3 rx_local 400000 2000000 200000      200000                   170073        268329  1336485  2000000  663515   0.331757               2          5651 56644897 163856206 179383449 181678949      2.0114      -40.8163           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_500000      1 rx_local 500000 2500000 250000      250000                   305130        498911  2492751  2500000    7249  0.0028996               2             0    18377 109402110 128360751 131074777     96.2752       1.02502                ramp      SATURATED|FREEZE             
      run rate_500000      2 rx_local 500000 2500000 250000      250000                   230691        361418  1802155  2500000  697845   0.279138               8             4 11726043 125851909 144479158 147462316 0.000333926       -12.636                rate SATURATED|LOSS|FREEZE             
      run rate_500000      3 rx_local 500000 2500000 250000      250000                   317021        499961  2469925  2500000   30075    0.01203               0             0     5185  68959501 101200315 104844080     3.91697     -0.589215                ramp        SATURATED|LOSS             
      run rate_600000      1 rx_local 600000 3000000 300000      300000                   234376        369207  1845297  3000000 1154703   0.384901               0        100052  1354974 178614398 198284119 200770349     32118.4       62.7314           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_600000      2 rx_local 600000 3000000 300000      300000                   189918        306649  1530750  3000000 1469250    0.48975               5         67636  2296150 178096484 184623844 186333554     21.0012       70.3402           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_600000      3 rx_local 600000 3000000 300000      300000                   157871        280936  1390329  3000000 1609671   0.536557              27         87997 82987383 400614463 415622545 417261131     2.26794       95.9414           ramp|rate        SATURATED|LOSS             
aggregate rate_100000    all      all 100000  500000  50000       50000                  65119.2         99996  1495159  1500000    4841 0.00322733               0             0     2346   1085778   3159912   4615554                                                                LOSS            0
aggregate rate_200000    all      all 200000 1000000 100000      100000                   128471        199453  2991134  3000000    8866 0.00295533               0             0     2702   7112627  12013756  13134373                                          ramp             SATURATED            0
aggregate rate_300000    all      all 300000 1500000 150000      150000                   188244        297444  4461468  4500000   38532 0.00856267               3             2     3106  23107621  27419295  30337544                                          ramp        SATURATED|LOSS            1
aggregate rate_400000    all      all 400000 2000000 200000      200000                   196851        315390  4847116  6000000 1152884   0.192147              15         15979    85569 166737790 199343963 199826429                                     ramp|rate SATURATED|LOSS|FREEZE            3
aggregate rate_500000    all      all 500000 2500000 250000      250000                   305130        498911  6764831  7500000  735169  0.0980225              10             4    18377 109402110 128360751 131074777                                          ramp SATURATED|LOSS|FREEZE            2
aggregate rate_600000    all      all 600000 3000000 300000      300000                   189918        306649  4766376  9000000 4233624   0.470403              32        255685  2296150 178614398 198284119 200770349                                     ramp|rate SATURATED|LOSS|FREEZE            2
~~~~

## Baseline

Highest offered rate with aggregate row not saturated: 100000

Achieved rate at that point: 99995.99967201214 messages per second

Aggregate p50 at that point: 2346.0 ns

Aggregate p99 at that point: 1085778.4199999983 ns

Aggregate p99.9 at that point: 3159912.386000003 ns