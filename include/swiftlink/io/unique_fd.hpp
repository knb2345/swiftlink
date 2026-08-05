// Minimal owning wrapper for a raw file descriptor.
//
// UdpSocket and File both own descriptors but carry domain-specific APIs. The
// epoll server also holds an epoll fd, a timerfd and a signalfd, which have no
// API beyond "close me exactly once". This is that.

#pragma once

#include <unistd.h>

#include <utility>

namespace swiftlink::io {

class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}

  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace swiftlink::io
