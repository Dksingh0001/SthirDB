#include "catalog/catalog.h"

#include <filesystem>
#include <string>

#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

const char* kTestCatalogPath = "./data/test_catalog.json";

void CleanTestFiles() {
  std::error_code ec;
  fs::remove(kTestCatalogPath, ec);
}

void test_catalog_create_and_lookup() {
  CleanTestFiles();
  sthir::Catalog catalog(kTestCatalogPath);
  std::string err;
  CHECK(catalog.Load(&err));

  sthir::TableSchema schema;
  schema.table_name = "Items";
  schema.columns = {
      {"ItemID", sthir::DataType::INT, 4, true, true, false},
      {"ItemName", sthir::DataType::VARCHAR, 100, false, false, false},
      {"RetailPrice", sthir::DataType::DECIMAL, 8, false, false, false}};
  schema.primary_key_columns = {"ItemID"};

  CHECK(catalog.CreateTable(schema, &err));
  CHECK(catalog.HasTable("items"));
  CHECK(catalog.HasTable("ITEMS"));

  const auto* tbl = catalog.GetTable("Items");
  CHECK(tbl != nullptr);
  CHECK_EQ(tbl->columns.size(), 3u);
  CHECK_EQ(tbl->FindColumnIndex("itemname"), 1);
  CHECK(tbl->HasColumn("retailprice"));

  CleanTestFiles();
  test_passed("catalog_create_and_lookup");
}

void test_catalog_validation_errors() {
  CleanTestFiles();
  sthir::Catalog catalog(kTestCatalogPath);
  std::string err;

  // Invalid table name
  sthir::TableSchema s1;
  s1.table_name = "123Bad";
  s1.columns = {{"id", sthir::DataType::INT, 4, false, false, false}};
  CHECK(!catalog.CreateTable(s1, &err));

  // Empty columns
  sthir::TableSchema s2;
  s2.table_name = "EmptyTable";
  CHECK(!catalog.CreateTable(s2, &err));

  // Duplicate columns
  sthir::TableSchema s3;
  s3.table_name = "DupCols";
  s3.columns = {
      {"colA", sthir::DataType::INT, 4, false, false, false},
      {"COLA", sthir::DataType::VARCHAR, 50, false, false, false}};
  CHECK(!catalog.CreateTable(s3, &err));

  // Non-existent primary key column
  sthir::TableSchema s4;
  s4.table_name = "BadPK";
  s4.columns = {{"id", sthir::DataType::INT, 4, false, false, false}};
  s4.primary_key_columns = {"missing_col"};
  CHECK(!catalog.CreateTable(s4, &err));

  CleanTestFiles();
  test_passed("catalog_validation_errors");
}

void test_catalog_persistence_across_restart() {
  CleanTestFiles();
  {
    sthir::Catalog catalog(kTestCatalogPath);
    std::string err;
    sthir::TableSchema schema;
    schema.table_name = "Customers";
    schema.columns = {
        {"CustID", sthir::DataType::INT, 4, true, true, false},
        {"Email", sthir::DataType::VARCHAR, 255, true, false, true}};
    schema.primary_key_columns = {"CustID"};
    schema.unique_constraints.push_back({"Email"});
    sthir::CheckConstraint chk;
    chk.name = "chk_cust_id";
    chk.expression = "CustID > 0";
    schema.check_constraints.push_back(chk);

    CHECK(catalog.CreateTable(schema, &err));
  }

  // Simulate program restart: new Catalog instance loading from file
  {
    sthir::Catalog catalog(kTestCatalogPath);
    std::string err;
    CHECK(catalog.Load(&err));
    CHECK(catalog.HasTable("customers"));

    const auto* tbl = catalog.GetTable("Customers");
    CHECK(tbl != nullptr);
    CHECK_EQ(tbl->columns.size(), 2u);
    CHECK(tbl->columns[0].not_null);
    CHECK_EQ(tbl->primary_key_columns.size(), 1u);
    CHECK_EQ(tbl->primary_key_columns[0], "CustID");
    CHECK_EQ(tbl->unique_constraints.size(), 1u);
    CHECK_EQ(tbl->unique_constraints[0][0], "Email");
    CHECK_EQ(tbl->check_constraints.size(), 1u);
    CHECK_EQ(tbl->check_constraints[0].expression, "CustID > 0");
  }

  CleanTestFiles();
  test_passed("catalog_persistence_across_restart");
}

void test_catalog_index_metadata_roundtrip() {
  CleanTestFiles();
  {
    sthir::Catalog catalog(kTestCatalogPath);
    std::string err;
    sthir::TableSchema schema;
    schema.table_name = "Items";
    schema.columns = {
        {"ItemID", sthir::DataType::INT, 4, true, true, false},
        {"RetailPrice", sthir::DataType::DECIMAL, 8, false, false, false}};
    CHECK(catalog.CreateTable(schema, &err));

    sthir::IndexMetadata idx;
    idx.index_name = "idx_price";
    idx.column_name = "RetailPrice";
    idx.root_page_id = 42;
    idx.height = 3;
    idx.order = 4;
    CHECK(catalog.AddIndex("Items", idx, &err));

    // Unknown column is rejected.
    sthir::IndexMetadata bad;
    bad.index_name = "idx_missing";
    bad.column_name = "NoSuchColumn";
    CHECK(!catalog.AddIndex("Items", bad, &err));
  }

  // Tree metadata must survive a restart.
  {
    sthir::Catalog catalog(kTestCatalogPath);
    std::string err;
    CHECK(catalog.Load(&err));
    const auto* tbl = catalog.GetTable("items");
    CHECK(tbl != nullptr);
    CHECK_EQ(tbl->indexes.size(), 1u);
    CHECK_EQ(tbl->indexes[0].index_name, "idx_price");
    CHECK_EQ(tbl->indexes[0].column_name, "RetailPrice");
    CHECK_EQ(tbl->indexes[0].root_page_id, 42u);
    CHECK_EQ(tbl->indexes[0].height, 3u);
    CHECK_EQ(tbl->indexes[0].order, 4u);
  }

  CleanTestFiles();
  test_passed("catalog_index_metadata_roundtrip");
}

}  // namespace

int main() {
  test_catalog_create_and_lookup();
  test_catalog_validation_errors();
  test_catalog_persistence_across_restart();
  test_catalog_index_metadata_roundtrip();
  return 0;
}
