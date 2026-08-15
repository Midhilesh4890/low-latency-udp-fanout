# Stage 7 Rate Sweep

## Short sweep

Command:

~~~~text
benchmark/sweep_rate.sh --rates 50000,100000 --repeats 2 --count 200000
~~~~

Run root:

~~~~text
benchmark/results/rate_sweep/20260815T200818Z
~~~~

Directory tree:

~~~~text
benchmark/results/rate_sweep/20260815T200818Z
benchmark/results/rate_sweep/20260815T200818Z/rate_100000
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/run.json
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local/consumer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local/latency.csv
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local/producer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local/receiver.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_1/rx_local/sender.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/run.json
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local/consumer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local/latency.csv
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local/producer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local/receiver.log
benchmark/results/rate_sweep/20260815T200818Z/rate_100000/rep_2/rx_local/sender.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/run.json
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local/consumer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local/latency.csv
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local/producer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local/receiver.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_1/rx_local/sender.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/run.json
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local/consumer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local/latency.csv
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local/producer.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local/receiver.log
benchmark/results/rate_sweep/20260815T200818Z/rate_50000/rep_2/rx_local/sender.log
benchmark/results/rate_sweep/20260815T200818Z/summary.csv
benchmark/results/rate_sweep/20260815T200818Z/sweep.json
~~~~

summarize.py output:

~~~~text
 row_type      config repeat receiver   rate  count warmup skip_warmup wall_clock_received_rate received expected dropped drop_rate  p50   p99     max ramp_ratio ramp_slope_ns flags freeze_count
      run rate_100000      1 rx_local 100000 200000  20000       20000                  85091.5   200000   200000       0         0 1763 33585  915939    1.06506    -0.0078393    OK             
      run rate_100000      2 rx_local 100000 200000  20000       20000                  85045.2   200000   200000       0         0 1725 31127 1189131    0.98311    0.00596741    OK             
      run  rate_50000      1 rx_local  50000 200000  20000       20000                  43915.3   200000   200000       0         0 1761 25843 1083519    1.01733   -0.00798228    OK             
      run  rate_50000      2 rx_local  50000 200000  20000       20000                  43999.2   200000   200000       0         0 1771 29657 1164711    1.03736   0.000516513    OK             
aggregate rate_100000    all      all 100000 200000  20000       20000                            400000   400000       0         0 1744 32356 1052535                             OK            0
aggregate  rate_50000    all      all  50000 200000  20000       20000                            400000   400000       0         0 1766 27750 1124115                             OK            0
~~~~

## Full sweep

Command:

~~~~text
benchmark/sweep_rate.sh
~~~~

Run root:

~~~~text
benchmark/results/rate_sweep/20260815T200848Z
~~~~

summarize.py output:

