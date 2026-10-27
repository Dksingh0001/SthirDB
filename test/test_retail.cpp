// Phase 7A tests: the retail schema DDL, its recorded constraints, and the
// FK/NOT NULL metadata hand-off to the Phase 4B constraint catalog (rules
// R3/R4 read this API).
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

const char* kTestDir = "./data/test_retail";
const char* kTestCatalogPath = "./data/test_retail/catalog.json";

void CleanDir() {
  std::error_code ec;
  fs::remove_all(kTestDir, ec);
  fs::create_directories(kTestDir, ec);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(static_cast<bool>(in));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void test_load_retail_schema() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  const std::string script = ReadFile("scripts/retail_schema.sql");
  const std::vector<std::string> statements = sthir::SplitSqlScript(script);
  CHECK_EQ(statements.size(), 8u);
  for (const auto& stmt : statements) {
    auto res = engine.Execute(stmt);
    if (!res.success) {
      std::fprintf(stderr, "schema statement failed: %s\n  %s\n",
                   res.error_message.c_str(), stmt.c_str());
      CHECK(res.success);
    }
  }

  CHECK_EQ(catalog.GetTableNames().size(), 8u);
  for (const char* t : {"Staff", "Shoppers", "Vendors", "Items", "Purchases",
                        "Invoices", "Purchase_Line_Items", "Item_Vendor_Catalogs"}) {
    CHECK(catalog.HasTable(t));
  }

  // UNIQUE(Shoppers.Email), UNIQUE(Invoices.PurchaseID), CHECK(StockLevel >= 0)
  const sthir::TableSchema* shoppers = catalog.GetTable("Shoppers");
  CHECK(shoppers != nullptr);
  CHECK_EQ(shoppers->unique_constraints.size(), 1u);
  CHECK_EQ(shoppers->unique_constraints[0][0], "Email");
  const sthir::TableSchema* invoices = catalog.GetTable("Invoices");
  CHECK(invoices != nullptr);
  CHECK_EQ(invoices->unique_constraints.size(), 1u);
  CHECK_EQ(invoices->unique_constraints[0][0], "PurchaseID");
  const sthir::TableSchema* items = catalog.GetTable("Items");
  CHECK(items != nullptr);
  CHECK_EQ(items->check_constraints.size(), 1u);
  CHECK(items->check_constraints[0].expression.find("StockLevel >= 0") != std::string::npos);
  const sthir::TableSchema* pli = catalog.GetTable("Purchase_Line_Items");
  CHECK(pli != nullptr);
  CHECK_EQ(pli->check_constraints.size(), 1u);
  CHECK(pli->check_constraints[0].expression.find("Quantity >= 1") != std::string::npos);

  test_passed("load_retail_schema");
}

void test_constraint_catalog_handoff() {
  CleanDir();
  sthir::Catalog catalog(kTestCatalogPath);
  catalog.Load();
  sthir::ExecutionEngine engine(&catalog, kTestDir);

  const std::string script = ReadFile("scripts/retail_schema.sql");
  for (const auto& stmt : sthir::SplitSqlScript(script)) {
    CHECK(engine.Execute(stmt).success);
  }

  // The Phase 4B hand-off: every FK edge with NOT NULL + parent-key facts.
  const auto edges = catalog.GetForeignKeyInfo();
  // Purchases x2 (Shoppers, Staff), Invoices x1, Purchase_Line_Items x2,
  // Item_Vendor_Catalogs x2.
  CHECK_EQ(edges.size(), 7u);

  auto find_edge = [&](const std::string& child, const std::string& parent) {
    for (const auto& e : edges) {
      if (e.child_table == child && e.parent_table == parent) return &e;
    }
    return static_cast<const sthir::ForeignKeyInfo*>(nullptr);
  };

  // R3 input: shopper -> purchases is NOT NULL and RESTRICT.
  const auto* purchase_shopper = find_edge("Purchases", "Shoppers");
  CHECK(purchase_shopper != nullptr);
  CHECK(purchase_shopper->child_not_null);
  CHECK(purchase_shopper->parent_unique);
  CHECK_EQ(static_cast<int>(purchase_shopper->on_delete),
           static_cast<int>(sthir::ForeignKey::OnDelete::RESTRICT));

  // R4 input: invoices join purchases on a unique key with CASCADE.
  const auto* invoice_purchase = find_edge("Invoices", "Purchases");
  CHECK(invoice_purchase != nullptr);
  CHECK(invoice_purchase->child_not_null);
  CHECK(invoice_purchase->parent_unique);
  CHECK_EQ(static_cast<int>(invoice_purchase->on_delete),
           static_cast<int>(sthir::ForeignKey::OnDelete::CASCADE));

  // Line items: CASCADE on the parent purchase, RESTRICT on the item.
  const auto* pli_purchase = find_edge("Purchase_Line_Items", "Purchases");
  CHECK(pli_purchase != nullptr);
  CHECK(pli_purchase->child_not_null);
  CHECK(pli_purchase->parent_unique);
  const auto* pli_item = find_edge("Purchase_Line_Items", "Items");
  CHECK(pli_item != nullptr);
  CHECK(pli_item->child_not_null);
  CHECK(pli_item->parent_unique);

  // Per-table view used by R3's "no parent column referenced" walk.
  CHECK_EQ(catalog.GetForeignKeysOn("Purchases").size(), 2u);
  CHECK_EQ(catalog.GetForeignKeysOn("Items").size(), 0u);  // Items is a parent only
  CHECK_EQ(catalog.GetForeignKeysOn("shoppers").size(), 0u);

  // A nullable FK column must be reported as child_not_null = false (R3 must
  // not fire on it).
  CHECK(engine.Execute("CREATE TABLE ParentT (pid INT PRIMARY KEY);").success);
  CHECK(engine.Execute("CREATE TABLE ChildT (cid INT PRIMARY KEY, pid INT REFERENCES ParentT (pid));").success);
  const auto edges_after = catalog.GetForeignKeyInfo();
  const sthir::ForeignKeyInfo* nullable = nullptr;
  for (const auto& e : edges_after) {
    if (e.child_table == "ChildT" && e.parent_table == "ParentT") nullable = &e;
  }
  CHECK(nullable != nullptr);
  CHECK(!nullable->child_not_null);

  test_passed("constraint_catalog_handoff");
}

}  // namespace

int main() {
  test_load_retail_schema();
  test_constraint_catalog_handoff();
  return 0;
}
