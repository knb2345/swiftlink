#include "swiftlink/io/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

namespace swiftlink::io {

File::~File() { close(); }

File::File(File&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

bool File::open_read(const std::string& path) noexcept {
  close();
  fd_ = ::open(path.c_str(), O_RDONLY);
  return fd_ >= 0;
}

bool File::open_write(const std::string& path) noexcept {
  close();
  // O_RDWR, not O_WRONLY: the receiver reads the finished file back to compute
  // its SHA-256 for the end-to-end integrity check, and a write-only
  // descriptor makes pread fail with EBADF.
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  return fd_ >= 0;
}

void File::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool File::size(std::uint64_t& out) const noexcept {
  if (!valid()) {
    errno = EBADF;
    return false;
  }
  struct stat info {};
  if (::fstat(fd_, &info) != 0) {
    return false;
  }
  out = static_cast<std::uint64_t>(info.st_size);
  return true;
}

std::ptrdiff_t File::read_at(std::span<std::byte> out,
                             std::uint64_t offset) const noexcept {
  if (!valid()) {
    errno = EBADF;
    return -1;
  }

  // pread may legitimately return fewer bytes than asked for without being an
  // error, so loop until the buffer is full or we hit end of file.
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t n = ::pread(fd_, out.data() + done, out.size() - done,
                              static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by a signal before transferring anything
      }
      return -1;
    }
    if (n == 0) {
      break;  // end of file
    }
    done += static_cast<std::size_t>(n);
  }
  return static_cast<std::ptrdiff_t>(done);
}

std::ptrdiff_t File::write_at(std::span<const std::byte> in,
                              std::uint64_t offset) const noexcept {
  if (!valid()) {
    errno = EBADF;
    return -1;
  }

  std::size_t done = 0;
  while (done < in.size()) {
    const ssize_t n = ::pwrite(fd_, in.data() + done, in.size() - done,
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      errno = EIO;
      return -1;
    }
    done += static_cast<std::size_t>(n);
  }
  return static_cast<std::ptrdiff_t>(done);
}

bool File::sync() const noexcept {
  if (!valid()) {
    errno = EBADF;
    return false;
  }
  return ::fsync(fd_) == 0;
}

}  // namespace swiftlink::io
