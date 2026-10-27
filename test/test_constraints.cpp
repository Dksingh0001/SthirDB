// Phase 7 enforcement tests: every constraint class of the retail schema must
// block its anomaly (and say which constraint), while legitimate statements
// still pass — plus the `constraints_enabled = off` baseline, where the same
// statements must be accepted.
#include "catalog/catalog.h"
#include "exec/execution_engine.h"
#include "parser/parser.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const char* kTestDir = "./data/test_constraints";
const char* kTestCatalogPath = "./data/test_constraints/catalog.json";

void CleanDir() {
  std::error_code ec;
  fs::remove_all(kTestDir, ec);
  fs::create_directories(kTestDir, ec);
}

bool LoadSchema(sthir::ExecutionEngine& engine) {
  std::ifstream in("scripts/retail_schema.sql", std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  for (const auto& stmt : sthir::SplitSqlScript(ss.str())) {
    if (!engine.Execute(stmt).success) return false;
  }
  return true;
}

bool Seed(sthir::ExecutionEngine& engine) {
  const char* statements[] = {
      "INSERT INTO Staff VALUES (1, 'Alice', 'alice@sthirdb.test');",
      "INSERT INTO Shoppers VALUES (1, 'Bob', 'bob@sthirdb.test', 'Paris', 100),"
      " (2, 'Cara', 'cara@sthirdb.test', 'Rome', 101);",
      "INSERT INTO Vendors VALUES (1, 'Acme', 'DE');",
      "INSERT INTO Items VALUES (1, 'Widget', 'Gadgets', 10.00, 5),"
      " (2, 'Gizmo', 'Gadgets', 20.00, 0);",
      "INSERT INTO Purchases VALUES (100, 1, 1, 1000, 30.00);",
      "INSERT INTO Purchase_Line_Items VALUES (1, 100, 1, 2, 10.00, 20.00),"
      " (2, 100, 2, 1, 20.00, 20.00);",
      "INSERT INTO Invoices VALUES (900, 100, 40.00, 1001);",
      "INSERT INTO Item_Vendor_Catalogs VALUES (1, 1, 1, 6.00, 100);",
  };
  for (const char* stmt : statements) {
    auto res = engine.Execute(stmt);
    if (!res.success) {
      std::fprintf(stderr, "seed failed: %s\n  %s\n", res.error_message.c_str(), stmt);
      return false;
    }
  }
  return true;
}

void ExpectBlocked(sthir::ExecutionEngine& engine, const std::string& sql,
                   const std::string& marker) {
  auto res = engine.Execute(sql);
  if (res.success) {
    std::fprintf(stderr, "expected block, but statement succeeded: %s\n", sql.c_str());
  }
  CHECK(!res.success);
  if (res.error_message.find(marker) == std::string::npos) {
    std::fprintf(stderr, "expected marker '%s', got: %s\n", marker.c_str(),
                 res.error_message.c_str());
  }
  CHECK(res.error_message.find(marker) != std::string::npos);
}

std::string Scalar(sthir::ExecutionEngine& engine, const std::string& sql) {
  auto res = engine.Execute(sql);
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 1u);
  CHECK_EQ(res.rows[0].size(), 1u);
  return res.rows[0][0];
}

void test_constraint_enforcement() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);
  CHECK(LoadSchema(engine));
  CHECK(Seed(engine));

  // --- UNIQUE (Shoppers.Email) -----------------------------------------
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (3, 'Dan', 'bob@sthirdb.test', 'Oslo', 102)",
                "UNIQUE violation");
  // --- PRIMARY KEY (duplicate id) --------------------------------------
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (1, 'Eve', 'eve@sthirdb.test', 'Lisbon', 103)",
                "PRIMARY KEY violation");
  // --- NOT NULL (explicit column and omitted column) --------------------
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (NULL, 'Fay', 'fay@sthirdb.test', 'Lisbon', 104)",
                "NOT NULL violation");
  ExpectBlocked(engine, "INSERT INTO Shoppers VALUES (4, 'Gil', NULL, 'Lisbon', 105)",
                "NOT NULL violation");
  ExpectBlocked(engine,
                "INSERT INTO Items (ItemID, ItemName, CategoryName, RetailPrice)"
                " VALUES (4, 'NoStock', 'Gadgets', 5.00)",
                "NOT NULL violation");
  // --- CHECK (StockLevel >= 0) -----------------------------------------
  ExpectBlocked(engine, "UPDATE Items SET StockLevel = -5 WHERE ItemID = 1",
                "CHECK violation");
  ExpectBlocked(engine, "INSERT INTO Items VALUES (3, 'Bad', 'Gadgets', 5.00, -9)",
                "CHECK violation");
  // --- CHECK (Quantity >= 1): zero and negative -------------------------
  ExpectBlocked(engine, "UPDATE Purchase_Line_Items SET Quantity = 0 WHERE LineItemID = 1",
                "CHECK violation");
  ExpectBlocked(engine, "UPDATE Purchase_Line_Items SET Quantity = -3 WHERE LineItemID = 2",
                "CHECK violation");
  // --- FOREIGN KEY: absent parent / orphan line items -------------------
  ExpectBlocked(engine, "INSERT INTO Purchases VALUES (101, 999, 1, 1000, 10.00)",
                "FOREIGN KEY violation");
  ExpectBlocked(engine,
                "INSERT INTO Purchase_Line_Items VALUES (3, 999, 1, 1, 5.00, 5.00)",
                "FOREIGN KEY violation");  // orphan: no such purchase
  ExpectBlocked(engine,
                "INSERT INTO Purchase_Line_Items VALUES (3, 100, 77, 1, 5.00, 5.00)",
                "FOREIGN KEY violation");  // orphan: no such item
  // --- ON DELETE RESTRICT ----------------------------------------------
  ExpectBlocked(engine, "DELETE FROM Shoppers WHERE ShopperID = 1",
                "ON DELETE RESTRICT violation");

  // Nothing above changed any data.
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Shoppers"), "2");
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Purchase_Line_Items"), "2");
  CHECK_EQ(Scalar(engine, "SELECT StockLevel FROM Items WHERE ItemID = 1"), "5");
  CHECK_EQ(Scalar(engine, "SELECT Quantity FROM Purchase_Line_Items WHERE LineItemID = 1"),
           "2");

  // Legitimate statements still pass.
  CHECK(engine.Execute("INSERT INTO Items VALUES (5, 'GadgetX', 'Gadgets', 3.50, 2)").success);
  CHECK(engine.Execute("UPDATE Items SET StockLevel = 7 WHERE ItemID = 1").success);
  // Shopper 2 has no purchases: deleting them is allowed.
  CHECK(engine.Execute("DELETE FROM Shoppers WHERE ShopperID = 2").success);
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Shoppers"), "1");

  // --- ON DELETE CASCADE: purchase -> invoices + line items -------------
  auto cascade = engine.Execute("DELETE FROM Purchases WHERE PurchaseID = 100");
  if (!cascade.success) std::fprintf(stderr, "cascade delete failed: %s\n", cascade.error_message.c_str());
  CHECK(cascade.success);
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Purchases"), "0");
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Purchase_Line_Items"), "0");
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Invoices"), "0");

  // Shopper 1 is no longer referenced: the RESTRICT set must have shrunk
  // with the cascaded rows (reference counts), so this delete now succeeds.
  auto freed = engine.Execute("DELETE FROM Shoppers WHERE ShopperID = 1");
  if (!freed.success) std::fprintf(stderr, "restrict set stale: %s\n", freed.error_message.c_str());
  CHECK(freed.success);
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Shoppers"), "0");

  CleanDir();
  test_passed("constraint_enforcement");
}

