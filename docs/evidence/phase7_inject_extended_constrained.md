### Anomaly interception — constrained, 8 classes x 50

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 50 | 50 | 0 | 100.0% |
| negative_stock | CHECK violation | 50 | 50 | 0 | 100.0% |
| missing_fk | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| zero_quantity | CHECK violation | 50 | 50 | 0 | 100.0% |
| duplicate_pk | PRIMARY KEY violation | 50 | 50 | 0 | 100.0% |
| orphan_line_item | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| null_not_null | NOT NULL violation | 50 | 50 | 0 | 100.0% |
| restrict_delete | ON DELETE RESTRICT violation | 50 | 50 | 0 | 100.0% |
| TOTAL | - | 400 | 400 | 0 | 100.0% |

_generated in 659 ms_
