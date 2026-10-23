#include "planner/planner.h"

#include <algorithm>

namespace sthir {

namespace {

std::unique_ptr<Expr> CloneExpr(const Expr* expr) {
  if (!expr) return nullptr;
  switch (expr->type()) {
    case ExprType::LITERAL: {
      const auto* lit = static_cast<const LiteralExpr*>(expr);
      switch (lit->lit_type()) {
        case LiteralExpr::LitType::INT: return LiteralExpr::MakeInt(lit->int_val());
        case LiteralExpr::LitType::DECIMAL: return LiteralExpr::MakeDecimal(lit->decimal_val());
        case LiteralExpr::LitType::STRING: return LiteralExpr::MakeString(lit->str_val());
        case LiteralExpr::LitType::NULL_VAL: return LiteralExpr::MakeNull();
      }
      break;
    }
    case ExprType::COLUMN_REF: {
      const auto* cr = static_cast<const ColumnRefExpr*>(expr);
      return std::make_unique<ColumnRefExpr>(cr->table_name(), cr->column_name());
    }
    case ExprType::UNARY_OP: {
      const auto* u = static_cast<const UnaryExpr*>(expr);
      return std::make_unique<UnaryExpr>(u->op(), CloneExpr(u->expr()));
    }
    case ExprType::BINARY_OP: {
      const auto* b = static_cast<const BinaryExpr*>(expr);
      return std::make_unique<BinaryExpr>(b->op(), CloneExpr(b->left()), CloneExpr(b->right()));
    }
    case ExprType::IS_NULL_EXPR: {
      const auto* in = static_cast<const IsNullExpr*>(expr);
      return std::make_unique<IsNullExpr>(CloneExpr(in->expr()), in->is_not());
    }
    case ExprType::AGGREGATE: {
      const auto* agg = static_cast<const AggregateExpr*>(expr);
      return std::make_unique<AggregateExpr>(agg->func(), agg->is_star(), CloneExpr(agg->arg()));
    }
  }
  return nullptr;
}

bool ContainsAggregate(const Expr* expr) {
  if (!expr) return false;
  if (expr->type() == ExprType::AGGREGATE) return true;
  if (expr->type() == ExprType::UNARY_OP) {
    return ContainsAggregate(static_cast<const UnaryExpr*>(expr)->expr());
  }
  if (expr->type() == ExprType::BINARY_OP) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    return ContainsAggregate(b->left()) || ContainsAggregate(b->right());
  }
  if (expr->type() == ExprType::IS_NULL_EXPR) {
    return ContainsAggregate(static_cast<const IsNullExpr*>(expr)->expr());
  }
  return false;
}

std::string GetExprName(const Expr* expr, const std::string& alias) {
  if (!alias.empty()) return alias;
  if (!expr) return "?";
  if (expr->type() == ExprType::COLUMN_REF) {
    return static_cast<const ColumnRefExpr*>(expr)->column_name();
  }
  if (expr->type() == ExprType::AGGREGATE) {
    const auto* agg = static_cast<const AggregateExpr*>(expr);
    std::string fn = "COUNT";
    if (agg->func() == AggFunc::SUM) fn = "SUM";
    else if (agg->func() == AggFunc::MIN) fn = "MIN";
    else if (agg->func() == AggFunc::MAX) fn = "MAX";
    else if (agg->func() == AggFunc::AVG) fn = "AVG";
    if (agg->is_star()) return fn + "(*)";
    return fn + "(" + GetExprName(agg->arg(), "") + ")";
  }
  return "expr";
}

DataType DeduceExprType(const Expr* expr, const TableSchema& schema) {
  if (!expr) return DataType::INT;
  if (expr->type() == ExprType::LITERAL) {
    const auto* lit = static_cast<const LiteralExpr*>(expr);
    if (lit->lit_type() == LiteralExpr::LitType::DECIMAL) return DataType::DECIMAL;
    if (lit->lit_type() == LiteralExpr::LitType::STRING) return DataType::VARCHAR;
    return DataType::INT;
  }
  if (expr->type() == ExprType::COLUMN_REF) {
    const auto* cr = static_cast<const ColumnRefExpr*>(expr);
    const Column* col = schema.GetColumn(cr->column_name());
    if (col) return col->type;
    return DataType::VARCHAR;
  }
  if (expr->type() == ExprType::AGGREGATE) {
    const auto* agg = static_cast<const AggregateExpr*>(expr);
    if (agg->func() == AggFunc::COUNT) return DataType::INT;
    if (agg->func() == AggFunc::AVG) return DataType::DECIMAL;
    if (agg->arg()) return DeduceExprType(agg->arg(), schema);
    return DataType::DECIMAL;
  }
  if (expr->type() == ExprType::BINARY_OP) {
    const auto* b = static_cast<const BinaryExpr*>(expr);
    if (b->op() == BinaryOp::AND || b->op() == BinaryOp::OR ||
        b->op() == BinaryOp::EQ || b->op() == BinaryOp::NEQ ||
        b->op() == BinaryOp::LT || b->op() == BinaryOp::LTE ||
        b->op() == BinaryOp::GT || b->op() == BinaryOp::GTE) {
      return DataType::INT;
    }
    DataType lt = DeduceExprType(b->left(), schema);
    DataType rt = DeduceExprType(b->right(), schema);
    if (lt == DataType::DECIMAL || rt == DataType::DECIMAL) return DataType::DECIMAL;
    return DataType::INT;
  }
  return DataType::INT;
}

}  // namespace

