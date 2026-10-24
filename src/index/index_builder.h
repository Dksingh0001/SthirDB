#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"

namespace sthir {

struct IndexBuildResult {
  uint32_t root_page_id = 0;
  uint32_t height = 0;
  size_t entries = 0;
};

// Builds (or rebuilds) the index file for `index_name` on `column_name` by
// scanning every live tuple of the table, exactly like a SeqScan, and inserting
// (key, RID) into a fresh B+tree. NULL values are not indexed. The index file is
// created next to the table file (see IndexFilePath) and gets its own disk
// manager and buffer pool, so it never competes with table pages for frames.
bool BuildIndex(const TableSchema& schema, const std::string& index_name,
                const std::string& column_name, DiskManager* table_dm, BufferPool* table_bp,
                IndexBuildResult* out, std::string* err);

}  // namespace sthir
