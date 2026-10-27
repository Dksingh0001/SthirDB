# Injection-rate sweep — unconstrained [constraints=off]

### Anomaly interception — unconstrained, 3 classes x 50 (rate 1% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 11 | 0 | 11 | 0.0% |
| negative_stock | CHECK violation | 50 | 0 | 50 | 0.0% |
| missing_fk | FOREIGN KEY violation | 1001 | 0 | 1001 | 0.0% |
| TOTAL | - | 1062 | 0 | 1062 | 0.0% |

_generated in 859 ms_

### Anomaly interception — unconstrained, 3 classes x 50 (rate 5% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 55 | 0 | 55 | 0.0% |
| negative_stock | CHECK violation | 250 | 0 | 250 | 0.0% |
| missing_fk | FOREIGN KEY violation | 5055 | 0 | 5055 | 0.0% |
| TOTAL | - | 5360 | 0 | 5360 | 0.0% |

_generated in 4019 ms_

### Anomaly interception — unconstrained, 3 classes x 50 (rate 10% of each target table)

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 116 | 0 | 116 | 0.0% |
| negative_stock | CHECK violation | 500 | 0 | 500 | 0.0% |
| missing_fk | FOREIGN KEY violation | 10615 | 0 | 10615 | 0.0% |
| TOTAL | - | 11231 | 0 | 11231 | 0.0% |

_generated in 8283 ms_

