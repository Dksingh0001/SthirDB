#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace sthir {

// Builds statistics for one column from its non-NULL values: row_count, NDV,
// min, max, and the locked kHistogramBuckets (= 3) equi-width histogram whose
// bucket counts sum to row_count.
void BuildColumnStats(const std::vector<double>& values, uint64_t null_count, ColumnStats* out);

// Scans the table exactly like SeqScan (one page pinned at a time) and computes
// statistics for `column_name`.
bool ComputeColumnStats(const TableSchema& schema, const std::string& column_name,
                        DiskManager* dm, BufferPool* bp, ColumnStats* out, std::string* err);

// Computes statistics for every INT/DECIMAL column of the table. VARCHAR
// histograms are not part of Phase 4 (the index only keys INT/DECIMAL too).
bool ComputeTableStats(const TableSchema& schema, DiskManager* dm, BufferPool* bp,
                       std::vector<ColumnStats>* out, std::string* err);

}  // namespace sthir
