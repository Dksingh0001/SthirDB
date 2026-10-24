#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "catalog/catalog.h"
#include "parser/ast.h"

namespace sthir {

// Index keys are int64 (Phase 3 simplification, see docs/report.md §4).
// INT columns use the value itself; DECIMAL columns are indexed as cents
// (value * kIndexDecimalScale) so a double column still maps to an integer key.
int64_t IndexKeyFromInt(int32_t value);
int64_t IndexKeyFromDecimal(double value);

// Multiplier between a column value and its index key.
int64_t IndexKeyScale(DataType type);

// Phase 3 indexes INT and DECIMAL columns; VARCHAR needs a different key type.
bool IsIndexableColumn(DataType type);

// Index file for a table+index, e.g. ./data/retail/items.dat + idx_price ->
// ./data/retail/items.idx_price.idx
std::string IndexFilePath(const TableSchema& schema, const std::string& index_name);

// Inclusive key range derived from a WHERE predicate.
//
// Bounds are deliberately widened by one key unit: the executor rechecks the
// original predicate on every fetched tuple, so the range only has to be a
// superset of matching rows. That absorbs decimal scaling and float rounding
// instead of letting them drop rows.
struct KeyRange {
  bool has_lo = false;
  int64_t lo = 0;
  bool has_hi = false;
  int64_t hi = 0;
  bool IsEmpty() const { return has_lo && has_hi && lo > hi; }
};

// True when `expression` only depends on the indexed column and yields bounds.
// Recognises `col OP literal`, `literal OP col`, and ANDs of those; anything
// else (OR, NOT, functions) is left to the filter.
bool ExtractKeyRange(const Expr* predicate, const TableSchema& schema, size_t key_column,
                     KeyRange* out);

}  // namespace sthir
