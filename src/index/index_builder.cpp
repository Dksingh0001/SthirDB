#include "index/index_builder.h"

#include <filesystem>
#include <vector>

#include "common/config.h"
#include "exec/value.h"
#include "index/bplus_tree.h"
#include "index/index_scan.h"
#include "storage/page.h"

namespace sthir {

bool BuildIndex(const TableSchema& schema, const std::string& index_name,
                const std::string& column_name, DiskManager* table_dm, BufferPool* table_bp,
                IndexBuildResult* out, std::string* err) {
  if (!table_dm || !table_bp || !out) {
    if (err) *err = "index build: missing storage";
    return false;
  }
  const int col_idx = schema.FindColumnIndex(column_name);
  if (col_idx < 0) {
    if (err) *err = "column '" + column_name + "' not found in table '" + schema.table_name + "'";
    return false;
  }
  const Column& column = schema.columns[col_idx];
  if (!IsIndexableColumn(column.type)) {
    if (err) {
      *err = "column '" + column.name + "' has type " + DataTypeToString(column.type) +
             "; Phase 3 indexes INT and DECIMAL columns only";
    }
    return false;
  }

  const std::string path = IndexFilePath(schema, index_name);
  std::error_code ec;
  std::filesystem::remove(path, ec);  // rebuild from scratch

  DiskManager idx_dm(path);
  BufferPool idx_bp(kDefaultPoolFrames, &idx_dm);
  BPlusTree tree(&idx_dm, &idx_bp, kInvalidPageId, 0);
  if (!tree.Create()) {
    if (err) *err = "index build: cannot allocate the root page";
    return false;
  }

  size_t entries = 0;
  const uint32_t pages = table_dm->num_pages();
  for (uint32_t pid = 0; pid < pages; ++pid) {
    Page* page = table_bp->fetch_page(pid);
    if (page == nullptr) {
      if (err) *err = "index build: buffer pool exhausted reading page " + std::to_string(pid);
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

      const Value& value = tuple.GetValue(static_cast<size_t>(col_idx));
      if (value.is_null()) continue;  // NULLs are not indexed

      const int64_t key = value.type() == DataType::INT ? IndexKeyFromInt(value.as_int())
                                                        : IndexKeyFromDecimal(value.as_decimal());
      if (!tree.Insert(key, IndexRid{pid, slot})) {
        table_bp->unpin_page(pid, false);
        if (err) *err = "index build: insert failed at page " + std::to_string(pid) + ", slot " +
                        std::to_string(slot);
        return false;
      }
      ++entries;
    }
    table_bp->unpin_page(pid, false);
  }

  idx_bp.flush_all();
  out->root_page_id = tree.root_page_id();
  out->height = tree.height();
  out->entries = entries;
  return true;
}

}  // namespace sthir
