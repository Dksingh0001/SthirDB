#include "optimizer/selectivity.h"

#include <algorithm>
#include <cmath>

namespace sthir {

namespace {

double Clamp01(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

bool LiteralAsDouble(const Expr* expr, double* out) {
  if (!expr || expr->type() != ExprType::LITERAL) return false;
  const auto* lit = static_cast<const LiteralExpr*>(expr);
  if (lit->lit_type() == LiteralExpr::LitType::INT) {
    *out = static_cast<double>(lit->int_val());
    return true;
  }
  if (lit->lit_type() == LiteralExpr::LitType::DECIMAL) {
    *out = lit->decimal_val();
    return true;
  }
  return false;
}

// Resolves a column reference against the schema (case-insensitive, and
// ignoring a redundant "table." prefix).
const Column* ResolveColumn(const Expr* expr, const TableSchema& schema) {
  if (!expr || expr->type() != ExprType::COLUMN_REF) return nullptr;
  const auto* ref = static_cast<const ColumnRefExpr*>(expr);
  return schema.GetColumn(ref->column_name());
}

const ColumnStats* StatsFor(const Column& column, const TableSchema& schema) {
  const ColumnStats* stats = schema.GetColumnStats(column.name);
  return (stats != nullptr && stats->valid) ? stats : nullptr;
}

double ComparisonSelectivity(const ColumnStats* stats, BinaryOp op, double value) {
  switch (op) {
    case BinaryOp::EQ:
      return stats != nullptr ? EqualitySelectivity(*stats) : kDefaultEqualitySelectivity;
    case BinaryOp::NEQ:
      return 1.0 - (stats != nullptr ? EqualitySelectivity(*stats) : kDefaultEqualitySelectivity);
    case BinaryOp::LT:
    case BinaryOp::LTE:
      if (stats != nullptr && op == BinaryOp::LT && value <= stats->min_value) return 0.0;
      return stats != nullptr ? RangeSelectivity(*stats, false, 0.0, true, value)
                              : kDefaultRangeSelectivity;
    case BinaryOp::GT:
    case BinaryOp::GTE:
      // Strictly beyond the largest value matches nothing.
      if (stats != nullptr && op == BinaryOp::GT && value >= stats->max_value) return 0.0;
      return stats != nullptr ? RangeSelectivity(*stats, true, value, false, 0.0)
                              : kDefaultRangeSelectivity;
    default:
      return kDefaultRangeSelectivity;
  }
}

// Mirrors a comparison when the literal is on the left: `5 < col` is `col > 5`.
BinaryOp MirrorOp(BinaryOp op) {
  switch (op) {
    case BinaryOp::LT: return BinaryOp::GT;
    case BinaryOp::LTE: return BinaryOp::GTE;
    case BinaryOp::GT: return BinaryOp::LT;
    case BinaryOp::GTE: return BinaryOp::LTE;
    default: return op;
  }
}

bool IsComparison(BinaryOp op) {
  return op == BinaryOp::EQ || op == BinaryOp::NEQ || op == BinaryOp::LT ||
         op == BinaryOp::LTE || op == BinaryOp::GT || op == BinaryOp::GTE;
}

double EstimateRec(const Expr* expr, const TableSchema& schema) {
  if (!expr) return 1.0;

  switch (expr->type()) {
    case ExprType::LITERAL:
      return 1.0;
    case ExprType::COLUMN_REF:
      return 1.0;  // a bare column is not a predicate; treat as always true
    case ExprType::UNARY_OP: {
      const auto* unary = static_cast<const UnaryExpr*>(expr);
      if (unary->op() == UnaryOp::NOT) return Clamp01(1.0 - EstimateRec(unary->expr(), schema));
      return kDefaultRangeSelectivity;
    }
    case ExprType::IS_NULL_EXPR: {
      const auto* is_null = static_cast<const IsNullExpr*>(expr);
      const Column* column = ResolveColumn(is_null->expr(), schema);
      const ColumnStats* stats = column != nullptr ? StatsFor(*column, schema) : nullptr;
      double null_fraction = 0.1;  // default when nothing was analyzed
      if (stats != nullptr && stats->row_count + stats->null_count > 0) {
        null_fraction = static_cast<double>(stats->null_count) /
                        static_cast<double>(stats->row_count + stats->null_count);
      }
      return is_null->is_not() ? Clamp01(1.0 - null_fraction) : null_fraction;
    }
    case ExprType::BINARY_OP: {
      const auto* bin = static_cast<const BinaryExpr*>(expr);
      if (bin->op() == BinaryOp::AND) {
        return Clamp01(EstimateRec(bin->left(), schema) * EstimateRec(bin->right(), schema));
      }
      if (bin->op() == BinaryOp::OR) {
        const double s1 = EstimateRec(bin->left(), schema);
        const double s2 = EstimateRec(bin->right(), schema);
        return Clamp01(s1 + s2 - s1 * s2);
      }
      if (!IsComparison(bin->op())) return kDefaultRangeSelectivity;

      double value = 0.0;
      const Column* column = ResolveColumn(bin->left(), schema);
      if (column != nullptr && LiteralAsDouble(bin->right(), &value)) {
        return Clamp01(ComparisonSelectivity(StatsFor(*column, schema), bin->op(), value));
      }
      column = ResolveColumn(bin->right(), schema);
      if (column != nullptr && LiteralAsDouble(bin->left(), &value)) {
        return Clamp01(ComparisonSelectivity(StatsFor(*column, schema), MirrorOp(bin->op()),
                                             value));
      }
      return kDefaultRangeSelectivity;  // column-to-column or an unmodelled operand
    }
    case ExprType::AGGREGATE:
      return 1.0;
  }
  return kDefaultRangeSelectivity;
}

}  // namespace

double EqualitySelectivity(const ColumnStats& stats) {
  if (!stats.valid || stats.ndv == 0 || stats.row_count == 0) return kDefaultEqualitySelectivity;
  return Clamp01(1.0 / static_cast<double>(stats.ndv));
}

double RangeSelectivity(const ColumnStats& stats, bool has_lo, double lo, bool has_hi,
                        double hi) {
  if (!stats.valid || stats.row_count == 0 || stats.histogram.empty()) {
    return kDefaultRangeSelectivity;
  }
  if (has_lo && has_hi && lo > hi) return 0.0;

  const double min_value = stats.min_value;
  const double max_value = stats.max_value;
  if (has_hi && hi < min_value) return 0.0;
  if (has_lo && lo > max_value) return 0.0;

  if (max_value <= min_value) {  // constant column: one bucket holds everything
    const bool inside = (!has_lo || min_value >= lo) && (!has_hi || min_value <= hi);
    return inside ? 1.0 : 0.0;
  }

  // A bound exactly on an endpoint still matches that one value.
  if (has_lo && !has_hi && lo == max_value) return Clamp01(1.0 / static_cast<double>(stats.row_count));
  if (!has_lo && has_hi && hi == min_value) return Clamp01(1.0 / static_cast<double>(stats.row_count));

  double matching = 0.0;
  for (const auto& bucket : stats.histogram) {
    const double width = bucket.hi - bucket.lo;
    if (width <= 0.0) continue;
    const double overlap_lo = has_lo ? std::max(bucket.lo, lo) : bucket.lo;
    const double overlap_hi = has_hi ? std::min(bucket.hi, hi) : bucket.hi;
    if (overlap_hi <= overlap_lo) continue;
    matching += static_cast<double>(bucket.count) * (overlap_hi - overlap_lo) / width;
  }
  return Clamp01(matching / static_cast<double>(stats.row_count));
}

double EstimateSelectivity(const Expr* predicate, const TableSchema& schema) {
  return Clamp01(EstimateRec(predicate, schema));
}

uint64_t EstimateRows(double selectivity, uint64_t row_count) {
  if (row_count == 0) return 0;
  if (selectivity <= 0.0) return 0;
  const double raw = selectivity * static_cast<double>(row_count);
  if (raw <= 0.0) return 0;
  uint64_t rows = static_cast<uint64_t>(std::llround(raw));
  if (rows == 0) rows = 1;
  if (rows > row_count) rows = row_count;
  return rows;
}

}  // namespace sthir