~~~~text
 row_type       config repeat receiver    rate  count warmup skip_warmup wall_clock_received_rate received expected dropped  drop_rate      p50      p99      max  ramp_ratio ramp_slope_ns       flags freeze_count
      run  rate_100000      1 rx_local  100000 200000  20000       20000                  85090.3   200000   200000       0          0     1768    55362  1321500    0.927213  -0.000949169          OK             
      run  rate_100000      2 rx_local  100000 200000  20000       20000                    85209   200000   200000       0          0     1753    24861   717323    0.910512   -0.00386711          OK             
      run  rate_100000      3 rx_local  100000 200000  20000       20000                  85155.4   200000   200000       0          0     1742    28063   749342    0.943355     0.0023644          OK             
      run rate_1000000      1 rx_local 1000000 200000  20000       20000                   495613   200000   200000       0          0 34703882 38659185 38927837     1.22048        42.129          OK             
      run rate_1000000      2 rx_local 1000000 200000  20000       20000                   478497   200000   200000       0          0 44160236 59335020 59565796     1.59507       136.416          OK             
      run rate_1000000      3 rx_local 1000000 200000  20000       20000                   491867   200000   200000       0          0 35180106 43735834 43946137     1.43999       98.6382          OK             
      run  rate_150000      1 rx_local  150000 200000  20000       20000                   124080   200000   200000       0          0     1726    37599  1032220    0.916994  -0.000829079          OK             
      run  rate_150000      2 rx_local  150000 200000  20000       20000                   123810   200000   200000       0          0     1736    31090   733876     1.00593   -0.00771411          OK             
      run  rate_150000      3 rx_local  150000 200000  20000       20000                  53341.8   194083   200000    5917   0.029585     1818  8112314 12812996  0.00157302      -8.12376 LOSS|FREEZE             
      run  rate_200000      1 rx_local  200000 200000  20000       20000                   159043   200000   200000       0          0     1773  1574138  2975018    0.937989      -1.27985          OK             
      run  rate_200000      2 rx_local  200000 200000  20000       20000                   160742   200000   200000       0          0     1671    83314  1118837     0.92446     0.0119026          OK             
      run  rate_200000      3 rx_local  200000 200000  20000       20000                   159824   200000   200000       0          0     1706   212370  2270476    0.977421      -0.12339          OK             
      run   rate_25000      1 rx_local   25000 200000  20000       20000                  22343.8   200000   200000       0          0     1783    54245 11884130     1.00056      0.198393          OK             
      run   rate_25000      2 rx_local   25000 200000  20000       20000                  22358.3   200000   200000       0          0     1787    26924  1283213     0.99327   -0.00286589          OK             
      run   rate_25000      3 rx_local   25000 200000  20000       20000                  22359.6   200000   200000       0          0     1776    22131  5709400     1.01707    -0.0457935          OK             
      run  rate_300000      1 rx_local  300000 200000  20000       20000                   227682   200000   200000       0          0     1737    51441   552589    0.983146   -0.00845931          OK             
      run  rate_300000      2 rx_local  300000 200000  20000       20000                   225523   200000   200000       0          0     1723   276009  1442206    0.957373    -0.0020678          OK             
      run  rate_300000      3 rx_local  300000 200000  20000       20000                   227080   200000   200000       0          0     1681    75789  1301785    0.959011    -0.0636006          OK             
      run  rate_400000      1 rx_local  400000 200000  20000       20000                   287267   200000   200000       0          0     1737  1457662  2889840     1.10961     -0.609761          OK             
      run  rate_400000      2 rx_local  400000 200000  20000       20000                   288310   200000   200000       0          0     1690   505980  1777551     0.95976      0.286531          OK             
      run  rate_400000      3 rx_local  400000 200000  20000       20000                   289735   200000   200000       0          0     1747  6361841  8710299    0.872487       -4.7229          OK             
      run   rate_50000      1 rx_local   50000 200000  20000       20000                  43934.1   200000   200000       0          0     1727    35876  1594213     1.02331    -0.0179567          OK             
      run   rate_50000      2 rx_local   50000 200000  20000       20000                  43994.1   200000   200000       0          0     1779    42646  2831414     1.04593     0.0731954          OK             
      run   rate_50000      3 rx_local   50000 200000  20000       20000                  43897.4   200000   200000       0          0     1900    26873  1214246     1.29954   -0.00856579          OK             
      run  rate_600000      1 rx_local  600000 200000  20000       20000                   388601   200000   200000       0          0     1790 19239411 19901257 0.000102161      -66.9328      FREEZE             
      run  rate_600000      2 rx_local  600000 200000  20000       20000                   392116   200000   200000       0          0     1970 18559472 19314158 0.000132873      -68.1622      FREEZE             
      run  rate_600000      3 rx_local  600000 200000  20000       20000                   388753   200000   200000       0          0     1769 11665158 12465453 0.000171807      -27.1632      FREEZE             
      run  rate_800000      1 rx_local  800000 200000  20000       20000                   475583   200000   200000       0          0 10650435 22979693 23268663   0.0331839      -136.277      FREEZE             
      run  rate_800000      2 rx_local  800000 200000  20000       20000                    82236   199739   200000     261   0.001305  7895413 19194538 19375852    0.437505      -64.5884          OK             
      run  rate_800000      3 rx_local  800000 200000  20000       20000                   464369   200000   200000       0          0    37521 10295674 10886101 0.000204149      -29.1424      FREEZE             
aggregate  rate_100000    all      all  100000 200000  20000       20000                            600000   600000       0          0     1753    28063   749342                                    OK            0
aggregate rate_1000000    all      all 1000000 200000  20000       20000                            600000   600000       0          0 35180106 43735834 43946137                                    OK            0
aggregate  rate_150000    all      all  150000 200000  20000       20000                            594083   600000    5917 0.00986167     1736    37599  1032220                                  LOSS            1
aggregate  rate_200000    all      all  200000 200000  20000       20000                            600000   600000       0          0     1706   212370  2270476                                    OK            0
aggregate   rate_25000    all      all   25000 200000  20000       20000                            600000   600000       0          0     1783    26924  5709400                                    OK            0
aggregate  rate_300000    all      all  300000 200000  20000       20000                            600000   600000       0          0     1723    75789  1301785                                    OK            0
aggregate  rate_400000    all      all  400000 200000  20000       20000                            600000   600000       0          0     1737  1457662  2889840                                    OK            0
aggregate   rate_50000    all      all   50000 200000  20000       20000                            600000   600000       0          0     1779    35876  1594213                                    OK            0
aggregate  rate_600000    all      all  600000 200000  20000       20000                            600000   600000       0          0     1790 18559472 19314158                                FREEZE            3
aggregate  rate_800000    all      all  800000 200000  20000       20000                            599739   600000     261   0.000435  7895413 19194538 19375852                                FREEZE            2
~~~~

## Deliverable

First aggregate saturated true rate: none observed

First aggregate drop_rate greater than one percent: none observed