#pragma once

#include <cstddef>
#include <cstdint>

namespace sthir {

inline constexpr const char* kVersion = "0.1.0";

// On-disk page geometry — locked for the whole project (see docs/scope.md).
inline constexpr uint32_t kPageSize = 8192;
inline constexpr uint32_t kPageHeaderSize = 16;
inline constexpr uint32_t kSlotEntrySize = 4;  // uint16 offset + uint16 length

// Default buffer-pool size in frames.
inline constexpr std::size_t kDefaultPoolFrames = 64;

// Runtime data locations (gitignored).
inline constexpr const char* kDataDir = "./data/retail";
inline constexpr const char* kCatalogPath = "./data/retail/catalog.json";
inline constexpr const char* kItemsFilePath = "./data/retail/items.dat";

// Phase 1 fixed demo row: [int32 id][char[50] name][double price][bool deleted].
inline constexpr uint32_t kRowIdSize = 4;
inline constexpr uint32_t kRowNameSize = 50;
inline constexpr uint32_t kRowPriceSize = 8;
inline constexpr uint32_t kRowDeletedSize = 1;
inline constexpr uint32_t kRowSize = kRowIdSize + kRowNameSize + kRowPriceSize + kRowDeletedSize;  // 63

}  // namespace sthir
