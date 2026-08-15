# Stage 7 Baseline

## Problem 1

benchmark/results/rate_sweep/20260815T205658Z latency CSV files contain only seq,latency_ns. They do not contain send_ts_ns, and run.json plus the process logs do not persist first or last send timestamps.

summarize.py now computes achieved_rate from post-warmup send_ts_ns when that column is present and drives the rate saturation trigger from that value. For existing two-column CSVs, achieved_rate is blank and the rate trigger does not fire. wall_clock_received_rate remains visible as a separate column.

Full table after re-running:

~~~~text
 row_type      config repeat receiver   rate   count warmup skip_warmup wall_clock_received_rate achieved_rate received expected dropped   drop_rate      p50      p99    p99_9        max ramp_ratio ramp_slope_ns saturation_triggers                 flags freeze_count
      run rate_100000      1 rx_local 100000  500000  50000       50000                  64979.9                 498913   500000    1087    0.002174     1735 20860390 28603623   28653590   0.962156       -1.5917                                        OK             
      run rate_100000      2 rx_local 100000  500000  50000       50000                  88092.9                 500000   500000       0           0     1737    31935   677051    2985912    1.02041    -0.0143473                                        OK             
      run rate_100000      3 rx_local 100000  500000  50000       50000                  88016.7                 500000   500000       0           0     1747    32693   240808    1182811    1.04673  -5.58904e-05                                        OK             
      run rate_200000      1 rx_local 200000 1000000 100000      100000                   129880                 999758  1000000     242    0.000242     1697    47117   730335    2646292    1.01656   -0.00466532                                        OK             
      run rate_200000      2 rx_local 200000 1000000 100000      100000                   175703                1000000  1000000       0           0     1703    39136   902515    2676754   0.998833    -0.0060815                                        OK             
      run rate_200000      3 rx_local 200000 1000000 100000      100000                   176050                1000000  1000000       0           0     1695   211890  6495706    7158707   0.987486      -0.09544                                        OK             
      run rate_300000      1 rx_local 300000 1500000 150000      150000                   261755                1500000  1500000       0           0     1732    57323   870294    4973588    1.26679    0.00833988                                        OK             
      run rate_300000      2 rx_local 300000 1500000 150000      150000                   194180                1498066  1500000    1934  0.00128933     2028 23130911 36957652   39429944    1.67803       1.68652                                        OK             
      run rate_300000      3 rx_local 300000 1500000 150000      150000                   194413                1499989  1500000      11 7.33333e-06     1950    80859  1019588    2725169    1.35163   -0.00290084                                        OK             
      run rate_400000      1 rx_local 400000 2000000 200000      200000                   258347                1999948  2000000      52     2.6e-05     1994   222443  1628156    2946416     1.2954    -0.0114144                                        OK             
      run rate_400000      2 rx_local 400000 2000000 200000      200000                   346915                2000000  2000000       0           0     1943   171131  2691431    4178254    1.35543   -0.00144336                                        OK             
      run rate_400000      3 rx_local 400000 2000000 200000      200000                   258051                1999330  2000000     670    0.000335     2078   135410  1087619    2269226    1.49818    0.00274244                                        OK             
      run rate_500000      1 rx_local 500000 2500000 250000      250000                   320143                2491325  2500000    8675     0.00347     2084  8947543 21206790   25985517    1.35168     -0.421895                                        OK             
      run rate_500000      2 rx_local 500000 2500000 250000      250000                   319993                2496010  2500000    3990    0.001596     2135  1449067  5268551    5464694    1.57901     0.0673244                                        OK             
      run rate_500000      3 rx_local 500000 2500000 250000      250000                   320523                2492283  2500000    7717   0.0030868     2085 25361160 40425454   46346461     1.3171     -0.605276                                        OK             
      run rate_600000      1 rx_local 600000 3000000 300000      300000                   383524                2998447  3000000    1553 0.000517667     2174  8439029 11896067   12347363    1.69348      0.393484                                    FREEZE             
      run rate_600000      2 rx_local 600000 3000000 300000      300000                   382664                2999848  3000000     152 5.06667e-05     2136  4316555 15665045   20200312     11.914      0.284489                ramp      SATURATED|FREEZE             
      run rate_600000      3 rx_local 600000 3000000 300000      300000                   383937                2998943  3000000    1057 0.000352333     2357  5873332  7459254    9807806    52.1536      0.413513                ramp      SATURATED|FREEZE             
      run rate_800000      1 rx_local 800000 4000000 400000      400000                   308929                2450372  4000000 1549628    0.387407  1186911 80992902 81921889 4294970451    195.582       27.0163                ramp SATURATED|LOSS|FREEZE             
      run rate_800000      2 rx_local 800000 4000000 400000      400000                   318543                2507018  4000000 1492982    0.373246   568400 79051783 81921410   82036632    368.365       16.5856                ramp SATURATED|LOSS|FREEZE             
      run rate_800000      3 rx_local 800000 4000000 400000      400000                   104792                 822414  4000000 3177586    0.794396 68800252 91171312 93360405   93662103    1.45018       26.7653                                      LOSS             
