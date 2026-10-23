#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "exec/value.h"

namespace sthir {

class Executor {
 public:
  virtual ~Executor() = default;

  virtual void init() = 0;
  virtual std::optional<Tuple> next() = 0;
  virtual void close() = 0;

  virtual const TableSchema& output_schema() const = 0;
};

}  // namespace sthir
