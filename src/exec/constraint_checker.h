#pragma once

// Phase 7 — constraint enforcement.
//
// The catalog has *declared* constraints since Phase 2; this component makes
// them *enforced* on every INSERT / UPDATE / DELETE, and it is the single
// switch behind `SET constraints_enabled = on|off`: when the flag is off the
// checker short-circuits and the unconstrained baseline accepts the same
// statements a schema without constraints would.
//
// Enforcement uses cached key sets instead of table scans per statement:
//   * uniqueness groups (PRIMARY KEY first, then every UNIQUE group) are
//     cached as composite key strings and checked in O(1);
//   * foreign keys are checked against the cached key set of the referenced
//     columns;
//   * ON DELETE RESTRICT looks at the cached key set of the *child* columns;
//   * CHECK and NOT NULL are evaluated on the row itself (no cache).
// Sets are loaded lazily (one scan of the table, via the engine's KeySetLoader)
// and maintained on successful writes; any abort or rollback invalidates them
// so a rolled-back write can never leave a stale key behind.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "catalog/catalog.h"
#include "exec/value.h"
#include "parser/ast.h"

namespace sthir {

class ConstraintChecker {
 public:
  // Loads every key of `columns` in `table` by scanning it once (engine side;
  // uses the same scan semantics as SeqScanExecutor). Values map to their
  // occurrence count — FK child sets are multisets (many purchases share one
  // shopper), so maintenance must be able to hit exactly zero.
  using KeySetLoader = std::function<bool(const std::string& table,
                                          const std::vector<std::string>& columns,
                                          std::unordered_map<std::string, uint32_t>* out,
                                          std::string* err)>;
  // Deletes the rows of `table` whose `columns` equal `key` (ON DELETE CASCADE).
  using KeyDeleter = std::function<size_t(const std::string& table,
                                          const std::vector<std::string>& columns,
                                          const std::vector<Value>& key,
                                          uint32_t txn_id, std::string* err)>;

  ConstraintChecker(Catalog* catalog, KeySetLoader loader, KeyDeleter deleter);

  // The schema flag: false short-circuits every check (unconstrained run).
  // Toggling invalidates the caches so a later re-enable re-reads reality.
  void SetEnabled(bool enabled);
  bool enabled() const;

  // Drops all cached key sets and derived rule metadata.
  void InvalidateAll();

  // INSERT: checks all rows (NOT NULL, CHECK, PRIMARY KEY/UNIQUE incl.
  // duplicates inside the same statement, FK existence) before any write.
  bool CheckInsert(const TableSchema& schema, const std::vector<Tuple>& rows,
                   std::string* err);
  // Cache maintenance after a row was successfully written.
  void RecordInsert(const TableSchema& schema, const Tuple& row);

  // UPDATE: NOT NULL / CHECK on the new row, uniqueness on every key that
  // actually changed, FK existence on every FK value that changed.
  bool CheckUpdate(const TableSchema& schema, const Tuple& old_row,
                   const Tuple& new_row, std::string* err);
  void RecordUpdate(const TableSchema& schema, const Tuple& old_row,
                    const Tuple& new_row);

  // DELETE: ON DELETE RESTRICT — rejects the row while a child references it.
  bool CheckDelete(const TableSchema& schema, const Tuple& row, std::string* err);
  void RecordDelete(const TableSchema& schema, const Tuple& row);

  // ON DELETE CASCADE: deletes the referencing child rows after the parent
  // row went away. Returns false (with `err` set) when a child delete fails;
  // the caller aborts the transaction, which also invalidates the caches.
  bool CascadeDelete(const TableSchema& schema, const Tuple& row, uint32_t txn_id,
                     std::string* err);

  // Error-message markers — the anomaly injector classifies blocks by these.
  static constexpr const char* kPrimaryKeyViolation = "PRIMARY KEY violation";
  static constexpr const char* kUniqueViolation = "UNIQUE violation";
  static constexpr const char* kForeignKeyViolation = "FOREIGN KEY violation";
  static constexpr const char* kCheckViolation = "CHECK violation";
  static constexpr const char* kNotNullViolation = "NOT NULL violation";
  static constexpr const char* kRestrictViolation = "ON DELETE RESTRICT violation";

  // Composite key string for (schema, columns) over one row. Returns false
  // when any component is NULL (NULL keys are never stored or compared).
  static bool KeyOf(const TableSchema& schema, const std::vector<std::string>& columns,
                    const Tuple& row, std::string* out);

  // One (cache-id, key) pair a row contributes to the key sets.
  struct RowKey {
    std::string id;
    std::string key;
  };
  // All key-set entries a row contributes: its uniqueness groups, its FK
  // child-column sets, and its parent-key sets — deduplicated by id so a row
  // counts exactly once per set (matching how the loader counts rows).

  // Cascade targets of a parent row: (child_table, child_columns, key_values)
  // for every ON DELETE CASCADE edge pointing at this table.
  struct CascadeTarget {
    std::string child_table;
    std::vector<std::string> child_columns;
    std::vector<Value> key;
  };
  std::vector<CascadeTarget> CascadeTargets(const TableSchema& schema, const Tuple& row);

 private:
  // Everything the checker must know about one table; derived from the catalog
  // and cached until InvalidateAll().
  struct TableRules {
    bool any = false;
    std::vector<std::vector<std::string>> unique_groups;  // PRIMARY KEY first
    std::vector<std::string> not_null_columns;           // incl. PK columns
    std::vector<CheckConstraint> checks;
    std::vector<ForeignKeyInfo> child_edges;   // FKs whose child is this table
    std::vector<ForeignKeyInfo> parent_edges;  // FKs whose parent is this table
  };

  TableRules BuildRules(const TableSchema& schema) const;
  std::vector<RowKey> KeysForRow(const TableSchema& schema, const TableRules& rules,
                                 const Tuple& row) const;
  // Returns the cached key set for (table, columns), loading it once.
  // Must be called WITHOUT holding mutex_ if loading (it calls loader_).
  bool KeySetFor(const std::string& table, const std::vector<std::string>& columns,
                 std::unordered_map<std::string, uint32_t>** set, std::string* err);
  const Expr* CheckExpression(const std::string& table, const std::string& expression);

  Catalog* catalog_;
  KeySetLoader loader_;
  KeyDeleter deleter_;

  mutable std::mutex mutex_;
  bool enabled_ = true;
  std::unordered_map<std::string, TableRules> rules_;      // lower(table) -> rules
  // "table|col1|col2" -> (key -> occurrence count). Membership == count > 0.
  std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> key_sets_;
  std::unordered_map<std::string, std::unique_ptr<Expr>> check_exprs_;         // "t|expr"
};

}  // namespace sthir
