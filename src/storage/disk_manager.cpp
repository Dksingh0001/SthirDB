#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "common/config.h"
#include "common/crash.h"

namespace sthir {

namespace {

[[noreturn]] void ThrowErrno(const std::string& what, const std::string& path) {
  throw std::runtime_error(what + " '" + path + "': " + std::strerror(errno));
}

}  // namespace

DiskManager::DiskManager(std::string db_file) : path_(std::move(db_file)) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) ThrowErrno("cannot open", path_);
  const off_t size = ::lseek(fd_, 0, SEEK_END);
  if (size < 0) ThrowErrno("cannot stat", path_);
  num_pages_ = static_cast<uint32_t>(size / kPageSize);
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) ::close(fd_);
}

void DiskManager::read_page(uint32_t page_id, char* out) {
  if (page_id >= num_pages_) {
    std::memset(out, 0, kPageSize);
    return;
  }
  const uint64_t offset = static_cast<uint64_t>(page_id) * kPageSize;
  size_t done = 0;
  while (done < kPageSize) {
    const ssize_t r = ::pread(fd_, out + done, kPageSize - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("read failed", path_);
    }
    if (r == 0) break;  // sparse tail: treat rest as zeros
    done += static_cast<size_t>(r);
  }
  if (done < kPageSize) std::memset(out + done, 0, kPageSize - done);
}

void DiskManager::write_page(uint32_t page_id, const char* in) {
  WriteAt(static_cast<uint64_t>(page_id) * kPageSize, in, kPageSize);
}

uint32_t DiskManager::allocate_page() {
  const uint32_t page_id = num_pages_;
  std::vector<char> zero(kPageSize, 0);
  WriteAt(static_cast<uint64_t>(page_id) * kPageSize, zero.data(), kPageSize);
  num_pages_ = page_id + 1;
  return page_id;
}

void DiskManager::WriteAt(uint64_t offset, const char* buf, size_t n) {
  // Single crash-injection hook for every page write (Phase 5B names the
  // specific hazards; until then this is a no-op outside test runs).
  crash_point("page_write");

  size_t done = 0;
  while (done < n) {
    const ssize_t w = ::pwrite(fd_, buf + done, n - done, static_cast<off_t>(offset + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      ThrowErrno("write failed", path_);
    }
    done += static_cast<size_t>(w);
  }
}

}  // namespace sthir