std::unique_ptr<PlanNode> Planner::CreatePlan(const SQLStatement& stmt, std::string* err) {
  switch (stmt.type()) {
    case StmtType::SELECT:
      return PlanSelect(static_cast<const SelectStmt&>(stmt), err);
    case StmtType::INSERT:
      return PlanInsert(static_cast<const InsertStmt&>(stmt), err);
    case StmtType::UPDATE:
      return PlanUpdate(static_cast<const UpdateStmt&>(stmt), err);
    case StmtType::DELETE:
      return PlanDelete(static_cast<const DeleteStmt&>(stmt), err);
    default:
      if (err) *err = "statement type does not produce an execution plan";
      return nullptr;
  }
}

std::unique_ptr<PlanNode> Planner::PlanSelect(const SelectStmt& stmt, std::string* err) {
  const TableSchema* from_schema = catalog_.GetTable(stmt.from_table);
  if (!from_schema) {
    if (err) *err = "table '" + stmt.from_table + "' does not exist";
    return nullptr;
  }

  std::unique_ptr<PlanNode> current_plan =
      std::make_unique<SeqScanPlan>(from_schema->table_name, *from_schema);
  TableSchema current_schema = *from_schema;

  // Handle JOIN
  if (stmt.has_join) {
    const TableSchema* join_schema = catalog_.GetTable(stmt.join_table);
    if (!join_schema) {
      if (err) *err = "joined table '" + stmt.join_table + "' does not exist";
      return nullptr;
    }
    auto right_plan = std::make_unique<SeqScanPlan>(join_schema->table_name, *join_schema);

    // Merge schemas
    TableSchema merged_schema;
    merged_schema.table_name = current_schema.table_name + "_" + join_schema->table_name;
    for (const auto& c : current_schema.columns) {
      Column col = c;
      merged_schema.columns.push_back(col);
    }
    for (const auto& c : join_schema->columns) {
      Column col = c;
      // If duplicate column name across tables, prefix with table name
      if (merged_schema.HasColumn(col.name)) {
        col.name = join_schema->table_name + "." + col.name;
      }
      merged_schema.columns.push_back(col);
    }

    current_plan = std::make_unique<NestedLoopJoinPlan>(
        CloneExpr(stmt.join_predicate.get()),
        std::move(current_plan),
        std::move(right_plan),
        merged_schema);
    current_schema = merged_schema;
  }

  // Handle WHERE
  if (stmt.where_clause) {
    current_plan = std::make_unique<FilterPlan>(
        CloneExpr(stmt.where_clause.get()),
        std::move(current_plan));
  }

  // Check if aggregation is needed (either explicit GROUP BY or aggregate functions in select list)
  bool has_aggregates = !stmt.group_by_columns.empty();
  if (!has_aggregates && !stmt.is_select_all) {
    for (const auto& item : stmt.select_list) {
      if (ContainsAggregate(item.expr.get())) {
        has_aggregates = true;
        break;
      }
    }
  }

  if (has_aggregates) {
    std::vector<AggEntry> agg_entries;
    TableSchema agg_schema;
    agg_schema.table_name = current_schema.table_name;

    // First group-by columns in schema
    for (const auto& gb_col : stmt.group_by_columns) {
      const Column* c = current_schema.GetColumn(gb_col);
      if (!c) {
        if (err) *err = "group by column '" + gb_col + "' not found";
        return nullptr;
      }
      agg_schema.columns.push_back(*c);
    }

    // Next aggregate functions
    for (const auto& item : stmt.select_list) {
      if (item.expr && item.expr->type() == ExprType::AGGREGATE) {
        const auto* agg = static_cast<const AggregateExpr*>(item.expr.get());
        std::string out_name = GetExprName(item.expr.get(), item.alias);
        AggEntry entry;
        entry.func = agg->func();
        entry.is_star = agg->is_star();
        entry.arg = CloneExpr(agg->arg());
        entry.output_name = out_name;
        agg_entries.push_back(std::move(entry));

        Column out_col;
        out_col.name = out_name;
        out_col.type = DeduceExprType(item.expr.get(), current_schema);
        agg_schema.columns.push_back(out_col);
      }
    }

    current_plan = std::make_unique<AggregatePlan>(
        std::move(agg_entries),
        stmt.group_by_columns,
        agg_schema,
        std::move(current_plan));
    current_schema = agg_schema;
  }

  // Handle ORDER BY
  if (!stmt.order_by.empty()) {
    current_plan = std::make_unique<SortPlan>(stmt.order_by, std::move(current_plan));
  }

  // Handle Projection (if not SELECT *)
  if (!stmt.is_select_all) {
    std::vector<std::pair<std::unique_ptr<Expr>, std::string>> projs;
    TableSchema proj_schema;
    proj_schema.table_name = current_schema.table_name;

    for (const auto& item : stmt.select_list) {
      std::string out_name = GetExprName(item.expr.get(), item.alias);
      Column out_col;
      out_col.name = out_name;
      out_col.type = DeduceExprType(item.expr.get(), current_schema);
      proj_schema.columns.push_back(out_col);
      projs.push_back({CloneExpr(item.expr.get()), out_name});
    }

    current_plan = std::make_unique<ProjectPlan>(
        std::move(projs),
        proj_schema,
        std::move(current_plan));
  }

  return current_plan;
}

