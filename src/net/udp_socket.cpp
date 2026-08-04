#include "swiftlink/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace swiftlink::net {
namespace {

// Endpoint -> sockaddr_in. Returns false if the address is not a valid dotted
// quad (inet_pton does not do DNS, by design: name resolution is a policy
// decision that does not belong in a socket wrapper).
[[nodiscard]] bool to_sockaddr(const Endpoint& endpoint, sockaddr_in& out) noexcept {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  // htons: host-to-network short. The port is a 16-bit field the kernel puts
  // straight into the UDP header, which is big-endian on the wire.
  out.sin_port = htons(endpoint.port);
  return inet_pton(AF_INET, endpoint.address.c_str(), &out.sin_addr) == 1;
}

void from_sockaddr(const sockaddr_in& in, Endpoint& out) noexcept {
  char text[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &in.sin_addr, text, sizeof(text)) != nullptr) {
    out.address = text;
  } else {
    out.address = "?";
  }
  // ntohs: the reverse of htons, network-to-host.
  out.port = ntohs(in.sin_port);
}

}  // namespace

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) {
  // The source must forget the descriptor, or its destructor closes ours.
  other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    // Close what we currently hold before taking over the other descriptor,
    // otherwise assigning over an open socket leaks it.
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

bool UdpSocket::open() noexcept {
  close();
  // AF_INET: IPv4. SOCK_DGRAM: message-oriented, unreliable, unordered --
  // i.e. UDP. The 0 lets the kernel pick the only protocol matching that
  // combination (IPPROTO_UDP).
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  return fd_ >= 0;
}

bool UdpSocket::bind(std::uint16_t port) noexcept {
  if (!valid()) {
    errno = EBADF;
    return false;
  }

  sockaddr_in address{};
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  // INADDR_ANY (0.0.0.0) accepts datagrams arriving on any local interface.
  // htonl for symmetry and correctness: it is a 32-bit network-order field,
  // even though this particular constant is zero and byte order cannot show.
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  // The cast to sockaddr* is the classic C polymorphism of the sockets API:
  // one bind() serves every address family, and sin_family tells the kernel
  // how to read the rest of the structure.
  return ::bind(fd_, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0;
}

void UdpSocket::close() noexcept {
  if (fd_ >= 0) {
    // Retrying close() on EINTR is wrong on Linux: the descriptor is already
    // released by the time the signal is reported, so a retry could close an
    // unrelated descriptor. Close once, drop it, move on.
    ::close(fd_);
    fd_ = -1;
  }
}

std::ptrdiff_t UdpSocket::send_to(std::span<const std::byte> bytes,
                                  const Endpoint& destination) noexcept {
  if (!valid()) {
    errno = EBADF;
    return -1;
  }

  sockaddr_in address{};
  if (!to_sockaddr(destination, address)) {
    errno = EINVAL;
    return -1;
  }

  const ssize_t sent = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                                reinterpret_cast<const sockaddr*>(&address),
                                sizeof(address));
  return static_cast<std::ptrdiff_t>(sent);
}

std::ptrdiff_t UdpSocket::recv_from(std::span<std::byte> out,
                                    Endpoint& from) noexcept {
  if (!valid()) {
    errno = EBADF;
    return -1;
  }

  sockaddr_in address{};
  // Value-result parameter: we pass in the size of our buffer, the kernel
  // writes back how many bytes of address it actually filled in.
  socklen_t address_length = sizeof(address);

  const ssize_t received =
      ::recvfrom(fd_, out.data(), out.size(), 0,
                 reinterpret_cast<sockaddr*>(&address), &address_length);
  if (received < 0) {
    return -1;
  }

  from_sockaddr(address, from);
  return static_cast<std::ptrdiff_t>(received);
}

}  // namespace swiftlink::net
