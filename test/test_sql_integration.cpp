#include "catalog/catalog.h"
#include "exec/execution_engine.h"

#include <filesystem>
#include <string>

#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const char* kTestDir = "./data/test_integration";
const char* kTestCatalogPath = "./data/test_integration/catalog.json";

void CleanDir() {
  std::error_code ec;
  fs::remove_all(kTestDir, ec);
  fs::create_directories(kTestDir, ec);
}

void test_acceptance_examples() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  // 1. CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL)
  auto res1 = engine.Execute("CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL);");
  CHECK(res1.success);
  CHECK(catalog.HasTable("items"));

  // 2. INSERT INTO Items VALUES (1, 'Widget', 9.99)
  auto res2 = engine.Execute("INSERT INTO Items VALUES (1, 'Widget', 9.99);");
  CHECK(res2.success);
  CHECK_EQ(res2.rows_affected, 1u);

  // 3. SELECT ItemName FROM Items WHERE RetailPrice > 5
  auto res3 = engine.Execute("SELECT ItemName FROM Items WHERE RetailPrice > 5;");
  CHECK(res3.success);
  CHECK(res3.is_query);
  CHECK_EQ(res3.rows.size(), 1u);
  CHECK_EQ(res3.rows[0][0], "Widget");

  // 4. SELECT COUNT(*) FROM Items
  auto res4 = engine.Execute("SELECT COUNT(*) FROM Items;");
  CHECK(res4.success);
  CHECK(res4.is_query);
  CHECK_EQ(res4.rows.size(), 1u);
  CHECK_EQ(res4.rows[0][0], "1");

  // 5. UPDATE Items SET RetailPrice = 12.50 WHERE ItemID = 1
  auto res5 = engine.Execute("UPDATE Items SET RetailPrice = 12.50 WHERE ItemID = 1;");
  CHECK(res5.success);
  CHECK_EQ(res5.rows_affected, 1u);

  // Verify update
  auto res5b = engine.Execute("SELECT RetailPrice FROM Items WHERE ItemID = 1;");
  CHECK(res5b.success);
  CHECK_EQ(res5b.rows.size(), 1u);
  CHECK_EQ(res5b.rows[0][0], "12.50");

  // 6. DELETE FROM Items WHERE ItemID = 1
  auto res6 = engine.Execute("DELETE FROM Items WHERE ItemID = 1;");
  CHECK(res6.success);
  CHECK_EQ(res6.rows_affected, 1u);

  // Verify delete
  auto res6b = engine.Execute("SELECT COUNT(*) FROM Items;");
  CHECK(res6b.success);
  CHECK_EQ(res6b.rows[0][0], "0");

  CleanDir();
  test_passed("acceptance_examples");
}

void test_joins_aggregates_and_nulls() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  CHECK(engine.Execute("CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL);").success);
  CHECK(engine.Execute("CREATE TABLE Orders (OrderID INT, ItemID INT, Quantity INT);").success);

  CHECK(engine.Execute("INSERT INTO Items VALUES (1, 'Widget', 10.00);").success);
  CHECK(engine.Execute("INSERT INTO Items VALUES (2, 'Gadget', 20.00);").success);
  CHECK(engine.Execute("INSERT INTO Items VALUES (3, 'MysteryItem', NULL);").success);

  CHECK(engine.Execute("INSERT INTO Orders VALUES (101, 1, 2);").success);
  CHECK(engine.Execute("INSERT INTO Orders VALUES (102, 2, 5);").success);
  CHECK(engine.Execute("INSERT INTO Orders VALUES (103, 1, 1);").success);

  auto res_join = engine.Execute(
      "SELECT Items.ItemName, Orders.Quantity FROM Items JOIN Orders ON Items.ItemID = Orders.ItemID ORDER BY Orders.Quantity DESC;");
  CHECK(res_join.success);
  CHECK_EQ(res_join.rows.size(), 3u);
  CHECK_EQ(res_join.rows[0][0], "Gadget");
  CHECK_EQ(res_join.rows[0][1], "5");

  // NULL checks
  auto res_null = engine.Execute("SELECT ItemName FROM Items WHERE RetailPrice IS NULL;");
  CHECK(res_null.success);
  CHECK_EQ(res_null.rows.size(), 1u);
  CHECK_EQ(res_null.rows[0][0], "MysteryItem");

  auto res_not_null = engine.Execute("SELECT ItemName FROM Items WHERE RetailPrice IS NOT NULL;");
  CHECK(res_not_null.success);
  CHECK_EQ(res_not_null.rows.size(), 2u);

  // COUNT(*) vs COUNT(col) with nulls
  auto res_count = engine.Execute("SELECT COUNT(*), COUNT(RetailPrice), SUM(RetailPrice), AVG(RetailPrice) FROM Items;");
  CHECK(res_count.success);
  CHECK_EQ(res_count.rows.size(), 1u);
  CHECK_EQ(res_count.rows[0][0], "3");      // COUNT(*)
  CHECK_EQ(res_count.rows[0][1], "2");      // COUNT(RetailPrice) skips NULL
  CHECK_EQ(res_count.rows[0][2], "30.00");  // SUM
  CHECK_EQ(res_count.rows[0][3], "15.00");  // AVG

  CleanDir();
  test_passed("joins_aggregates_and_nulls");
}

