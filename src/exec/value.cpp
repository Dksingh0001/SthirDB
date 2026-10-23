#include "exec/value.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <functional>

namespace sthir {

std::string Value::ToString() const {
  if (is_null_) return "NULL";
  switch (type_) {
    case DataType::INT:
      return std::to_string(int_val_);
    case DataType::DECIMAL: {
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << decimal_val_;
      std::string s = ss.str();
      // If ends with .00 and had no fractional part, or keep .00 for decimal representation
      return s;
    }
    case DataType::VARCHAR:
      return str_val_;
  }
  return "NULL";
}

Tribool Value::Compare(BinaryOp op, const Value& other) const {
  if (is_null_ || other.is_null_) {
    return Tribool::Unknown;
  }

  int cmp = 0;
  if (type_ == DataType::VARCHAR || other.type_ == DataType::VARCHAR) {
    std::string s1 = (type_ == DataType::VARCHAR) ? str_val_ : ToString();
    std::string s2 = (other.type_ == DataType::VARCHAR) ? other.str_val_ : other.ToString();
    if (s1 < s2) cmp = -1;
    else if (s1 > s2) cmp = 1;
    else cmp = 0;
  } else if (type_ == DataType::DECIMAL || other.type_ == DataType::DECIMAL) {
    double d1 = as_decimal();
    double d2 = other.as_decimal();
    if (std::abs(d1 - d2) < 1e-9) cmp = 0;
    else if (d1 < d2) cmp = -1;
    else cmp = 1;
  } else {
    // Both INT
    int32_t i1 = int_val_;
    int32_t i2 = other.int_val_;
    if (i1 < i2) cmp = -1;
    else if (i1 > i2) cmp = 1;
    else cmp = 0;
  }

  bool res = false;
  switch (op) {
    case BinaryOp::EQ: res = (cmp == 0); break;
    case BinaryOp::NEQ: res = (cmp != 0); break;
    case BinaryOp::LT: res = (cmp < 0); break;
    case BinaryOp::LTE: res = (cmp <= 0); break;
    case BinaryOp::GT: res = (cmp > 0); break;
    case BinaryOp::GTE: res = (cmp >= 0); break;
    default: return Tribool::Unknown;
  }
  return res ? Tribool::True : Tribool::False;
}

Value Value::Add(const Value& other) const {
  if (is_null_ || other.is_null_) return Value::MakeNull(type_);
  if (type_ == DataType::DECIMAL || other.type_ == DataType::DECIMAL) {
    return Value::MakeDecimal(as_decimal() + other.as_decimal());
  }
  return Value::MakeInt(int_val_ + other.int_val_);
}

Value Value::Subtract(const Value& other) const {
  if (is_null_ || other.is_null_) return Value::MakeNull(type_);
  if (type_ == DataType::DECIMAL || other.type_ == DataType::DECIMAL) {
    return Value::MakeDecimal(as_decimal() - other.as_decimal());
  }
  return Value::MakeInt(int_val_ - other.int_val_);
}

Value Value::Multiply(const Value& other) const {
  if (is_null_ || other.is_null_) return Value::MakeNull(type_);
  if (type_ == DataType::DECIMAL || other.type_ == DataType::DECIMAL) {
    return Value::MakeDecimal(as_decimal() * other.as_decimal());
  }
  return Value::MakeInt(int_val_ * other.int_val_);
}

Value Value::Divide(const Value& other) const {
  if (is_null_ || other.is_null_) return Value::MakeNull(type_);
  double denom = other.as_decimal();
  if (std::abs(denom) < 1e-9) return Value::MakeNull(type_);  // Division by zero -> NULL
  if (type_ == DataType::DECIMAL || other.type_ == DataType::DECIMAL) {
    return Value::MakeDecimal(as_decimal() / denom);
  }
  return Value::MakeInt(int_val_ / other.int_val_);
}

Value Value::CastTo(DataType target_type) const {
  if (is_null_) return Value::MakeNull(target_type);
  if (type_ == target_type) return *this;

  switch (target_type) {
    case DataType::INT: {
      if (type_ == DataType::DECIMAL) return Value::MakeInt(static_cast<int32_t>(decimal_val_));
      if (type_ == DataType::VARCHAR) {
        try {
          return Value::MakeInt(static_cast<int32_t>(std::stol(str_val_)));
        } catch (...) {
          return Value::MakeNull(DataType::INT);
        }
      }
      break;
    }
    case DataType::DECIMAL: {
      if (type_ == DataType::INT) return Value::MakeDecimal(static_cast<double>(int_val_));
      if (type_ == DataType::VARCHAR) {
        try {
          return Value::MakeDecimal(std::stod(str_val_));
        } catch (...) {
          return Value::MakeNull(DataType::DECIMAL);
        }
      }
      break;
    }
    case DataType::VARCHAR:
      return Value::MakeVarchar(ToString());
  }
  return Value::MakeNull(target_type);
}

size_t Value::Hash() const {
  if (is_null_) return 0x9e3779b9;
  switch (type_) {
    case DataType::INT:
      return std::hash<int32_t>{}(int_val_);
    case DataType::DECIMAL:
      return std::hash<double>{}(decimal_val_);
    case DataType::VARCHAR:
      return std::hash<std::string>{}(str_val_);
  }
  return 0;
}

bool Value::operator==(const Value& other) const {
  if (is_null_ && other.is_null_) return true;
  if (is_null_ || other.is_null_) return false;
  return Compare(BinaryOp::EQ, other) == Tribool::True;
}

// -------------------------------------------------------------
// Tuple serialization / deserialization
// -------------------------------------------------------------

std::vector<char> Tuple::Serialize(const TableSchema& schema) const {
  std::vector<char> buf;
  // 1 byte deleted flag
  buf.push_back(deleted_ ? 1 : 0);

  // Null bitmap: (cols + 7) / 8 bytes
  size_t num_cols = schema.columns.size();
  size_t bitmap_bytes = (num_cols + 7) / 8;
  std::vector<uint8_t> bitmap(bitmap_bytes, 0);

  for (size_t i = 0; i < num_cols; ++i) {
    bool is_null = (i >= values_.size() || values_[i].is_null());
    if (is_null) {
      bitmap[i / 8] |= (1 << (i % 8));
    }
  }

  for (uint8_t b : bitmap) {
    buf.push_back(static_cast<char>(b));
  }

  // Column values
  for (size_t i = 0; i < num_cols; ++i) {
    if (i < values_.size() && !values_[i].is_null()) {
      const Value& val = values_[i];
      const Column& col = schema.columns[i];
      switch (col.type) {
        case DataType::INT: {
          int32_t iv = val.as_int();
          char b[4];
          std::memcpy(b, &iv, 4);
          buf.insert(buf.end(), b, b + 4);
          break;
        }
        case DataType::DECIMAL: {
          double dv = val.as_decimal();
          char b[8];
          std::memcpy(b, &dv, 8);
          buf.insert(buf.end(), b, b + 8);
          break;
        }
        case DataType::VARCHAR: {
          std::string sv = val.as_string();
          if (col.length > 0 && sv.size() > col.length) {
            sv = sv.substr(0, col.length);
          }
          uint16_t len = static_cast<uint16_t>(sv.size());
          char lb[2];
          std::memcpy(lb, &len, 2);
          buf.insert(buf.end(), lb, lb + 2);
          buf.insert(buf.end(), sv.data(), sv.data() + len);
          break;
        }
      }
    }
  }

  return buf;
}

Tuple Tuple::Deserialize(const char* data, uint16_t len, const TableSchema& schema) {
  Tuple tuple;
  if (len == 0 || data == nullptr) return tuple;

  // Check if legacy Phase 1 63-byte row:
  // [int32 id][char[50] name][double price][bool deleted] = 63
  if (len == 63 && schema.columns.size() == 3 &&
      schema.columns[0].type == DataType::INT &&
      schema.columns[1].type == DataType::VARCHAR &&
      schema.columns[2].type == DataType::DECIMAL) {
    int32_t id = 0;
    std::memcpy(&id, data, 4);

    char name_buf[51] = {};
    std::memcpy(name_buf, data + 4, 50);
    name_buf[50] = '\0';
    size_t n = 0;
    while (n < 50 && name_buf[n] != '\0') ++n;
    std::string name(name_buf, n);

    double price = 0.0;
    std::memcpy(&price, data + 54, 8);

    bool del = (data[62] != 0);

    tuple.values_.push_back(Value::MakeInt(id));
    tuple.values_.push_back(Value::MakeVarchar(name));
    tuple.values_.push_back(Value::MakeDecimal(price));
    tuple.set_deleted(del);
    return tuple;
  }

  // Standard Phase 2 dynamic tuple format
  tuple.deleted_ = (data[0] != 0);
  size_t num_cols = schema.columns.size();
  size_t bitmap_bytes = (num_cols + 7) / 8;
  if (len < 1 + bitmap_bytes) return tuple;

  const uint8_t* bitmap = reinterpret_cast<const uint8_t*>(data + 1);
  size_t offset = 1 + bitmap_bytes;

  for (size_t i = 0; i < num_cols; ++i) {
    bool is_null = (bitmap[i / 8] & (1 << (i % 8))) != 0;
    const Column& col = schema.columns[i];
    if (is_null) {
      tuple.values_.push_back(Value::MakeNull(col.type));
    } else {
      switch (col.type) {
        case DataType::INT: {
          if (offset + 4 <= len) {
            int32_t iv = 0;
            std::memcpy(&iv, data + offset, 4);
            tuple.values_.push_back(Value::MakeInt(iv));
            offset += 4;
          } else {
            tuple.values_.push_back(Value::MakeNull(DataType::INT));
          }
          break;
        }
        case DataType::DECIMAL: {
          if (offset + 8 <= len) {
            double dv = 0.0;
            std::memcpy(&dv, data + offset, 8);
            tuple.values_.push_back(Value::MakeDecimal(dv));
            offset += 8;
          } else {
            tuple.values_.push_back(Value::MakeNull(DataType::DECIMAL));
          }
          break;
        }
        case DataType::VARCHAR: {
          if (offset + 2 <= len) {
            uint16_t vlen = 0;
            std::memcpy(&vlen, data + offset, 2);
            offset += 2;
            if (offset + vlen <= len) {
              std::string sv(data + offset, vlen);
              tuple.values_.push_back(Value::MakeVarchar(sv));
              offset += vlen;
            } else {
              tuple.values_.push_back(Value::MakeNull(DataType::VARCHAR));
            }
          } else {
            tuple.values_.push_back(Value::MakeNull(DataType::VARCHAR));
          }
          break;
        }
      }
    }
  }

  return tuple;
}

}  // namespace sthir
