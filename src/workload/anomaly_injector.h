#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sthir {

class ExecutionEngine;

// Phase 7B: injects deliberately anomalous statements and records, per class,
// whether the engine blocked them (and with which constraint) or accepted
// them. Runs against the SAME database under `SET constraints_enabled = on`
// (constrained) and `= off` (unconstrained baseline).
//
// Default workload: the three Yuan 2026 classes x 50 = 150 records.
//   duplicate_email  — register a shopper with an email that already exists
//   negative_stock   — update an item's StockLevel below the CHECK floor
//   missing_fk       — insert a purchase whose ShopperID has no parent row
// Extended classes (add with --extended), from the constraint-aware guide:
//   zero_quantity    — line item with Quantity 0 / -1 (CHECK Quantity >= 1)
//   duplicate_pk     — insert a shopper whose PRIMARY KEY already exists
//   orphan_line_item — child line item whose parent purchase does not exist
//   null_not_null    — NULL in the NOT NULL Email column   (added by this project)
//   restrict_delete  — delete a shopper that purchases reference (RESTRICT)
struct InjectionClassDef {
  std::string name;
  std::string constraint;  // marker the blocking error must contain
  std::string target_table;// table whose row count sizes the rate mode
  size_t count = 0;
};

struct InjectConfig {
  // Explicit class list. Empty -> defaults (3 Yuan classes), plus the
  // extended classes when `extended` is set.
  std::vector<InjectionClassDef> classes;
  bool extended = false;
  size_t default_count = 50;   // records per class without --classes/--rate
  int rate_percent = 0;        // 1/5/10: count = rate% of the target table
};

struct InjectionClassResult {
  std::string name;
  std::string constraint;
  size_t injected = 0;
  size_t blocked = 0;
  size_t accepted = 0;
  size_t wrong_constraint = 0;  // blocked by something other than `constraint`
  std::string first_block;      // first blocking error message, for the report
};

struct InjectResult {
  std::vector<InjectionClassResult> classes;
  size_t injected = 0;
  size_t blocked = 0;
  size_t accepted = 0;
  double elapsed_ms = 0.0;
};

// Runs every configured injection through `engine` (statement by statement).
// Returns false only on a harness/engine failure — an "accepted" injection is
// a measurement, not an error.
bool RunAnomalyInjector(ExecutionEngine* engine, const InjectConfig& cfg, InjectResult* out,
                        std::string* err);

// Markdown table (with a mode header line) for docs/evidence and the report.
std::string FormatInjectionTable(const InjectResult& result, const std::string& mode_label);

}  // namespace sthir