void test_constraints_toggle() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);
  CHECK(LoadSchema(engine));
  CHECK(Seed(engine));

  // Unconstrained baseline: every anomaly below is accepted because the
  // caches and checks are switched off by the single schema flag.
  CHECK(engine.Execute("SET constraints_enabled = off").success);
  CHECK(engine.Execute(
      "INSERT INTO Shoppers VALUES (3, 'Dan', 'dan@sthirdb.test', 'Oslo', 102)").success);
  CHECK(engine.Execute("INSERT INTO Shoppers VALUES (1, 'Eve', 'eve@sthirdb.test', 'X', 103)").success);
  CHECK(engine.Execute("UPDATE Items SET StockLevel = -5 WHERE ItemID = 1").success);
  CHECK(engine.Execute("INSERT INTO Purchases VALUES (101, 999, 1, 1000, 10.00)").success);
  CHECK(engine.Execute(
      "INSERT INTO Shoppers VALUES (NULL, 'Fay', 'fay@sthirdb.test', 'Y', 104)").success);

  // Data really changed in the baseline: 2 seeded + 3 accepted inserts = 5.
  // The stock update took, and the purchase with a dangling FK was stored.
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Shoppers"), "5");
  CHECK_EQ(Scalar(engine, "SELECT StockLevel FROM Items WHERE ItemID = 1"), "-5");
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Purchases"), "2");
  // RESTRICT would reject this while constraints are on; both rows carrying
  // ShopperID 1 (Bob and the duplicate 'Eve') go away, leaving 3.
  CHECK(engine.Execute("DELETE FROM Shoppers WHERE ShopperID = 1").success);
  CHECK_EQ(Scalar(engine, "SELECT COUNT(*) FROM Shoppers"), "3");  // Cara, Dan, Fay

  // Re-enable: the caches rebuild from disk, so the same classes of
  // statement are blocked again from now on.
  CHECK(engine.Execute("SET constraints_enabled = on").success);
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (9, 'Hal', 'cara@sthirdb.test', 'Z', 105)",
                "UNIQUE violation");
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (3, 'Eve2', 'eve2@sthirdb.test', 'Z', 106)",
                "PRIMARY KEY violation");
  ExpectBlocked(engine,
                "INSERT INTO Shoppers VALUES (NULL, 'Gil', 'gil@sthirdb.test', 'Z', 107)",
                "NOT NULL violation");
  ExpectBlocked(engine, "UPDATE Items SET StockLevel = -1 WHERE ItemID = 2",
                "CHECK violation");
  // Shopper 1 is referenced by purchase 100, so re-creating them and trying
  // to delete them must hit RESTRICT again.
  CHECK(engine.Execute(
      "INSERT INTO Shoppers VALUES (1, 'Bob', 'bob@sthirdb.test', 'Paris', 100)").success);
  ExpectBlocked(engine, "DELETE FROM Shoppers WHERE ShopperID = 1",
                "ON DELETE RESTRICT violation");
  // The valid path still works after the toggle round-trip.
  CHECK(engine.Execute("INSERT INTO Items VALUES (6, 'GadgetY', 'Gadgets', 4.25, 1)").success);

  CleanDir();
  test_passed("constraints_toggle");
}

}  // namespace

int main() {
  test_constraint_enforcement();
  test_constraints_toggle();
  return 0;
}
