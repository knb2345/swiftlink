// Milestone 1 server: bind a UDP port, block until one datagram arrives,
// decode it, print every header field, exit.
//
// No loop, no concurrency, no epoll -- those are later milestones.

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/protocol/packet.hpp"

namespace {

constexpr std::uint16_t kDefaultPort = 9000;

[[nodiscard]] bool parse_port(const char* text, std::uint16_t& out) {
  const std::string_view view{text};
  unsigned value = 0;
  const auto result =
      std::from_chars(view.data(), view.data() + view.size(), value);
  if (result.ec != std::errc{} || result.ptr != view.data() + view.size() ||
      value == 0 || value > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

// Renders a payload as text, replacing anything unprintable with '.' so a
// binary payload cannot scramble the terminal.
[[nodiscard]] std::string printable(std::span<const std::byte> payload) {
  std::string text;
  text.reserve(payload.size());
  for (const std::byte b : payload) {
    const auto c = static_cast<unsigned char>(b);
    text.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
  }
  return text;
}

void print_header(const swiftlink::protocol::PacketHeader& h) {
  using swiftlink::protocol::to_string;
  std::cout << std::format("  magic                   0x{:08X}\n", h.magic);
  std::cout << std::format("  version                 {}\n", h.version);
  std::cout << std::format("  packet_type             {} ({})\n",
                           static_cast<unsigned>(h.packet_type),
                           to_string(h.packet_type));
  std::cout << std::format("  header_length           {}\n", h.header_length);
  std::cout << std::format("  session_id              {}\n", h.session_id);
  std::cout << std::format("  sequence_number         {}\n", h.sequence_number);
  std::cout << std::format("  acknowledgement_number  {}\n",
                           h.acknowledgement_number);
  std::cout << std::format("  byte_offset             {}\n", h.byte_offset);
  std::cout << std::format("  payload_length          {}\n", h.payload_length);
  std::cout << std::format("  flags                   0x{:04X}\n", h.flags);
  std::cout << std::format("  checksum                0x{:08X}\n", h.checksum);
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = kDefaultPort;
  if (argc > 2) {
    std::cerr << "usage: swiftlink_server [port]\n";
    return 2;
  }
  if (argc == 2 && !parse_port(argv[1], port)) {
    std::cerr << std::format("invalid port: {}\n", argv[1]);
    return 2;
  }

  swiftlink::net::UdpSocket socket;
  if (!socket.open()) {
    std::cerr << std::format("socket() failed: {}\n", std::strerror(errno));
    return 1;
  }
  if (!socket.bind(port)) {
    std::cerr << std::format("bind({}) failed: {}\n", port,
                             std::strerror(errno));
    return 1;
  }

  std::cout << std::format("swiftlink server listening on 0.0.0.0:{}\n", port);
  std::cout << "waiting for one datagram...\n";

  // Sized for the largest packet the format can describe, so a legitimate
  // packet is never silently truncated by recvfrom.
  std::vector<std::byte> buffer(swiftlink::protocol::kHeaderWireSize +
                                swiftlink::protocol::kMaxPayloadSize);

  swiftlink::net::Endpoint peer;
  const std::ptrdiff_t received = socket.recv_from(buffer, peer);
  if (received < 0) {
    std::cerr << std::format("recvfrom() failed: {}\n", std::strerror(errno));
    return 1;
  }

  std::cout << std::format("\nreceived {} bytes from {}:{}\n", received,
                           peer.address, peer.port);

  // Trim to what actually arrived: the rest of `buffer` is untouched zeroes and
  // must not be fed to the decoder, or payload_length validation would fail.
  const std::span<const std::byte> datagram{buffer.data(),
                                            static_cast<std::size_t>(received)};

  const auto result = swiftlink::protocol::deserialize(datagram);
  if (!result.ok()) {
    std::cerr << std::format("decode failed: {}\n",
                             swiftlink::protocol::to_string(result.error()));
    return 1;
  }

  const swiftlink::protocol::Packet& packet = result.value();
  std::cout << "\nheader:\n";
  print_header(packet.header);
  std::cout << std::format("\npayload ({} bytes): \"{}\"\n",
                           packet.payload.size(), printable(packet.payload));

  return 0;
}
