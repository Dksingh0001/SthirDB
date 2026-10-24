#include "optimizer/statistics.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "exec/value.h"
#include "storage/page.h"

namespace sthir {

namespace {

bool IsNumericColumn(DataType type) {
  return type == DataType::INT || type == DataType::DECIMAL;
}

double ValueAsDouble(const Value& value) {
  return value.type() == DataType::INT ? static_cast<double>(value.as_int()) : value.as_decimal();
}

// Single scan that appends one value per live tuple into `columns[i]`.
bool ScanNumericColumns(const TableSchema& schema, const std::vector<size_t>& column_indexes,
                        DiskManager* dm, BufferPool* bp, std::vector<std::vector<double>>* columns,
                        std::vector<uint64_t>* null_counts, std::string* err) {
  columns->assign(column_indexes.size(), {});
  null_counts->assign(column_indexes.size(), 0);
  if (dm == nullptr || bp == nullptr) {
    if (err) *err = "statistics: missing storage";
    return false;
  }

  const uint32_t pages = dm->num_pages();
  for (uint32_t pid = 0; pid < pages; ++pid) {
    Page* page = bp->fetch_page(pid);
    if (page == nullptr) {
      if (err) *err = "statistics: buffer pool exhausted reading page " + std::to_string(pid);
      return false;
    }
    for (uint16_t slot = 0; slot < page->slot_count(); ++slot) {
      if (!page->tuple_alive(slot)) continue;
      const uint16_t len = page->tuple_length(slot);
      if (len == 0) continue;
      std::vector<char> buf(len);
      const int bytes = page->ReadTuple(slot, buf.data());
      if (bytes <= 0) continue;

      const Tuple tuple = Tuple::Deserialize(buf.data(), static_cast<uint16_t>(bytes), schema);
      if (tuple.is_deleted()) continue;
      for (size_t c = 0; c < column_indexes.size(); ++c) {
        const Value& value = tuple.GetValue(column_indexes[c]);
        if (value.is_null()) {
          ++(*null_counts)[c];
        } else {
          (*columns)[c].push_back(ValueAsDouble(value));
        }
      }
    }
    bp->unpin_page(pid, false);
  }
  return true;
}

}  // namespace

void BuildColumnStats(const std::vector<double>& values, uint64_t null_count, ColumnStats* out) {
  if (!out) return;
  out->row_count = values.size();
  out->null_count = null_count;
  out->histogram.clear();
  out->valid = !values.empty();
  if (values.empty()) {
    out->ndv = 0;
    out->min_value = 0.0;
    out->max_value = 0.0;
    return;
  }

  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  out->min_value = sorted.front();
  out->max_value = sorted.back();
  out->ndv = static_cast<uint64_t>(std::unique(sorted.begin(), sorted.end()) - sorted.begin());

  // Locked format: kHistogramBuckets equi-width buckets over [min, max]. A
  // constant column puts every row in the first bucket.
  const double width = (out->max_value - out->min_value) / static_cast<double>(kHistogramBuckets);
  out->histogram.resize(kHistogramBuckets);
  for (uint32_t b = 0; b < kHistogramBuckets; ++b) {
    out->histogram[b].lo = out->min_value + width * static_cast<double>(b);
    out->histogram[b].hi = b + 1 == kHistogramBuckets ? out->max_value
                                                     : out->min_value + width * static_cast<double>(b + 1);
    out->histogram[b].count = 0;
  }
  if (width <= 0.0) {
    out->histogram[0].count = out->row_count;
  } else {
    for (double v : values) {
      int64_t b = static_cast<int64_t>((v - out->min_value) / width);
      if (b < 0) b = 0;
      if (b >= static_cast<int64_t>(kHistogramBuckets)) b = kHistogramBuckets - 1;
      ++out->histogram[static_cast<size_t>(b)].count;
    }
  }
}

bool ComputeColumnStats(const TableSchema& schema, const std::string& column_name,
                        DiskManager* dm, BufferPool* bp, ColumnStats* out, std::string* err) {
  if (!out) {
    if (err) *err = "statistics: null output";
    return false;
  }
  const int col = schema.FindColumnIndex(column_name);
  if (col < 0) {
    if (err) *err = "column '" + column_name + "' not found in table '" + schema.table_name + "'";
    return false;
  }
  if (!IsNumericColumn(schema.columns[static_cast<size_t>(col)].type)) {
    if (err) {
      *err = "column '" + schema.columns[static_cast<size_t>(col)].name + "' has type " +
             DataTypeToString(schema.columns[static_cast<size_t>(col)].type) +
             "; Phase 4 collects statistics for INT and DECIMAL columns only";
    }
    return false;
  }

  std::vector<std::vector<double>> columns;
  std::vector<uint64_t> null_counts;
  if (!ScanNumericColumns(schema, {static_cast<size_t>(col)}, dm, bp, &columns, &null_counts, err)) {
    return false;
  }
  BuildColumnStats(columns[0], null_counts[0], out);
  out->column_name = schema.columns[static_cast<size_t>(col)].name;
  return true;
}

bool ComputeTableStats(const TableSchema& schema, DiskManager* dm, BufferPool* bp,
                       std::vector<ColumnStats>* out, std::string* err) {
  if (!out) {
    if (err) *err = "statistics: null output";
    return false;
  }
  out->clear();

  std::vector<size_t> indexes;
  for (size_t c = 0; c < schema.columns.size(); ++c) {
    if (IsNumericColumn(schema.columns[c].type)) indexes.push_back(c);
  }
  if (indexes.empty()) {
    if (err) *err = "table '" + schema.table_name + "' has no INT or DECIMAL column to analyze";
    return false;
  }

  std::vector<std::vector<double>> columns;
  std::vector<uint64_t> null_counts;
  if (!ScanNumericColumns(schema, indexes, dm, bp, &columns, &null_counts, err)) return false;

  for (size_t i = 0; i < indexes.size(); ++i) {
    ColumnStats stats;
    BuildColumnStats(columns[i], null_counts[i], &stats);
    stats.column_name = schema.columns[indexes[i]].name;
    out->push_back(std::move(stats));
  }
  return true;
}

}  // namespace sthir
