# Stage 7 Corrected Rate Sweep

## Detector Fix Verification

Command:

~~~~text
benchmark/summarize.py benchmark/results/rate_sweep
~~~~

Relevant aggregate rows:

~~~~text
20260815T200249Z/rate_1000000 rate=1000000 saturated=True triggers=ramp|rate
20260815T200249Z/rate_600000 rate=600000 saturated=True triggers=rate
20260815T200249Z/rate_800000 rate=800000 saturated=True triggers=rate
20260815T200848Z/rate_1000000 rate=1000000 saturated=True triggers=rate
20260815T200848Z/rate_600000 rate=600000 saturated=True triggers=rate
20260815T200848Z/rate_800000 rate=800000 saturated=True triggers=rate
20260815T205658Z/rate_600000 rate=600000 saturated=True triggers=ramp|rate
20260815T205658Z/rate_800000 rate=800000 saturated=True triggers=ramp|rate
bad_count 0
~~~~

## Time-Based Sweep Plan

Defaults used: warmup-seconds=0.5, duration-seconds=5.0, repeats=3, slots=65536.

Computed per-rate counts before execution:

~~~~text
rate=100000 count=500000 warmup=50000
rate=200000 count=1000000 warmup=100000
rate=300000 count=1500000 warmup=150000
rate=400000 count=2000000 warmup=200000
rate=500000 count=2500000 warmup=250000
rate=600000 count=3000000 warmup=300000
rate=800000 count=4000000 warmup=400000
~~~~

## Corrected Re-Sweep

Command:

~~~~text
benchmark/sweep_rate.sh --rates 100000,200000,300000,400000,500000,600000,800000
~~~~

Run root:

~~~~text
benchmark/results/rate_sweep/20260815T205658Z
~~~~

Completed runs: 21
Failures: []

summarize.py output:

