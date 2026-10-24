#include "index/index_scan.h"

#include <cctype>
#include <cmath>
#include <filesystem>

#include "common/config.h"

namespace sthir {

namespace {

constexpr double kDecimalScale = 100.0;  // DECIMAL columns are indexed as cents

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Largest key whose column value is <= v, floored to the key grid.
int64_t KeyFloor(double v, int64_t scale) {
  return static_cast<int64_t>(std::floor(v * static_cast<double>(scale)));
}

// Smallest key whose column value is >= v, ceiled to the key grid.
int64_t KeyCeil(double v, int64_t scale) {
  return static_cast<int64_t>(std::ceil(v * static_cast<double>(scale)));
}

bool ColumnMatches(const Expr* expr, const TableSchema& schema, size_t key_column,
                   const std::string& col_name) {
  if (!expr || expr->type() != ExprType::COLUMN_REF) return false;
  const auto* ref = static_cast<const ColumnRefExpr*>(expr);
  if (!ref->table_name().empty() && ToLower(ref->table_name()) != ToLower(schema.table_name)) {
    return false;
  }
  return schema.FindColumnIndex(ref->column_name()) == static_cast<int>(key_column) &&
         ToLower(ref->column_name()) == ToLower(col_name);
}

// Reads a numeric literal as double. Returns false for strings/nulls.
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

void MergeLo(KeyRange* range, int64_t lo) {
  if (!range->has_lo || lo > range->lo) range->lo = lo;
  range->has_lo = true;
}

void MergeHi(KeyRange* range, int64_t hi) {
  if (!range->has_hi || hi < range->hi) range->hi = hi;
  range->has_hi = true;
}

// Applies `column OP value` to `range`. `flipped` means the column was on the
// right-hand side, so the operator is mirrored.
bool ApplyComparison(BinaryOp op, double value, int64_t scale, bool flipped, KeyRange* range) {
  if (flipped) {
    switch (op) {
      case BinaryOp::LT: op = BinaryOp::GT; break;
      case BinaryOp::LTE: op = BinaryOp::GTE; break;
      case BinaryOp::GT: op = BinaryOp::LT; break;
      case BinaryOp::GTE: op = BinaryOp::LTE; break;
      default: break;
    }
  }
  const int64_t floor_key = KeyFloor(value, scale);
  const int64_t ceil_key = KeyCeil(value, scale);

  switch (op) {
    case BinaryOp::EQ:
      // A value off the key grid matches nothing; the widened range is empty.
      MergeLo(range, ceil_key - 1);
      MergeHi(range, floor_key + 1);
      return true;
    case BinaryOp::GT:
      MergeLo(range, floor_key);  // one key unit wide of the true bound
      return true;
    case BinaryOp::GTE:
      MergeLo(range, ceil_key - 1);
      return true;
    case BinaryOp::LT:
      MergeHi(range, ceil_key);
      return true;
    case BinaryOp::LTE:
      MergeHi(range, floor_key + 1);
      return true;
    default:
      return false;
  }
}

bool ExtractRec(const Expr* expr, const TableSchema& schema, size_t key_column,
                const std::string& col_name, int64_t scale, KeyRange* out) {
  if (!expr || expr->type() != ExprType::BINARY_OP) return false;
  const auto* bin = static_cast<const BinaryExpr*>(expr);

  if (bin->op() == BinaryOp::AND) {
    KeyRange left;
    KeyRange right;
    const bool has_left = ExtractRec(bin->left(), schema, key_column, col_name, scale, &left);
    const bool has_right = ExtractRec(bin->right(), schema, key_column, col_name, scale, &right);
    if (has_left && has_right) {
      *out = left;
      if (right.has_lo) MergeLo(out, right.lo);
      if (right.has_hi) MergeHi(out, right.hi);
    } else if (has_left) {
      *out = left;
    } else if (has_right) {
      *out = right;
    }
    return has_left || has_right;
  }

  const bool col_on_left = ColumnMatches(bin->left(), schema, key_column, col_name);
  const bool col_on_right = ColumnMatches(bin->right(), schema, key_column, col_name);
  double value = 0.0;
  if (col_on_left && LiteralAsDouble(bin->right(), &value)) {
    return ApplyComparison(bin->op(), value, scale, false, out);
  }
  if (col_on_right && LiteralAsDouble(bin->left(), &value)) {
    return ApplyComparison(bin->op(), value, scale, true, out);
  }
  return false;
}

}  // namespace

int64_t IndexKeyFromInt(int32_t value) { return static_cast<int64_t>(value); }

int64_t IndexKeyFromDecimal(double value) {
  return static_cast<int64_t>(std::llround(value * kDecimalScale));
}

int64_t IndexKeyScale(DataType type) {
  return type == DataType::DECIMAL ? static_cast<int64_t>(kDecimalScale) : 1;
}

bool IsIndexableColumn(DataType type) {
  return type == DataType::INT || type == DataType::DECIMAL;
}

std::string IndexFilePath(const TableSchema& schema, const std::string& index_name) {
  std::filesystem::path table_path(schema.data_file_path);
  std::string base = table_path.stem().string();
  if (base.empty()) base = schema.table_name;

  std::string safe_name;
  for (char c : index_name) {
    safe_name.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_');
  }
  const std::string file_name = base + "." + safe_name + ".idx";

  if (table_path.has_parent_path()) {
    return (table_path.parent_path() / file_name).string();
  }
  return file_name;
}

bool ExtractKeyRange(const Expr* predicate, const TableSchema& schema, size_t key_column,
                     KeyRange* out) {
  if (!out || key_column >= schema.columns.size()) return false;
  const Column& column = schema.columns[key_column];
  if (!IsIndexableColumn(column.type)) return false;

  KeyRange range;
  if (!ExtractRec(predicate, schema, key_column, column.name, IndexKeyScale(column.type), &range)) {
    return false;
  }
  *out = range;
  return true;
}

}  // namespace sthir
