# Injection-rate sweep — constrained [constraints=on]

### Anomaly interception — constrained, 3 classes x 50 (rate 1% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 10 | 10 | 0 | 100.0% |
| negative_stock | CHECK violation | 50 | 50 | 0 | 100.0% |
| missing_fk | FOREIGN KEY violation | 1000 | 1000 | 0 | 100.0% |
| TOTAL | - | 1060 | 1060 | 0 | 100.0% |

_generated in 930 ms_

### Anomaly interception — constrained, 3 classes x 50 (rate 5% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 50 | 50 | 0 | 100.0% |
| negative_stock | CHECK violation | 250 | 250 | 0 | 100.0% |
| missing_fk | FOREIGN KEY violation | 5000 | 5000 | 0 | 100.0% |
| TOTAL | - | 5300 | 5300 | 0 | 100.0% |

_generated in 3941 ms_

### Anomaly interception — constrained, 3 classes x 50 (rate 10% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 100 | 100 | 0 | 100.0% |
| negative_stock | CHECK violation | 500 | 500 | 0 | 100.0% |
| missing_fk | FOREIGN KEY violation | 10000 | 10000 | 0 | 100.0% |
| TOTAL | - | 10600 | 10600 | 0 | 100.0% |

_generated in 7862 ms_

