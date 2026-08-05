// A thin RAII wrapper around a blocking IPv4 UDP socket.
//
// This is the only layer that knows what a file descriptor is. It knows
// nothing about SwiftLink packets: it moves opaque byte buffers. The protocol
// layer produces those buffers and never sees a socket.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace swiftlink::net {

// A resolved IPv4 address in dotted-quad text form plus a host-order port.
// Only numeric addresses are accepted -- no DNS in this milestone.
struct Endpoint {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
};

// Owns a socket file descriptor and closes it in the destructor, including on
// an early return or an exception unwinding past it. That is the whole point:
// there is exactly one close() in the codebase and it cannot be forgotten.
//
// Errors are reported as a false / negative return with errno left set by the
// failing syscall, rather than by throwing. Callers in main() print
// std::strerror(errno) and exit.
class UdpSocket {
 public:
  // Constructs an empty wrapper that owns nothing. open() creates the socket.
  UdpSocket() noexcept = default;
  ~UdpSocket();

  // Copying is deleted because the class owns a unique resource. A copy would
  // duplicate the integer, and then two destructors would close() the same
  // descriptor. The second close() either fails with EBADF or -- much worse --
  // closes a descriptor that some other part of the program has since been
  // handed for a different file. That is a genuinely nasty class of bug, so
  // the compiler is told to make it unwritable.
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Move transfers ownership. These are written out rather than defaulted:
  // a defaulted move would member-wise *copy* the int and leave the source
  // still holding a valid fd, giving exactly the double-close described above.
  // "Defaulted move" is safe only for members that clear themselves when moved
  // from (std::vector, std::unique_ptr); a raw int is not one of them.
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  // socket(AF_INET, SOCK_DGRAM, 0). Returns false and leaves errno set on
  // failure. Closes any descriptor already held.
  [[nodiscard]] bool open() noexcept;

  // bind() to INADDR_ANY on `port`. Servers must call this so the kernel knows
  // which datagrams belong to us. Clients normally skip it and let the kernel
  // pick an ephemeral source port on the first send.
  [[nodiscard]] bool bind(std::uint16_t port) noexcept;

  // Explicit close. Safe to call repeatedly; the destructor calls it too.
  void close() noexcept;

  // SO_RCVTIMEO: recv_from gives up after `timeout` and fails with EAGAIN
  // (== EWOULDBLOCK) instead of blocking forever. This is how the stop-and-wait
  // sender implements its retransmission timeout while staying on a blocking
  // socket. A zero timeout restores indefinite blocking.
  //
  // Milestone 5 replaces this with a non-blocking socket driven by epoll, which
  // is the only way to wait on several sessions at once. For one packet in
  // flight, a receive timeout is simpler and does the same job.
  [[nodiscard]] bool set_receive_timeout(std::chrono::microseconds timeout) noexcept;

  // O_NONBLOCK. With it set, recv_from returns -1/EAGAIN on an empty queue
  // instead of sleeping, which is what lets one epoll loop serve many sessions
  // without any of them blocking the others.
  [[nodiscard]] bool set_nonblocking(bool enabled) noexcept;

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  // Sends the buffer as exactly one datagram. Returns bytes sent, or -1 with
  // errno set. A short send is not a thing for UDP: the datagram goes out whole
  // or not at all.
  [[nodiscard]] std::ptrdiff_t send_to(std::span<const std::byte> bytes,
                                       const Endpoint& destination) noexcept;

  // Blocks until one datagram arrives, copies it into `out`, and fills `from`
  // with the sender's address. Returns bytes received, or -1 with errno set.
  // A datagram larger than `out` is truncated and the excess is discarded --
  // hence a receive buffer sized for the largest packet the format permits.
  [[nodiscard]] std::ptrdiff_t recv_from(std::span<std::byte> out,
                                         Endpoint& from) noexcept;

 private:
  // -1 is the "owns nothing" state: not a valid descriptor, and what a
  // moved-from socket is left holding.
  int fd_ = -1;
};

}  // namespace swiftlink::net
