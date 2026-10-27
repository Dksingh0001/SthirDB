### Constrained vs unconstrained — concurrent query latency

3 iterations per thread; latency = wall time of one Execute (parse + plan + execute).

| mode | threads | queries | failures | avg ms | p99 ms | min ms | max ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| constrained | 100 | 300 | 0 | 2275.0 | 3507.4 | 1140.4 | 3688.7 |
| constrained | 300 | 900 | 0 | 6701.8 | 9111.9 | 2347.0 | 9515.3 |
| constrained | 500 | 1500 | 0 | 11220.2 | 15125.4 | 1770.3 | 15725.2 |
| unconstrained | 100 | 300 | 0 | 2239.1 | 3064.5 | 920.4 | 3141.7 |
| unconstrained | 300 | 900 | 0 | 6750.3 | 9124.9 | 1455.4 | 9442.6 |
| unconstrained | 500 | 1500 | 0 | 10996.9 | 17881.3 | 991.2 | 19152.7 |
