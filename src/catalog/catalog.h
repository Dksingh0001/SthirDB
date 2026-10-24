#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace sthir {

enum class DataType {
  INT,
  VARCHAR,
  DECIMAL
};

std::string DataTypeToString(DataType type);
std::optional<DataType> StringToDataType(const std::string& str);

struct Column {
  std::string name;
  DataType type = DataType::INT;
  uint32_t length = 4;  // for VARCHAR(n), n bytes; for INT, 4; for DECIMAL, 8
  bool not_null = false;
  bool primary_key = false;
  bool unique = false;
};

struct ForeignKey {
  std::string name;
  std::vector<std::string> column_names;
  std::string ref_table_name;
  std::vector<std::string> ref_column_names;
  enum class OnDelete { RESTRICT, CASCADE } on_delete = OnDelete::RESTRICT;
};

struct CheckConstraint {
  std::string name;
  std::string expression;
};

// Tree metadata for one B+tree index. root_page_id and height describe the
// on-disk tree; order is recorded so a future build can reject order mismatches.
struct IndexMetadata {
  std::string index_name;
  std::string column_name;
  uint32_t root_page_id = 0;
  uint32_t height = 0;
  uint32_t order = 4;
};

struct TableSchema {
  std::string table_name;
  std::string data_file_path;
  std::vector<Column> columns;
  std::vector<std::string> primary_key_columns;
  std::vector<std::vector<std::string>> unique_constraints;
  std::vector<ForeignKey> foreign_keys;
  std::vector<CheckConstraint> check_constraints;
  std::vector<IndexMetadata> indexes;

  int FindColumnIndex(const std::string& col_name) const;
  const Column* GetColumn(const std::string& col_name) const;
  bool HasColumn(const std::string& col_name) const;
};

class Catalog {
 public:
  explicit Catalog(std::string catalog_path = "./data/retail/catalog.json");

  // Loads table metadata from catalog.json if it exists.
  bool Load(std::string* err = nullptr);

  // Persists all table metadata to catalog.json.
  bool Save(std::string* err = nullptr) const;

  // Validates schema and registers a new table.
  bool CreateTable(const TableSchema& schema, std::string* err = nullptr);

  // Drops a table from the catalog.
  bool DropTable(const std::string& table_name, std::string* err = nullptr);

  // Looks up a table (case-insensitive).
  const TableSchema* GetTable(const std::string& table_name) const;
  bool HasTable(const std::string& table_name) const;

  std::vector<std::string> GetTableNames() const;

  // Registers an index for a table.
  bool AddIndex(const std::string& table_name, const IndexMetadata& idx, std::string* err = nullptr);

  const std::string& catalog_path() const { return catalog_path_; }

  static bool IsValidIdentifier(const std::string& id);

 private:
  static std::string ToLower(std::string s);

  std::string catalog_path_;
  std::unordered_map<std::string, TableSchema> tables_;  // keyed by lower-case table name
};

}  // namespace sthir
