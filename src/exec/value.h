#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>

#include "catalog/catalog.h"
#include "parser/ast.h"

namespace sthir {

enum class Tribool {
  False = 0,
  True = 1,
  Unknown = 2
};

inline Tribool TriboolAnd(Tribool a, Tribool b) {
  if (a == Tribool::False || b == Tribool::False) return Tribool::False;
  if (a == Tribool::True && b == Tribool::True) return Tribool::True;
  return Tribool::Unknown;
}

inline Tribool TriboolOr(Tribool a, Tribool b) {
  if (a == Tribool::True || b == Tribool::True) return Tribool::True;
  if (a == Tribool::False && b == Tribool::False) return Tribool::False;
  return Tribool::Unknown;
}

inline Tribool TriboolNot(Tribool a) {
  if (a == Tribool::True) return Tribool::False;
  if (a == Tribool::False) return Tribool::True;
  return Tribool::Unknown;
}

struct RID {
  uint32_t page_id = 0;
  uint16_t slot_id = 0;
  bool is_valid = false;

  bool operator==(const RID& other) const {
    return is_valid && other.is_valid && page_id == other.page_id && slot_id == other.slot_id;
  }
};

class Value {
 public:
  Value() : type_(DataType::INT), is_null_(true) {}

  static Value MakeNull(DataType type = DataType::INT) {
    Value v;
    v.type_ = type;
    v.is_null_ = true;
    return v;
  }

  static Value MakeInt(int32_t val) {
    Value v;
    v.type_ = DataType::INT;
    v.is_null_ = false;
    v.int_val_ = val;
    return v;
  }

  static Value MakeDecimal(double val) {
    Value v;
    v.type_ = DataType::DECIMAL;
    v.is_null_ = false;
    v.decimal_val_ = val;
    return v;
  }

  static Value MakeVarchar(std::string val) {
    Value v;
    v.type_ = DataType::VARCHAR;
    v.is_null_ = false;
    v.str_val_ = std::move(val);
    return v;
  }

  DataType type() const { return type_; }
  bool is_null() const { return is_null_; }

  int32_t as_int() const { return int_val_; }
  double as_decimal() const {
    if (type_ == DataType::INT) return static_cast<double>(int_val_);
    return decimal_val_;
  }
  const std::string& as_string() const { return str_val_; }

  std::string ToString() const;

  // Three-valued logic comparison
  Tribool Compare(BinaryOp op, const Value& other) const;

  // Arithmetic operations
  Value Add(const Value& other) const;
  Value Subtract(const Value& other) const;
  Value Multiply(const Value& other) const;
  Value Divide(const Value& other) const;

  // Cast to target data type
  Value CastTo(DataType target_type) const;

  // Hash code for hash aggregation
  size_t Hash() const;

  bool operator==(const Value& other) const;

 private:
  DataType type_;
  bool is_null_ = false;
  int32_t int_val_ = 0;
  double decimal_val_ = 0.0;
  std::string str_val_;
};

class Tuple {
 public:
  Tuple() = default;
  explicit Tuple(std::vector<Value> values) : values_(std::move(values)) {}

  size_t size() const { return values_.size(); }
  const Value& GetValue(size_t idx) const { return values_[idx]; }
  void SetValue(size_t idx, Value val) { values_[idx] = std::move(val); }
  const std::vector<Value>& values() const { return values_; }

  const RID& rid() const { return rid_; }
  void set_rid(const RID& rid) { rid_ = rid; }

  bool is_deleted() const { return deleted_; }
  void set_deleted(bool d) { deleted_ = d; }

  // Serialization into byte buffer for slotted page
  std::vector<char> Serialize(const TableSchema& schema) const;

  // Deserialization from byte buffer
  static Tuple Deserialize(const char* data, uint16_t len, const TableSchema& schema);

 private:
  std::vector<Value> values_;
  RID rid_;
  bool deleted_ = false;
};

}  // namespace sthir
