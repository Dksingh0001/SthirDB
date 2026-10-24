#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "catalog/catalog.h"

namespace sthir {

enum class ExprType {
  LITERAL,
  COLUMN_REF,
  UNARY_OP,
  BINARY_OP,
  IS_NULL_EXPR,
  AGGREGATE
};

class Expr {
 public:
  virtual ~Expr() = default;
  virtual ExprType type() const = 0;
  virtual std::unique_ptr<Expr> Clone() const = 0;
};

class LiteralExpr : public Expr {
 public:
  enum class LitType { INT, DECIMAL, STRING, NULL_VAL };

  static std::unique_ptr<LiteralExpr> MakeInt(int64_t val) {
    auto e = std::make_unique<LiteralExpr>();
    e->lit_type_ = LitType::INT;
    e->int_val_ = val;
    return e;
  }

  static std::unique_ptr<LiteralExpr> MakeDecimal(double val) {
    auto e = std::make_unique<LiteralExpr>();
    e->lit_type_ = LitType::DECIMAL;
    e->decimal_val_ = val;
    return e;
  }

  static std::unique_ptr<LiteralExpr> MakeString(std::string val) {
    auto e = std::make_unique<LiteralExpr>();
    e->lit_type_ = LitType::STRING;
    e->str_val_ = std::move(val);
    return e;
  }

  static std::unique_ptr<LiteralExpr> MakeNull() {
    auto e = std::make_unique<LiteralExpr>();
    e->lit_type_ = LitType::NULL_VAL;
    return e;
  }

  ExprType type() const override { return ExprType::LITERAL; }
  std::unique_ptr<Expr> Clone() const override {
    switch (lit_type_) {
      case LitType::INT: return MakeInt(int_val_);
      case LitType::DECIMAL: return MakeDecimal(decimal_val_);
      case LitType::STRING: return MakeString(str_val_);
      case LitType::NULL_VAL: return MakeNull();
    }
    return MakeNull();
  }
  LitType lit_type() const { return lit_type_; }
  int64_t int_val() const { return int_val_; }
  double decimal_val() const { return decimal_val_; }
  const std::string& str_val() const { return str_val_; }

 private:
  LitType lit_type_ = LitType::NULL_VAL;
  int64_t int_val_ = 0;
  double decimal_val_ = 0.0;
  std::string str_val_;
};

class ColumnRefExpr : public Expr {
 public:
  ColumnRefExpr(std::string table_name, std::string column_name)
      : table_name_(std::move(table_name)), column_name_(std::move(column_name)) {}

  ExprType type() const override { return ExprType::COLUMN_REF; }
  std::unique_ptr<Expr> Clone() const override {
    return std::make_unique<ColumnRefExpr>(table_name_, column_name_);
  }
  const std::string& table_name() const { return table_name_; }
  const std::string& column_name() const { return column_name_; }

 private:
  std::string table_name_;
  std::string column_name_;
};

enum class UnaryOp { NOT, NEGATE };

class UnaryExpr : public Expr {
 public:
  UnaryExpr(UnaryOp op, std::unique_ptr<Expr> expr)
      : op_(op), expr_(std::move(expr)) {}

  ExprType type() const override { return ExprType::UNARY_OP; }
  std::unique_ptr<Expr> Clone() const override {
    return std::make_unique<UnaryExpr>(op_, expr_ ? expr_->Clone() : nullptr);
  }
  UnaryOp op() const { return op_; }
  const Expr* expr() const { return expr_.get(); }
  std::unique_ptr<Expr>& expr() { return expr_; }

 private:
  UnaryOp op_;
  std::unique_ptr<Expr> expr_;
};

enum class BinaryOp {
  EQ, NEQ, LT, LTE, GT, GTE,
  AND, OR,
  ADD, SUB, MUL, DIV
};

class BinaryExpr : public Expr {
 public:
  BinaryExpr(BinaryOp op, std::unique_ptr<Expr> left, std::unique_ptr<Expr> right)
      : op_(op), left_(std::move(left)), right_(std::move(right)) {}

  ExprType type() const override { return ExprType::BINARY_OP; }
  std::unique_ptr<Expr> Clone() const override {
    return std::make_unique<BinaryExpr>(op_, left_ ? left_->Clone() : nullptr, right_ ? right_->Clone() : nullptr);
  }
  BinaryOp op() const { return op_; }
  const Expr* left() const { return left_.get(); }
  const Expr* right() const { return right_.get(); }
  std::unique_ptr<Expr>& left() { return left_; }
  std::unique_ptr<Expr>& right() { return right_; }

 private:
  BinaryOp op_;
  std::unique_ptr<Expr> left_;
  std::unique_ptr<Expr> right_;
};

class IsNullExpr : public Expr {
 public:
  IsNullExpr(std::unique_ptr<Expr> expr, bool is_not)
      : expr_(std::move(expr)), is_not_(is_not) {}

