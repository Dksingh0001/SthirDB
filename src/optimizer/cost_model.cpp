#include "optimizer/cost_model.h"

#include <algorithm>
#include <cmath>

#include "common/config.h"

namespace sthir {

double SeqScanCost(double table_pages) {
  if (table_pages <= 0.0) return 0.0;
  return table_pages * kSeqPageCost;
}

double PagesOfMatchingRows(double matching_rows, double table_rows, double table_pages) {
  if (matching_rows <= 0.0 || table_pages <= 0.0) return 0.0;
  if (table_rows <= 0.0) return std::min(matching_rows, table_pages);  // unknown table size
  const double pages = std::ceil(matching_rows * table_pages / table_rows);
  return std::min(pages, table_pages);
}

double IndexScanCost(double index_height, double matching_rows, double table_rows,
                     double table_pages) {
  const double tree_pages = index_height > 0.0 ? index_height : 0.0;
  return tree_pages * kSeqPageCost +
         PagesOfMatchingRows(matching_rows, table_rows, table_pages) * kRandPageCost;
}

double FilterCost(double child_cost, double input_rows) {
  (void)input_rows;  // CPU cost is outside the page-based model
  return child_cost;
}

double SortCost(double rows, double row_bytes) {
  if (rows <= 0.0 || row_bytes <= 0.0) return 0.0;
  const double data_bytes = rows * row_bytes;
  const double memory_bytes = static_cast<double>(kDefaultPoolFrames) * static_cast<double>(kPageSize);
  if (data_bytes <= memory_bytes) return 0.0;  // fits in the pool: in-memory sort
  const double pages = data_bytes / static_cast<double>(kPageSize);
  return 2.0 * pages * kSeqPageCost;  // spill write pass + read pass
}

double AggregateCost(double child_cost, double input_rows) {
  (void)input_rows;  // hash aggregation is in memory for this project's sizes
  return child_cost;
}

double NestedLoopJoinCost(double outer_pages, double outer_rows, double inner_pages) {
  if (outer_pages <= 0.0 && outer_rows <= 0.0) return 0.0;
  return outer_pages * kSeqPageCost + outer_rows * std::max(0.0, inner_pages) * kSeqPageCost;
}

}  // namespace sthir
