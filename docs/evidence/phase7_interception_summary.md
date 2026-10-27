### Constrained vs unconstrained — anomaly interception (default 3 x 50)

| mode | class | constraint | injected | blocked | accepted | interception % |
|---|---|---|---:|---:|---:|---:|
| constrained | duplicate_email | UNIQUE violation | 50 | 50 | 0 | 100.0% |
| constrained | negative_stock | CHECK violation | 50 | 50 | 0 | 100.0% |
| constrained | missing_fk | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| unconstrained | duplicate_email | UNIQUE violation | 50 | 0 | 50 | 0.0% |
| unconstrained | negative_stock | CHECK violation | 50 | 0 | 50 | 0.0% |
| unconstrained | missing_fk | FOREIGN KEY violation | 50 | 0 | 50 | 0.0% |

### Constrained vs unconstrained — anomaly interception (8 classes x 50)

| mode | class | constraint | injected | blocked | accepted | interception % |
|---|---|---|---:|---:|---:|---:|
| constrained | duplicate_email | UNIQUE violation | 50 | 50 | 0 | 100.0% |
| constrained | negative_stock | CHECK violation | 50 | 50 | 0 | 100.0% |
| constrained | missing_fk | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| constrained | zero_quantity | CHECK violation | 50 | 50 | 0 | 100.0% |
| constrained | duplicate_pk | PRIMARY KEY violation | 50 | 50 | 0 | 100.0% |
| constrained | orphan_line_item | FOREIGN KEY violation | 50 | 50 | 0 | 100.0% |
| constrained | null_not_null | NOT NULL violation | 50 | 50 | 0 | 100.0% |
| constrained | restrict_delete | ON DELETE RESTRICT violation | 50 | 50 | 0 | 100.0% |
| unconstrained | duplicate_email | UNIQUE violation | 50 | 0 | 50 | 0.0% |
| unconstrained | negative_stock | CHECK violation | 50 | 0 | 50 | 0.0% |
| unconstrained | missing_fk | FOREIGN KEY violation | 50 | 0 | 50 | 0.0% |
| unconstrained | zero_quantity | CHECK violation | 50 | 0 | 50 | 0.0% |
| unconstrained | duplicate_pk | PRIMARY KEY violation | 50 | 0 | 50 | 0.0% |
| unconstrained | orphan_line_item | FOREIGN KEY violation | 50 | 0 | 50 | 0.0% |
| unconstrained | null_not_null | NOT NULL violation | 50 | 0 | 50 | 0.0% |
| unconstrained | restrict_delete | ON DELETE RESTRICT violation | 50 | 0 | 50 | 0.0% |
