#include "parser/lexer.h"
#include "parser/parser.h"

#include <string>

#include "test_macros.h"

namespace {

std::unique_ptr<sthir::SQLStatement> ParseSQL(const std::string& sql, std::string* err) {
  sthir::Lexer lexer(sql);
  auto tokens = lexer.Tokenize();
  if (lexer.has_error()) {
    if (err) *err = lexer.error_message();
    return nullptr;
  }
  sthir::Parser parser(std::move(tokens));
  auto stmt = parser.ParseStatement();
  if (parser.has_error() && err) {
    *err = parser.error_message();
  }
  return stmt;
}

void test_parse_create_table() {
  std::string err;
  auto stmt = ParseSQL("CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL);", &err);
  CHECK(stmt != nullptr);
  CHECK_EQ(static_cast<int>(stmt->type()), static_cast<int>(sthir::StmtType::CREATE_TABLE));

  auto* ct = dynamic_cast<sthir::CreateTableStmt*>(stmt.get());
  CHECK(ct != nullptr);
  CHECK_EQ(ct->table_name, "Items");
  CHECK_EQ(ct->columns.size(), 3u);
  CHECK_EQ(ct->columns[0].name, "ItemID");
  CHECK_EQ(static_cast<int>(ct->columns[0].type), static_cast<int>(sthir::DataType::INT));
  CHECK_EQ(ct->columns[1].name, "ItemName");
  CHECK_EQ(static_cast<int>(ct->columns[1].type), static_cast<int>(sthir::DataType::VARCHAR));
  CHECK_EQ(ct->columns[1].length, 100u);
  CHECK_EQ(ct->columns[2].name, "RetailPrice");
  CHECK_EQ(static_cast<int>(ct->columns[2].type), static_cast<int>(sthir::DataType::DECIMAL));

  test_passed("parse_create_table");
}

void test_parse_create_table_with_constraints() {
  std::string sql =
      "CREATE TABLE Orders ("
      "  OrderID INT PRIMARY KEY,"
      "  CustID INT NOT NULL,"
      "  OrderCode VARCHAR(50) UNIQUE,"
      "  Total DECIMAL,"
      "  FOREIGN KEY (CustID) REFERENCES Customers (CustID) ON DELETE CASCADE,"
      "  CHECK (Total >= 0)"
      ");";
  std::string err;
  auto stmt = ParseSQL(sql, &err);
  CHECK(stmt != nullptr);
  auto* ct = dynamic_cast<sthir::CreateTableStmt*>(stmt.get());
  CHECK(ct != nullptr);
  CHECK_EQ(ct->table_name, "Orders");
  CHECK_EQ(ct->columns.size(), 4u);
  CHECK(ct->columns[0].primary_key);
  CHECK(ct->columns[1].not_null);
  CHECK(ct->columns[2].unique);
  CHECK_EQ(ct->primary_key_columns.size(), 1u);
  CHECK_EQ(ct->primary_key_columns[0], "OrderID");
  CHECK_EQ(ct->foreign_keys.size(), 1u);
  CHECK_EQ(ct->foreign_keys[0].ref_table_name, "Customers");
  CHECK_EQ(static_cast<int>(ct->foreign_keys[0].on_delete),
           static_cast<int>(sthir::ForeignKey::OnDelete::CASCADE));
  CHECK_EQ(ct->check_constraints.size(), 1u);

  test_passed("parse_create_table_with_constraints");
}

void test_parse_insert() {
  std::string err;
  auto stmt = ParseSQL("INSERT INTO Items VALUES (1, 'Widget', 9.99);", &err);
  CHECK(stmt != nullptr);
  auto* ins = dynamic_cast<sthir::InsertStmt*>(stmt.get());
  CHECK(ins != nullptr);
  CHECK_EQ(ins->table_name, "Items");
  CHECK(ins->column_names.empty());
  CHECK_EQ(ins->values_matrix.size(), 1u);
  CHECK_EQ(ins->values_matrix[0].size(), 3u);

  auto stmt2 = ParseSQL("INSERT INTO Items (ItemID, ItemName) VALUES (2, 'Gadget');", &err);
  CHECK(stmt2 != nullptr);
  auto* ins2 = dynamic_cast<sthir::InsertStmt*>(stmt2.get());
  CHECK(ins2 != nullptr);
  CHECK_EQ(ins2->column_names.size(), 2u);
  CHECK_EQ(ins2->column_names[0], "ItemID");
  CHECK_EQ(ins2->column_names[1], "ItemName");

  test_passed("parse_insert");
}

void test_parse_select() {
  std::string err;
  auto stmt = ParseSQL("SELECT ItemName FROM Items WHERE RetailPrice > 5;", &err);
  CHECK(stmt != nullptr);
  auto* sel = dynamic_cast<sthir::SelectStmt*>(stmt.get());
  CHECK(sel != nullptr);
  CHECK(!sel->is_select_all);
  CHECK_EQ(sel->select_list.size(), 1u);
  CHECK_EQ(sel->from_table, "Items");
  CHECK(sel->where_clause != nullptr);

  auto stmt2 = ParseSQL("SELECT COUNT(*) FROM Items;", &err);
  CHECK(stmt2 != nullptr);
  auto* sel2 = dynamic_cast<sthir::SelectStmt*>(stmt2.get());
  CHECK(sel2 != nullptr);
  CHECK_EQ(sel2->select_list.size(), 1u);
  auto* agg = dynamic_cast<sthir::AggregateExpr*>(sel2->select_list[0].expr.get());
  CHECK(agg != nullptr);
  CHECK(agg->is_star());
  CHECK_EQ(static_cast<int>(agg->func()), static_cast<int>(sthir::AggFunc::COUNT));

  test_passed("parse_select");
}

void test_parse_select_aggregates_group_order_join() {
  std::string sql =
      "SELECT Dept, COUNT(Salary), AVG(Salary) "
      "FROM Employees JOIN Departments ON Employees.DeptID = Departments.ID "
      "WHERE Salary >= 50000 AND Bonus IS NOT NULL "
      "GROUP BY Dept "
      "ORDER BY Dept DESC;";
  std::string err;
  auto stmt = ParseSQL(sql, &err);
  CHECK(stmt != nullptr);
  auto* sel = dynamic_cast<sthir::SelectStmt*>(stmt.get());
  CHECK(sel != nullptr);
  CHECK_EQ(sel->select_list.size(), 3u);
  CHECK_EQ(sel->joins.size(), 1u);
  CHECK_EQ(sel->joins[0].table_name, "Departments");
  CHECK(sel->joins[0].on != nullptr);
  CHECK(sel->where_clause != nullptr);
  CHECK_EQ(sel->group_by_columns.size(), 1u);
  CHECK_EQ(sel->group_by_columns[0], "Dept");
  CHECK_EQ(sel->order_by.size(), 1u);
  CHECK_EQ(sel->order_by[0].column_name, "Dept");
  CHECK(sel->order_by[0].is_desc);

  test_passed("parse_select_aggregates_group_order_join");
}

void test_parse_update_delete() {
  std::string err;
  auto stmt1 = ParseSQL("UPDATE Items SET RetailPrice = 12.50 WHERE ItemID = 1;", &err);
  CHECK(stmt1 != nullptr);
  auto* upd = dynamic_cast<sthir::UpdateStmt*>(stmt1.get());
  CHECK(upd != nullptr);
  CHECK_EQ(upd->table_name, "Items");
  CHECK_EQ(upd->assignments.size(), 1u);
  CHECK_EQ(upd->assignments[0].first, "RetailPrice");
  CHECK(upd->where_clause != nullptr);

  auto stmt2 = ParseSQL("DELETE FROM Items WHERE ItemID = 1;", &err);
  CHECK(stmt2 != nullptr);
  auto* del = dynamic_cast<sthir::DeleteStmt*>(stmt2.get());
  CHECK(del != nullptr);
  CHECK_EQ(del->table_name, "Items");
  CHECK(del->where_clause != nullptr);

  test_passed("parse_update_delete");
}

void test_parse_transactions_and_utils() {
  std::string err;
  CHECK(ParseSQL("BEGIN;", &err) != nullptr);
  CHECK(ParseSQL("COMMIT TRANSACTION;", &err) != nullptr);
  CHECK(ParseSQL("ROLLBACK;", &err) != nullptr);
  CHECK(ParseSQL("ANALYZE Items;", &err) != nullptr);
  CHECK(ParseSQL("SET constraint_aware = on;", &err) != nullptr);

  auto exp = ParseSQL("EXPLAIN SELECT * FROM Items;", &err);
  CHECK(exp != nullptr);
  auto* exp_stmt = dynamic_cast<sthir::ExplainStmt*>(exp.get());
  CHECK(exp_stmt != nullptr);
  CHECK(exp_stmt->stmt() != nullptr);
  CHECK_EQ(static_cast<int>(exp_stmt->stmt()->type()), static_cast<int>(sthir::StmtType::SELECT));

  test_passed("parse_transactions_and_utils");
}

void test_parse_precedence_and_parentheses() {
  std::string err;
  // A AND B OR C -> (A AND B) OR C
  auto stmt1 = ParseSQL("SELECT * FROM t WHERE a = 1 AND b = 2 OR c = 3;", &err);
  CHECK(stmt1 != nullptr);
  auto* sel1 = dynamic_cast<sthir::SelectStmt*>(stmt1.get());
  auto* bin1 = dynamic_cast<sthir::BinaryExpr*>(sel1->where_clause.get());
  CHECK(bin1 != nullptr);
  CHECK_EQ(static_cast<int>(bin1->op()), static_cast<int>(sthir::BinaryOp::OR));

  // (A OR B) AND C
  auto stmt2 = ParseSQL("SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3;", &err);
  CHECK(stmt2 != nullptr);
  auto* sel2 = dynamic_cast<sthir::SelectStmt*>(stmt2.get());
  auto* bin2 = dynamic_cast<sthir::BinaryExpr*>(sel2->where_clause.get());
  CHECK(bin2 != nullptr);
  CHECK_EQ(static_cast<int>(bin2->op()), static_cast<int>(sthir::BinaryOp::AND));

  test_passed("parse_precedence_and_parentheses");
}

void test_parse_create_index() {
  std::string err;
  auto stmt = ParseSQL("CREATE INDEX idx_price ON Items (RetailPrice)", &err);
  CHECK(stmt != nullptr);
  CHECK_EQ(static_cast<int>(stmt->type()), static_cast<int>(sthir::StmtType::CREATE_INDEX));

  auto* ci = dynamic_cast<sthir::CreateIndexStmt*>(stmt.get());
  CHECK(ci != nullptr);
  CHECK_EQ(ci->index_name, "idx_price");
  CHECK_EQ(ci->table_name, "Items");
  CHECK_EQ(ci->column_name, "RetailPrice");

  // Missing pieces are reported, not silently accepted.
  err.clear();
  CHECK(ParseSQL("CREATE INDEX idx_price Items (RetailPrice)", &err) == nullptr);
  CHECK(!err.empty());

  err.clear();
  CHECK(ParseSQL("CREATE INDEX idx_price ON Items RetailPrice", &err) == nullptr);
  CHECK(!err.empty());

  test_passed("parse_create_index");
}

void test_parse_errors() {
  std::string err;
  CHECK(ParseSQL("CREATE TABLE ;", &err) == nullptr);
  CHECK(!err.empty());

  err.clear();
  CHECK(ParseSQL("SELECT FROM Items;", &err) == nullptr);
  CHECK(!err.empty());

  err.clear();
  CHECK(ParseSQL("INSERT INTO Items VALUES (1, 'Unterminated);", &err) == nullptr);
  CHECK(!err.empty());

  test_passed("parse_errors");
}

void test_parse_chained_joins() {
  std::string err;
  std::string sql =
      "SELECT Items.CategoryName, COUNT(*) "
      "FROM Purchases "
      "JOIN Purchase_Line_Items ON Purchase_Line_Items.PurchaseID = Purchases.PurchaseID "
      "JOIN Items ON Items.ItemID = Purchase_Line_Items.ItemID "
      "JOIN Invoices ON Invoices.PurchaseID = Purchases.PurchaseID "
      "GROUP BY Items.CategoryName;";
  auto stmt = ParseSQL(sql, &err);
  CHECK(stmt != nullptr);
  auto* sel = dynamic_cast<sthir::SelectStmt*>(stmt.get());
  CHECK(sel != nullptr);
  CHECK_EQ(sel->from_table, "Purchases");
  CHECK_EQ(sel->joins.size(), 3u);
  CHECK_EQ(sel->joins[0].table_name, "Purchase_Line_Items");
  CHECK_EQ(sel->joins[1].table_name, "Items");
  CHECK_EQ(sel->joins[2].table_name, "Invoices");
  for (const auto& clause : sel->joins) CHECK(clause.on != nullptr);
  CHECK_EQ(sel->group_by_columns.size(), 1u);

  // The legacy `inner join` spelling still parses, as do zero joins.
  auto stmt2 = ParseSQL("SELECT * FROM A inner JOIN B ON A.id = B.id;", &err);
  CHECK(stmt2 != nullptr);
  auto* sel2 = dynamic_cast<sthir::SelectStmt*>(stmt2.get());
  CHECK_EQ(sel2->joins.size(), 1u);
  CHECK_EQ(sel2->joins[0].table_name, "B");

  auto stmt3 = ParseSQL("SELECT * FROM A;", &err);
  auto* sel3 = dynamic_cast<sthir::SelectStmt*>(stmt3.get());
  CHECK_EQ(sel3->joins.size(), 0u);

  test_passed("parse_chained_joins");
}

}  // namespace

int main() {
  test_parse_create_table();
  test_parse_create_table_with_constraints();
  test_parse_create_index();
  test_parse_insert();
  test_parse_select();
  test_parse_select_aggregates_group_order_join();
  test_parse_chained_joins();
  test_parse_update_delete();
  test_parse_transactions_and_utils();
  test_parse_precedence_and_parentheses();
  test_parse_errors();
  return 0;
}