~~~~text
 row_type      config repeat receiver   rate   count warmup skip_warmup wall_clock_received_rate received expected dropped   drop_rate      p50      p99    p99_9        max ramp_ratio ramp_slope_ns saturation_triggers                 flags freeze_count
      run rate_100000      1 rx_local 100000  500000  50000       50000                  64979.9   498913   500000    1087    0.002174     1735 20860390 28603623   28653590   0.962156       -1.5917                rate             SATURATED             
      run rate_100000      2 rx_local 100000  500000  50000       50000                  88092.9   500000   500000       0           0     1737    31935   677051    2985912    1.02041    -0.0143473                                        OK             
      run rate_100000      3 rx_local 100000  500000  50000       50000                  88016.7   500000   500000       0           0     1747    32693   240808    1182811    1.04673  -5.58904e-05                                        OK             
      run rate_200000      1 rx_local 200000 1000000 100000      100000                   129880   999758  1000000     242    0.000242     1697    47117   730335    2646292    1.01656   -0.00466532                rate             SATURATED             
      run rate_200000      2 rx_local 200000 1000000 100000      100000                   175703  1000000  1000000       0           0     1703    39136   902515    2676754   0.998833    -0.0060815                                        OK             
      run rate_200000      3 rx_local 200000 1000000 100000      100000                   176050  1000000  1000000       0           0     1695   211890  6495706    7158707   0.987486      -0.09544                                        OK             
      run rate_300000      1 rx_local 300000 1500000 150000      150000                   261755  1500000  1500000       0           0     1732    57323   870294    4973588    1.26679    0.00833988                                        OK             
      run rate_300000      2 rx_local 300000 1500000 150000      150000                   194180  1498066  1500000    1934  0.00128933     2028 23130911 36957652   39429944    1.67803       1.68652                rate             SATURATED             
      run rate_300000      3 rx_local 300000 1500000 150000      150000                   194413  1499989  1500000      11 7.33333e-06     1950    80859  1019588    2725169    1.35163   -0.00290084                rate             SATURATED             
      run rate_400000      1 rx_local 400000 2000000 200000      200000                   258347  1999948  2000000      52     2.6e-05     1994   222443  1628156    2946416     1.2954    -0.0114144                rate             SATURATED             
      run rate_400000      2 rx_local 400000 2000000 200000      200000                   346915  2000000  2000000       0           0     1943   171131  2691431    4178254    1.35543   -0.00144336                                        OK             
      run rate_400000      3 rx_local 400000 2000000 200000      200000                   258051  1999330  2000000     670    0.000335     2078   135410  1087619    2269226    1.49818    0.00274244                rate             SATURATED             
      run rate_500000      1 rx_local 500000 2500000 250000      250000                   320143  2491325  2500000    8675     0.00347     2084  8947543 21206790   25985517    1.35168     -0.421895                rate             SATURATED             
      run rate_500000      2 rx_local 500000 2500000 250000      250000                   319993  2496010  2500000    3990    0.001596     2135  1449067  5268551    5464694    1.57901     0.0673244                rate             SATURATED             
      run rate_500000      3 rx_local 500000 2500000 250000      250000                   320523  2492283  2500000    7717   0.0030868     2085 25361160 40425454   46346461     1.3171     -0.605276                rate             SATURATED             
      run rate_600000      1 rx_local 600000 3000000 300000      300000                   383524  2998447  3000000    1553 0.000517667     2174  8439029 11896067   12347363    1.69348      0.393484                rate      SATURATED|FREEZE             
      run rate_600000      2 rx_local 600000 3000000 300000      300000                   382664  2999848  3000000     152 5.06667e-05     2136  4316555 15665045   20200312     11.914      0.284489           ramp|rate      SATURATED|FREEZE             
      run rate_600000      3 rx_local 600000 3000000 300000      300000                   383937  2998943  3000000    1057 0.000352333     2357  5873332  7459254    9807806    52.1536      0.413513           ramp|rate      SATURATED|FREEZE             
      run rate_800000      1 rx_local 800000 4000000 400000      400000                   308929  2450372  4000000 1549628    0.387407  1186911 80992902 81921889 4294970451    195.582       27.0163           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_800000      2 rx_local 800000 4000000 400000      400000                   318543  2507018  4000000 1492982    0.373246   568400 79051783 81921410   82036632    368.365       16.5856           ramp|rate SATURATED|LOSS|FREEZE             
      run rate_800000      3 rx_local 800000 4000000 400000      400000                   104792   822414  4000000 3177586    0.794396 68800252 91171312 93360405   93662103    1.45018       26.7653                rate        SATURATED|LOSS             
aggregate rate_100000    all      all 100000  500000  50000       50000                  88016.7  1498913  1500000    1087 0.000724667     1737    32693   677051    2985912                                                                 OK            0
aggregate rate_200000    all      all 200000 1000000 100000      100000                   175703  2999758  3000000     242 8.06667e-05     1697    47117   902515    2676754                                                                 OK            0
aggregate rate_300000    all      all 300000 1500000 150000      150000                   194413  4498055  4500000    1945 0.000432222     1950    80859  1019588    4973588                                         rate             SATURATED            0
aggregate rate_400000    all      all 400000 2000000 200000      200000                   258347  5999278  6000000     722 0.000120333     1994   171131  1628156    2946416                                         rate             SATURATED            0
aggregate rate_500000    all      all 500000 2500000 250000      250000                   320143  7479618  7500000   20382   0.0027176     2085  8947543 21206790   25985517                                         rate             SATURATED            0
aggregate rate_600000    all      all 600000 3000000 300000      300000                   383524  8997238  9000000    2762 0.000306889     2174  5873332 11896067   12347363                                    ramp|rate      SATURATED|FREEZE            3
aggregate rate_800000    all      all 800000 4000000 400000      400000                   308929  5779804 12000000 6220196     0.51835  1186911 80992902 81921889   93662103                                    ramp|rate SATURATED|LOSS|FREEZE            2
~~~~

## Corrected Baseline

Highest offered rate with aggregate row not saturated: 200000

Achieved rate at that point: 175703.1171333459 messages per second

Aggregate p50 at that point: 1697 ns

Aggregate p99 at that point: 47117.29999999935 ns

Aggregate p99.9 at that point: 902514.6060000993 ns