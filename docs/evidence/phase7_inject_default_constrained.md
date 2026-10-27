### Anomaly interception — constrained, 3 classes x 50

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 50 | 50 | 0 | 100.0% |
| negative_stock | CHECK violation | 50 | 50 | 0 | 100.0% |
| missing_fk | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| TOTAL | - | 150 | 150 | 0 | 100.0% |

_generated in 318 ms_