void test_persistence_across_restart() {
  CleanDir();
  {
    sthir::Catalog catalog(kTestCatalogPath);
    catalog.Load();
    sthir::ExecutionEngine engine(&catalog, kTestDir);
    CHECK(engine.Execute("CREATE TABLE Products (ProdID INT PRIMARY KEY, ProdName VARCHAR(50));").success);
    CHECK(engine.Execute("INSERT INTO Products VALUES (10, 'Hammer'), (20, 'Nail');").success);
  }

  // Restart engine with fresh Catalog and ExecutionEngine instances
  {
    sthir::Catalog catalog(kTestCatalogPath);
    CHECK(catalog.Load());
    CHECK(catalog.HasTable("products"));
    sthir::ExecutionEngine engine(&catalog, kTestDir);

    auto res = engine.Execute("SELECT ProdName FROM Products ORDER BY ProdID ASC;");
    CHECK(res.success);
    CHECK_EQ(res.rows.size(), 2u);
    CHECK_EQ(res.rows[0][0], "Hammer");
    CHECK_EQ(res.rows[1][0], "Nail");
  }

  CleanDir();
  test_passed("persistence_across_restart");
}

void test_transactions_and_utils() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  CHECK(engine.Execute("CREATE TABLE T (x INT);").success);
  CHECK(engine.Execute("BEGIN;").success);
  CHECK(engine.Execute("COMMIT;").success);
  CHECK(engine.Execute("ROLLBACK;").success);
  CHECK(engine.Execute("ANALYZE T;").success);
  CHECK(engine.Execute("SET constraint_aware = on;").success);
  CHECK_EQ(engine.GetSessionFlag("constraint_aware"), "on");

  auto res_exp = engine.Execute("EXPLAIN SELECT * FROM T;");
  CHECK(res_exp.success);
  CHECK(res_exp.is_query);
  CHECK(!res_exp.rows.empty());

  CleanDir();
  test_passed("transactions_and_utils");
}

void test_error_handling() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  // Unknown table
  auto r1 = engine.Execute("SELECT * FROM NonExistent;");
  CHECK(!r1.success);
  CHECK(!r1.error_message.empty());

  // Unknown column
  CHECK(engine.Execute("CREATE TABLE T (a INT);").success);
  auto r2 = engine.Execute("SELECT b FROM T;");
  CHECK(!r2.success);
  CHECK(!r2.error_message.empty());

  // Invalid SQL syntax
  auto r3 = engine.Execute("SELECTTT * FROM T;");
  CHECK(!r3.success);
  CHECK(!r3.error_message.empty());

  // Duplicate table
  auto r4 = engine.Execute("CREATE TABLE T (b INT);");
  CHECK(!r4.success);
  CHECK(!r4.error_message.empty());

  CleanDir();
  test_passed("error_handling");
}

void test_multi_table_join_chain() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  CHECK(engine.Execute("CREATE TABLE Purchases (PurchaseID INT, ShopperID INT);").success);
  CHECK(engine.Execute("CREATE TABLE Purchase_Line_Items (LineItemID INT, PurchaseID INT, ItemID INT, Quantity INT);").success);
  CHECK(engine.Execute("CREATE TABLE Items (ItemID INT, CategoryName VARCHAR(30), RetailPrice DECIMAL(10,2));").success);
  CHECK(engine.Execute("CREATE TABLE Invoices (InvoiceID INT, PurchaseID INT, Total DECIMAL(10,2));").success);

  CHECK(engine.Execute("INSERT INTO Items VALUES (1, 'Books', 10.00), (2, 'Games', 50.00), (3, 'Books', 20.00);").success);
  CHECK(engine.Execute("INSERT INTO Purchases VALUES (100, 1), (101, 2);").success);
  CHECK(engine.Execute("INSERT INTO Purchase_Line_Items VALUES (1, 100, 1, 2), (2, 100, 2, 1), (3, 101, 1, 1), (4, 101, 3, 3);").success);
  CHECK(engine.Execute("INSERT INTO Invoices VALUES (900, 100, 70.00), (901, 101, 70.00);").success);

  // Four-table reporting query: three chained JOIN clauses.
  auto res = engine.Execute(
      "SELECT Items.CategoryName, COUNT(*), SUM(Quantity) "
      "FROM Purchases "
      "JOIN Purchase_Line_Items ON Purchase_Line_Items.PurchaseID = Purchases.PurchaseID "
      "JOIN Items ON Items.ItemID = Purchase_Line_Items.ItemID "
      "JOIN Invoices ON Invoices.PurchaseID = Purchases.PurchaseID "
      "GROUP BY Items.CategoryName "
      "ORDER BY Items.CategoryName;");
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 2u);
  CHECK_EQ(res.rows[0][0], "Books");
  CHECK_EQ(res.rows[0][1], "3");  // line items 1, 3, 4
  CHECK_EQ(res.rows[0][2], "6.00");  // quantities 2 + 1 + 3
  CHECK_EQ(res.rows[1][0], "Games");
  CHECK_EQ(res.rows[1][1], "1");
  CHECK_EQ(res.rows[1][2], "1.00");

  // A join chain with a WHERE clause that references two tables.
  auto res2 = engine.Execute(
      "SELECT COUNT(*) FROM Purchases "
      "JOIN Purchase_Line_Items ON Purchase_Line_Items.PurchaseID = Purchases.PurchaseID "
      "JOIN Invoices ON Invoices.PurchaseID = Purchases.PurchaseID "
      "WHERE Purchases.PurchaseID = 100;");
  CHECK(res2.success);
  CHECK_EQ(res2.rows.size(), 1u);
  CHECK_EQ(res2.rows[0][0], "2");  // two line items of purchase 100

  CleanDir();
  test_passed("multi_table_join_chain");
}

}  // namespace

int main() {
  test_acceptance_examples();
  test_joins_aggregates_and_nulls();
  test_multi_table_join_chain();
  test_persistence_across_restart();
  test_transactions_and_utils();
  test_error_handling();
  return 0;
}