  ExprType type() const override { return ExprType::IS_NULL_EXPR; }
  std::unique_ptr<Expr> Clone() const override {
    return std::make_unique<IsNullExpr>(expr_ ? expr_->Clone() : nullptr, is_not_);
  }
  const Expr* expr() const { return expr_.get(); }
  bool is_not() const { return is_not_; }

 private:
  std::unique_ptr<Expr> expr_;
  bool is_not_ = false;
};

enum class AggFunc { COUNT, SUM, MIN, MAX, AVG };

class AggregateExpr : public Expr {
 public:
  AggregateExpr(AggFunc func, bool is_star, std::unique_ptr<Expr> arg)
      : func_(func), is_star_(is_star), arg_(std::move(arg)) {}

  ExprType type() const override { return ExprType::AGGREGATE; }
  std::unique_ptr<Expr> Clone() const override {
    return std::make_unique<AggregateExpr>(func_, is_star_, arg_ ? arg_->Clone() : nullptr);
  }
  AggFunc func() const { return func_; }
  bool is_star() const { return is_star_; }
  const Expr* arg() const { return arg_.get(); }

 private:
  AggFunc func_;
  bool is_star_ = false;
  std::unique_ptr<Expr> arg_;
};

enum class StmtType {
  CREATE_TABLE,
  CREATE_INDEX,
  INSERT,
  SELECT,
  UPDATE,
  DELETE,
  TRANSACTION,
  ANALYZE,
  SET,
  EXPLAIN
};

class SQLStatement {
 public:
  virtual ~SQLStatement() = default;
  virtual StmtType type() const = 0;
};

class CreateTableStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::CREATE_TABLE; }

  std::string table_name;
  std::vector<Column> columns;
  std::vector<std::string> primary_key_columns;
  std::vector<std::vector<std::string>> unique_constraints;
  std::vector<ForeignKey> foreign_keys;
  std::vector<CheckConstraint> check_constraints;
};

// CREATE INDEX <index_name> ON <table> (<column>)
class CreateIndexStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::CREATE_INDEX; }

  std::string index_name;
  std::string table_name;
  std::string column_name;
};

class InsertStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::INSERT; }

  std::string table_name;
  std::vector<std::string> column_names;
  std::vector<std::vector<std::unique_ptr<Expr>>> values_matrix;
};

struct SelectItem {
  std::unique_ptr<Expr> expr;
  std::string alias;
  bool is_star = false;
};

struct OrderByItem {
  std::string column_name;
  bool is_desc = false;
};

class SelectStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::SELECT; }

  std::vector<SelectItem> select_list;
  bool is_select_all = false;
  std::string from_table;

  bool has_join = false;
  std::string join_table;
  std::unique_ptr<Expr> join_predicate;

  std::unique_ptr<Expr> where_clause;
  std::vector<std::string> group_by_columns;
  std::vector<OrderByItem> order_by;
};

class UpdateStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::UPDATE; }

  std::string table_name;
  std::vector<std::pair<std::string, std::unique_ptr<Expr>>> assignments;
  std::unique_ptr<Expr> where_clause;
};

class DeleteStmt : public SQLStatement {
 public:
  StmtType type() const override { return StmtType::DELETE; }

  std::string table_name;
  std::unique_ptr<Expr> where_clause;
};

class TransactionStmt : public SQLStatement {
 public:
  enum class Action { BEGIN, COMMIT, ROLLBACK, CHECKPOINT };

  explicit TransactionStmt(Action act) : action_(act) {}
  StmtType type() const override { return StmtType::TRANSACTION; }
  Action action() const { return action_; }

 private:
  Action action_;
};

class AnalyzeStmt : public SQLStatement {
 public:
  explicit AnalyzeStmt(std::string table) : table_name_(std::move(table)) {}
  StmtType type() const override { return StmtType::ANALYZE; }
  const std::string& table_name() const { return table_name_; }

 private:
  std::string table_name_;
};

class SetStmt : public SQLStatement {
 public:
  SetStmt(std::string flag, std::string value)
      : flag_(std::move(flag)), value_(std::move(value)) {}
  StmtType type() const override { return StmtType::SET; }
  const std::string& flag() const { return flag_; }
  const std::string& value() const { return value_; }

 private:
  std::string flag_;
  std::string value_;
};

class ExplainStmt : public SQLStatement {
 public:
  explicit ExplainStmt(std::unique_ptr<SQLStatement> stmt)
      : stmt_(std::move(stmt)) {}
  StmtType type() const override { return StmtType::EXPLAIN; }
  const SQLStatement* stmt() const { return stmt_.get(); }
  std::unique_ptr<SQLStatement>& stmt() { return stmt_; }

 private:
  std::unique_ptr<SQLStatement> stmt_;
};

}  // namespace sthir
