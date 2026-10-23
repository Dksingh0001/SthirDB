#include "exec/expr_eval.h"

#include <cctype>
#include <algorithm>

namespace sthir {

namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int ResolveColumn(const ColumnRefExpr* cr, const TableSchema& schema) {
  std::string target_col = ToLower(cr->column_name());
  std::string target_tbl = ToLower(cr->table_name());

  // First try exact match on full name
  if (!target_tbl.empty()) {
    std::string full_name = target_tbl + "." + target_col;
    for (size_t i = 0; i < schema.columns.size(); ++i) {
      if (ToLower(schema.columns[i].name) == full_name) return static_cast<int>(i);
    }
  }

  // Next try matching column name
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    std::string cname = ToLower(schema.columns[i].name);
    if (cname == target_col) return static_cast<int>(i);

    // If schema column has table prefix "table.col"
    auto dot = cname.find('.');
    if (dot != std::string::npos) {
      std::string tbl_part = cname.substr(0, dot);
      std::string col_part = cname.substr(dot + 1);
      if (col_part == target_col && (target_tbl.empty() || tbl_part == target_tbl)) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

}  // namespace

Value EvaluateExpr(const Expr* expr, const Tuple& tuple, const TableSchema& schema) {
  if (!expr) return Value::MakeNull();

  switch (expr->type()) {
    case ExprType::LITERAL: {
      const auto* lit = static_cast<const LiteralExpr*>(expr);
      switch (lit->lit_type()) {
        case LiteralExpr::LitType::INT:
          return Value::MakeInt(static_cast<int32_t>(lit->int_val()));
        case LiteralExpr::LitType::DECIMAL:
          return Value::MakeDecimal(lit->decimal_val());
        case LiteralExpr::LitType::STRING:
          return Value::MakeVarchar(lit->str_val());
        case LiteralExpr::LitType::NULL_VAL:
          return Value::MakeNull();
      }
      break;
    }
    case ExprType::COLUMN_REF: {
      const auto* cr = static_cast<const ColumnRefExpr*>(expr);
      int idx = ResolveColumn(cr, schema);
      if (idx >= 0 && idx < static_cast<int>(tuple.size())) {
        return tuple.GetValue(static_cast<size_t>(idx));
      }
      return Value::MakeNull();
    }
    case ExprType::UNARY_OP: {
      const auto* u = static_cast<const UnaryExpr*>(expr);
      Value child_val = EvaluateExpr(u->expr(), tuple, schema);
      if (child_val.is_null()) return Value::MakeNull();
      if (u->op() == UnaryOp::NEGATE) {
        if (child_val.type() == DataType::DECIMAL) {
          return Value::MakeDecimal(-child_val.as_decimal());
        }
        return Value::MakeInt(-child_val.as_int());
      }
      if (u->op() == UnaryOp::NOT) {
        Tribool tb = EvaluatePredicate(u->expr(), tuple, schema);
        Tribool res = TriboolNot(tb);
        if (res == Tribool::True) return Value::MakeInt(1);
        if (res == Tribool::False) return Value::MakeInt(0);
        return Value::MakeNull();
      }
      break;
    }
    case ExprType::BINARY_OP: {
      const auto* b = static_cast<const BinaryExpr*>(expr);
      if (b->op() == BinaryOp::AND || b->op() == BinaryOp::OR ||
          b->op() == BinaryOp::EQ || b->op() == BinaryOp::NEQ ||
          b->op() == BinaryOp::LT || b->op() == BinaryOp::LTE ||
          b->op() == BinaryOp::GT || b->op() == BinaryOp::GTE) {
        Tribool tb = EvaluatePredicate(expr, tuple, schema);
        if (tb == Tribool::True) return Value::MakeInt(1);
        if (tb == Tribool::False) return Value::MakeInt(0);
        return Value::MakeNull();
      }
      Value l = EvaluateExpr(b->left(), tuple, schema);
      Value r = EvaluateExpr(b->right(), tuple, schema);
      if (b->op() == BinaryOp::ADD) return l.Add(r);
      if (b->op() == BinaryOp::SUB) return l.Subtract(r);
      if (b->op() == BinaryOp::MUL) return l.Multiply(r);
      if (b->op() == BinaryOp::DIV) return l.Divide(r);
      break;
    }
    case ExprType::IS_NULL_EXPR: {
      Tribool tb = EvaluatePredicate(expr, tuple, schema);
      if (tb == Tribool::True) return Value::MakeInt(1);
      return Value::MakeInt(0);
    }
    case ExprType::AGGREGATE: {
      const auto* agg = static_cast<const AggregateExpr*>(expr);
      std::string fn = "COUNT";
      if (agg->func() == AggFunc::SUM) fn = "SUM";
      else if (agg->func() == AggFunc::MIN) fn = "MIN";
      else if (agg->func() == AggFunc::MAX) fn = "MAX";
      else if (agg->func() == AggFunc::AVG) fn = "AVG";

      std::string arg_name;
      if (agg->arg()) {
        if (agg->arg()->type() == ExprType::COLUMN_REF) {
          arg_name = static_cast<const ColumnRefExpr*>(agg->arg())->column_name();
        }
      }
      std::string full_name = agg->is_star() ? (fn + "(*)") : (fn + "(" + arg_name + ")");
      std::string target = ToLower(full_name);

      for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (ToLower(schema.columns[i].name) == target) {
          if (i < tuple.size()) return tuple.GetValue(i);
        }
      }
      // Fallback: match by prefix fn + "("
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (ToLower(schema.columns[i].name).rfind(ToLower(fn + "("), 0) == 0) {
          if (i < tuple.size()) return tuple.GetValue(i);
        }
      }
      break;
    }
  }
  return Value::MakeNull();
}

