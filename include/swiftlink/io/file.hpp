// RAII wrapper around a file descriptor, with positional I/O.
//
// Deliberately uses pread/pwrite rather than lseek+read/write. Positional I/O
// carries the offset as an argument instead of mutating the descriptor's shared
// file offset, which means:
//   * a receiver can write chunk 57 before chunk 12 arrives without any seeking
//     dance, and without the order of arrival mattering at all;
//   * writes are idempotent -- a duplicate chunk rewrites the same bytes at the
//     same place, so replaying a retransmission cannot corrupt the file;
//   * it stays correct when the epoll server (milestone 5) interleaves several
//     sessions, since there is no per-descriptor cursor to race on.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace swiftlink::io {

class File {
 public:
  File() noexcept = default;
  ~File();

  // Same ownership reasoning as UdpSocket: one descriptor, one owner.
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;

  // O_RDONLY. Returns false with errno set.
  [[nodiscard]] bool open_read(const std::string& path) noexcept;

  // O_RDWR | O_CREAT | O_TRUNC, mode 0644. Read access is required because
  // the receiver hashes the completed file by reading it back.
  [[nodiscard]] bool open_write(const std::string& path) noexcept;

  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  // Size in bytes via fstat. Returns false with errno set.
  [[nodiscard]] bool size(std::uint64_t& out) const noexcept;

  // pread/pwrite, both looping until the requested count is transferred or an
  // error occurs. A short read means end of file and is reported as the count
  // actually read.
  [[nodiscard]] std::ptrdiff_t read_at(std::span<std::byte> out,
                                       std::uint64_t offset) const noexcept;
  [[nodiscard]] std::ptrdiff_t write_at(std::span<const std::byte> in,
                                        std::uint64_t offset) const noexcept;

  [[nodiscard]] bool sync() const noexcept;

 private:
  int fd_ = -1;
};

}  // namespace swiftlink::io
