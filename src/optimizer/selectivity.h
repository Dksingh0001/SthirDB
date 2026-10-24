#pragma once

#include <cstdint>

#include "catalog/catalog.h"
#include "parser/ast.h"

namespace sthir {

// Used when a column has no statistics (never analyzed, or a string column):
// 10% for equality, one third for a range, one third for anything unmodelled.
// They are conventional defaults, not measurements (see docs/report.md §5).
inline constexpr double kDefaultEqualitySelectivity = 0.1;
inline constexpr double kDefaultRangeSelectivity = 1.0 / 3.0;

// row_count / NDV, clamped to [0,1].
double EqualitySelectivity(const ColumnStats& stats);

// Fractional histogram overlap x row_count, clamped to [0,1]. Strict and
// non-strict bounds are treated alike: a three-bucket histogram cannot resolve a
// single point, and equality is estimated through NDV instead.
double RangeSelectivity(const ColumnStats& stats, bool has_lo, double lo, bool has_hi,
                        double hi);

// Selectivity of a WHERE expression: comparison against a literal, IS NULL,
// NOT, AND (independence: s1*s2) and OR (inclusion-exclusion: s1+s2-s1*s2).
double EstimateSelectivity(const Expr* predicate, const TableSchema& schema);

// selectivity x row_count, rounded, clamped to [0,row_count]; at least 1 row
// when the selectivity is positive.
uint64_t EstimateRows(double selectivity, uint64_t row_count);

}  // namespace sthir