std::unique_ptr<PlanNode> Planner::PlanInsert(const InsertStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  std::vector<std::string> target_cols;
  if (stmt.column_names.empty()) {
    for (const auto& c : schema->columns) {
      target_cols.push_back(c.name);
    }
  } else {
    for (const auto& cn : stmt.column_names) {
      if (!schema->HasColumn(cn)) {
        if (err) *err = "column '" + cn + "' not found in table '" + stmt.table_name + "'";
        return nullptr;
      }
      target_cols.push_back(cn);
    }
  }

  // Validate values matrix dimensions
  for (const auto& row : stmt.values_matrix) {
    if (row.size() != target_cols.size()) {
      if (err) *err = "INSERT has " + std::to_string(target_cols.size()) +
                      " target columns but VALUES has " + std::to_string(row.size()) + " values";
      return nullptr;
    }
  }

  std::vector<std::vector<std::unique_ptr<Expr>>> cloned_matrix;
  for (const auto& row : stmt.values_matrix) {
    std::vector<std::unique_ptr<Expr>> cloned_row;
    for (const auto& expr : row) {
      cloned_row.push_back(CloneExpr(expr.get()));
    }
    cloned_matrix.push_back(std::move(cloned_row));
  }

  return std::make_unique<InsertPlan>(stmt.table_name, *schema, target_cols, std::move(cloned_matrix));
}

std::unique_ptr<PlanNode> Planner::PlanUpdate(const UpdateStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  for (const auto& assign : stmt.assignments) {
    if (!schema->HasColumn(assign.first)) {
      if (err) *err = "column '" + assign.first + "' not found in table '" + stmt.table_name + "'";
      return nullptr;
    }
  }

  std::unique_ptr<PlanNode> scan = std::make_unique<SeqScanPlan>(stmt.table_name, *schema);
  if (stmt.where_clause) {
    scan = std::make_unique<FilterPlan>(CloneExpr(stmt.where_clause.get()), std::move(scan));
  }

  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> cloned_assignments;
  for (const auto& a : stmt.assignments) {
    cloned_assignments.push_back({a.first, CloneExpr(a.second.get())});
  }

  return std::make_unique<UpdatePlan>(stmt.table_name, *schema, std::move(cloned_assignments), std::move(scan));
}

std::unique_ptr<PlanNode> Planner::PlanDelete(const DeleteStmt& stmt, std::string* err) {
  const TableSchema* schema = catalog_.GetTable(stmt.table_name);
  if (!schema) {
    if (err) *err = "table '" + stmt.table_name + "' does not exist";
    return nullptr;
  }

  std::unique_ptr<PlanNode> scan = std::make_unique<SeqScanPlan>(stmt.table_name, *schema);
  if (stmt.where_clause) {
    scan = std::make_unique<FilterPlan>(CloneExpr(stmt.where_clause.get()), std::move(scan));
  }

  return std::make_unique<DeletePlan>(stmt.table_name, *schema, std::move(scan));
}

}  // namespace sthir