aggregate rate_100000    all      all 100000  500000  50000       50000                  88016.7                1498913  1500000    1087 0.000724667     1737    32693   677051    2985912                                                                 OK            0
aggregate rate_200000    all      all 200000 1000000 100000      100000                   175703                2999758  3000000     242 8.06667e-05     1697    47117   902515    2676754                                                                 OK            0
aggregate rate_300000    all      all 300000 1500000 150000      150000                   194413                4498055  4500000    1945 0.000432222     1950    80859  1019588    4973588                                                                 OK            0
aggregate rate_400000    all      all 400000 2000000 200000      200000                   258347                5999278  6000000     722 0.000120333     1994   171131  1628156    2946416                                                                 OK            0
aggregate rate_500000    all      all 500000 2500000 250000      250000                   320143                7479618  7500000   20382   0.0027176     2085  8947543 21206790   25985517                                                                 OK            0
aggregate rate_600000    all      all 600000 3000000 300000      300000                   383524                8997238  9000000    2762 0.000306889     2174  5873332 11896067   12347363                                         ramp      SATURATED|FREEZE            3
aggregate rate_800000    all      all 800000 4000000 400000      400000                   308929                5779804 12000000 6220196     0.51835  1186911 80992902 81921889   93662103                                         ramp SATURATED|LOSS|FREEZE            2
~~~~

## Problem 2

Overflow investigation for benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_1/rx_local/latency.csv:

~~~~text
target benchmark/results/rate_sweep/20260815T205658Z/rate_800000/rep_1/rx_local/latency.csv
rows 2850372
ten largest latency_ns values
seq=3161360 latency_ns=4294970451 delta_from_2^32=3155
seq=3164081 latency_ns=4294969861 delta_from_2^32=2565
seq=4399626 latency_ns=88225067 delta_from_2^32=-4206742229
seq=4399627 latency_ns=88224407 delta_from_2^32=-4206742889
seq=4399628 latency_ns=88223520 delta_from_2^32=-4206743776
seq=4399629 latency_ns=88222734 delta_from_2^32=-4206744562
seq=4399630 latency_ns=88221743 delta_from_2^32=-4206745553
seq=4399631 latency_ns=88220817 delta_from_2^32=-4206746479
seq=4399632 latency_ns=88219872 delta_from_2^32=-4206747424
seq=4399633 latency_ns=88218886 delta_from_2^32=-4206748410
ten smallest latency_ns values
seq=1313839 latency_ns=1122 delta_from_2^32=-4294966174
seq=1376821 latency_ns=1145 delta_from_2^32=-4294966151
seq=1303980 latency_ns=1147 delta_from_2^32=-4294966149
seq=1454626 latency_ns=1152 delta_from_2^32=-4294966144
seq=1315602 latency_ns=1153 delta_from_2^32=-4294966143
seq=1301032 latency_ns=1154 delta_from_2^32=-4294966142
seq=255552 latency_ns=1155 delta_from_2^32=-4294966141
seq=256797 latency_ns=1155 delta_from_2^32=-4294966141
seq=1761162 latency_ns=1157 delta_from_2^32=-4294966139
seq=283042 latency_ns=1163 delta_from_2^32=-4294966133
negative_count=0
count_gt_2^32=2
count_within_1000_of_2^32=0
other runs near 2^32
none
~~~~

The two values above 2^32 are isolated, boundary-adjacent outliers. No negative values were present, no values were within 1000 ns of 2^32, and no other run in this sweep had values near or above that boundary.

This looks like numeric wraparound or timestamp corruption, not a genuine multi-second transport stall. A real 4.295 second stall should produce a broader neighborhood of multi-second latencies rather than two isolated samples followed by an immediate return to sub-100 ms tail values.

## Baseline

A trustworthy corrected baseline cannot be stated from the existing artifacts because the requested steady-state achieved-rate measurement requires send timestamps that were not recorded, and the high-rate data contains evidence consistent with numeric wraparound. Per instruction, I am reporting the wraparound finding and stopping before treating these measurements as a Phase 2 baseline.