Tribool EvaluatePredicate(const Expr* expr, const Tuple& tuple, const TableSchema& schema) {
  if (!expr) return Tribool::True;

  switch (expr->type()) {
    case ExprType::BINARY_OP: {
      const auto* b = static_cast<const BinaryExpr*>(expr);
      if (b->op() == BinaryOp::AND) {
        Tribool left = EvaluatePredicate(b->left(), tuple, schema);
        Tribool right = EvaluatePredicate(b->right(), tuple, schema);
        return TriboolAnd(left, right);
      }
      if (b->op() == BinaryOp::OR) {
        Tribool left = EvaluatePredicate(b->left(), tuple, schema);
        Tribool right = EvaluatePredicate(b->right(), tuple, schema);
        return TriboolOr(left, right);
      }
      // Comparison operator
      Value l = EvaluateExpr(b->left(), tuple, schema);
      Value r = EvaluateExpr(b->right(), tuple, schema);
      return l.Compare(b->op(), r);
    }
    case ExprType::UNARY_OP: {
      const auto* u = static_cast<const UnaryExpr*>(expr);
      if (u->op() == UnaryOp::NOT) {
        Tribool child = EvaluatePredicate(u->expr(), tuple, schema);
        return TriboolNot(child);
      }
      break;
    }
    case ExprType::IS_NULL_EXPR: {
      const auto* in = static_cast<const IsNullExpr*>(expr);
      Value v = EvaluateExpr(in->expr(), tuple, schema);
      if (in->is_not()) {
        return v.is_null() ? Tribool::False : Tribool::True;
      }
      return v.is_null() ? Tribool::True : Tribool::False;
    }
    case ExprType::LITERAL: {
      Value v = EvaluateExpr(expr, tuple, schema);
      if (v.is_null()) return Tribool::Unknown;
      return (v.as_int() != 0) ? Tribool::True : Tribool::False;
    }
    case ExprType::COLUMN_REF: {
      Value v = EvaluateExpr(expr, tuple, schema);
      if (v.is_null()) return Tribool::Unknown;
      return (v.as_int() != 0) ? Tribool::True : Tribool::False;
    }
    default:
      break;
  }
  return Tribool::Unknown;
}

}  // namespace sthir
