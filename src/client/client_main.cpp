// Milestone 1 client: build one DATA packet, serialise it, send it, print what
// was sent, exit.
//
// No handshake, no retransmission, no acknowledgement handling -- the packet is
// fired once and we do not care whether it arrives. That is exactly the
// unreliability the later milestones exist to fix.

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/protocol/packet.hpp"

namespace {

constexpr std::uint16_t kDefaultPort = 9000;
constexpr std::string_view kDefaultHost = "127.0.0.1";
constexpr std::string_view kDefaultMessage = "hello from swiftlink";

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

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return bytes;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 4) {
    std::cerr << "usage: swiftlink_client [host] [port] [message]\n";
    return 2;
  }

  const std::string host = (argc > 1) ? argv[1] : std::string{kDefaultHost};
  std::uint16_t port = kDefaultPort;
  if (argc > 2 && !parse_port(argv[2], port)) {
    std::cerr << std::format("invalid port: {}\n", argv[2]);
    return 2;
  }
  const std::string message = (argc > 3) ? argv[3] : std::string{kDefaultMessage};

  namespace proto = swiftlink::protocol;

  proto::PacketHeader header;
  header.packet_type = proto::PacketType::kData;
  header.session_id = 0x0123456789ABCDEFULL;
  header.sequence_number = 1;
  header.acknowledgement_number = 0;
  header.byte_offset = 0;
  header.flags = 0;
  header.checksum = 0;  // CRC32 is a later milestone

  const std::vector<std::byte> payload = to_bytes(message);
  const std::vector<std::byte> wire = proto::serialize(header, payload);
  if (wire.empty()) {
    std::cerr << "serialisation failed (payload too large?)\n";
    return 1;
  }

  swiftlink::net::UdpSocket socket;
  if (!socket.open()) {
    std::cerr << std::format("socket() failed: {}\n", std::strerror(errno));
    return 1;
  }
  // No bind(): the kernel assigns an ephemeral source port on the first sendto.

  const swiftlink::net::Endpoint destination{host, port};
  const std::ptrdiff_t sent = socket.send_to(wire, destination);
  if (sent < 0) {
    std::cerr << std::format("sendto() failed: {}\n", std::strerror(errno));
    return 1;
  }

  std::cout << std::format("sent {} bytes to {}:{}\n", sent, host, port);
  std::cout << std::format("  packet_type     {} ({})\n",
                           static_cast<unsigned>(header.packet_type),
                           proto::to_string(header.packet_type));
  std::cout << std::format("  session_id      {}\n", header.session_id);
  std::cout << std::format("  sequence_number {}\n", header.sequence_number);
  std::cout << std::format("  byte_offset     {}\n", header.byte_offset);
  std::cout << std::format("  payload_length  {}\n", payload.size());
  std::cout << std::format("  payload         \"{}\"\n", message);

  // First 16 header bytes, so the big-endian layout is visible by eye: the
  // magic reads 53 57 4C 4B ("SWLK") and not 4B 4C 57 53.
  std::cout << "  first 16 wire bytes ";
  for (std::size_t i = 0; i < 16 && i < wire.size(); ++i) {
    std::cout << std::format("{:02X} ", static_cast<unsigned>(wire[i]));
  }
  std::cout << '\n';

  return 0;
}
