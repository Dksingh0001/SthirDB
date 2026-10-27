### Anomaly interception — unconstrained, 8 classes x 50

| class | constraint | injected | blocked | accepted | interception % |
|---|---|---:|---:|---:|---:|
| duplicate_email | UNIQUE violation | 50 | 0 | 50 | 0.0% |
| negative_stock | CHECK violation | 50 | 0 | 50 | 0.0% |
| missing_fk | FOREIGN KEY violation | 50 | 0 | 50 | 0.0% |
| zero_quantity | CHECK violation | 50 | 0 | 50 | 0.0% |
| duplicate_pk | PRIMARY KEY violation | 50 | 0 | 50 | 0.0% |
| orphan_line_item | FOREIGN KEY violation | 50 | 0 | 50 | 0.0% |
| null_not_null | NOT NULL violation | 50 | 0 | 50 | 0.0% |
| restrict_delete | ON DELETE RESTRICT violation | 50 | 0 | 50 | 0.0% |
| TOTAL | - | 400 | 0 | 400 | 0.0% |

_generated in 427 ms_
