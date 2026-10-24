#include "wal/wal_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>

#include "common/crash.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

static void FsyncFile(int fd) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
  }
}

static ssize_t WalPread(int fd, void* buf, size_t count, int64_t offset) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
  DWORD bytesRead = 0;
  if (!ReadFile(h, buf, static_cast<DWORD>(count), &bytesRead, &ov)) {
    DWORD err = GetLastError();
    if (err == ERROR_HANDLE_EOF) return 0;
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(bytesRead);
}

static ssize_t WalPwrite(int fd, const void* buf, size_t count, int64_t offset) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  OVERLAPPED ov{};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
  ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
  DWORD bytesWritten = 0;
  if (!WriteFile(h, buf, static_cast<DWORD>(count), &bytesWritten, &ov)) {
    errno = EIO;
    return -1;
  }
  return static_cast<ssize_t>(bytesWritten);
}
#else
#ifndef O_BINARY
#define O_BINARY 0
#endif

static void FsyncFile(int fd) {
  ::fsync(fd);
}

static ssize_t WalPread(int fd, void* buf, size_t count, int64_t offset) {
  return ::pread(fd, buf, count, offset);
}

static ssize_t WalPwrite(int fd, const void* buf, size_t count, int64_t offset) {
  return ::pwrite(fd, buf, count, offset);
}
#endif

namespace sthir {

namespace {

void EnsureDirExists(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos != std::string::npos) {
    std::string dir = path.substr(0, pos);
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
  }
}

}  // namespace

WalManager::WalManager(std::string wal_path) : wal_path_(std::move(wal_path)) {
  EnsureDirExists(wal_path_);
  fd_ = ::open(wal_path_.c_str(), O_RDWR | O_CREAT | O_BINARY, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("cannot open WAL file: " + wal_path_);
  }

  const off_t size = ::lseek(fd_, 0, SEEK_END);
  if (size < 0) {
    throw std::runtime_error("cannot seek WAL file: " + wal_path_);
  }

  if (size == 0) {
    next_lsn_ = 1;
    flushed_lsn_ = 0;
    buffer_start_lsn_ = 1;
  } else {
    next_lsn_ = static_cast<uint64_t>(size);
    flushed_lsn_ = static_cast<uint64_t>(size);
    buffer_start_lsn_ = next_lsn_;
  }
}

WalManager::~WalManager() {
  Flush();
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

uint64_t WalManager::AppendRecord(LogRecord* record) {
  if (!record) return 0;
  std::lock_guard<std::mutex> lock(latch_);

  record->lsn = next_lsn_;
  std::vector<char> bytes = record->Serialize();
  uint32_t size = static_cast<uint32_t>(bytes.size());

  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  next_lsn_ += size;

  return record->lsn;
}

void WalManager::FlushUpTo(uint64_t lsn) {
  std::lock_guard<std::mutex> lock(latch_);
  if (lsn > 0 && flushed_lsn_ >= lsn) {
    return;
  }

  if (buffer_.empty()) {
    return;
  }

  // Crash injection point before flushing log
  crash_point("mid_log_flush");

  size_t bytes_to_write = buffer_.size();
  size_t done = 0;
  int64_t offset = static_cast<int64_t>(buffer_start_lsn_);

  while (done < bytes_to_write) {
    ssize_t w = WalPwrite(fd_, buffer_.data() + done, bytes_to_write - done, offset + done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("WAL write failed: " + std::string(std::strerror(errno)));
    }
    done += static_cast<size_t>(w);
  }

  FsyncFile(fd_);

  flushed_lsn_ = buffer_start_lsn_ + done;
  buffer_start_lsn_ = flushed_lsn_;
  buffer_.clear();

  // Crash injection hook: after log is durable on disk, but before dirty page is flushed
  crash_point("after_log_before_page");
}

bool WalManager::ReadRecord(uint64_t lsn, LogRecord* out) {
  if (lsn == 0) return false;
  std::lock_guard<std::mutex> lock(latch_);

  // If the record is still in memory buffer, search there first
  if (lsn >= buffer_start_lsn_ && lsn < next_lsn_) {
    size_t buf_offset = static_cast<size_t>(lsn - buffer_start_lsn_);
    if (buf_offset < buffer_.size()) {
      return LogRecord::Deserialize(buffer_.data() + buf_offset, buffer_.size() - buf_offset, out);
    }
    return false;
  }

  // Read header (at least 4 bytes to find total size)
  uint32_t total_size = 0;
  ssize_t r = WalPread(fd_, &total_size, 4, static_cast<int64_t>(lsn));
  if (r < 4 || total_size < 48) {
    return false;
  }

  std::vector<char> buf(total_size);
  size_t done = 0;
  while (done < total_size) {
    r = WalPread(fd_, buf.data() + done, total_size - done, static_cast<int64_t>(lsn + done));
    if (r <= 0) break;
    done += static_cast<size_t>(r);
  }

  if (done < total_size) return false;
  return LogRecord::Deserialize(buf.data(), total_size, out);
}

std::vector<LogRecord> WalManager::ReadAllRecords(uint64_t start_lsn) {
  Flush();  // Ensure everything on disk
  std::lock_guard<std::mutex> lock(latch_);

  std::vector<LogRecord> records;
  uint64_t cur_lsn = (start_lsn == 0) ? 1 : start_lsn;

  const off_t file_len = ::lseek(fd_, 0, SEEK_END);
  if (file_len <= 0) return records;

  while (cur_lsn < static_cast<uint64_t>(file_len)) {
    uint32_t total_size = 0;
    ssize_t r = WalPread(fd_, &total_size, 4, static_cast<int64_t>(cur_lsn));
    if (r < 4 || total_size < 48 || cur_lsn + total_size > static_cast<uint64_t>(file_len)) {
      break;
    }

    std::vector<char> buf(total_size);
    size_t done = 0;
    while (done < total_size) {
      r = WalPread(fd_, buf.data() + done, total_size - done, static_cast<int64_t>(cur_lsn + done));
      if (r <= 0) break;
      done += static_cast<size_t>(r);
    }

    if (done < total_size) break;

    LogRecord rec;
    size_t bytes_read = 0;
    if (LogRecord::Deserialize(buf.data(), total_size, &rec, &bytes_read)) {
      records.push_back(std::move(rec));
      cur_lsn += bytes_read;
    } else {
      break;
    }
  }

  return records;
}

void WalManager::Clear() {
  std::lock_guard<std::mutex> lock(latch_);
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = ::open(wal_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
  next_lsn_ = 1;
  flushed_lsn_ = 0;
  buffer_start_lsn_ = 1;
  buffer_.clear();
}

}  // namespace sthir
