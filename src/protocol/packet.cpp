#include "swiftlink/protocol/packet.hpp"

namespace swiftlink::protocol {

bool is_valid_packet_type(std::uint8_t raw) noexcept {
  switch (static_cast<PacketType>(raw)) {
    case PacketType::kStart:
    case PacketType::kStartAck:
    case PacketType::kData:
    case PacketType::kAck:
    case PacketType::kFin:
    case PacketType::kFinAck:
    case PacketType::kError:
      return true;
  }
  // No default label: adding a new enumerator makes the compiler warn here
  // (-Wswitch) instead of letting the new type silently fail validation.
  return false;
}

std::string_view to_string(PacketType type) noexcept {
  switch (type) {
    case PacketType::kStart:
      return "START";
    case PacketType::kStartAck:
      return "START_ACK";
    case PacketType::kData:
      return "DATA";
    case PacketType::kAck:
      return "ACK";
    case PacketType::kFin:
      return "FIN";
    case PacketType::kFinAck:
      return "FIN_ACK";
    case PacketType::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::string_view to_string(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::kNone:
      return "ok";
    case DecodeError::kBufferTooSmall:
      return "buffer smaller than a packet header";
    case DecodeError::kBadMagic:
      return "magic number mismatch (not a SwiftLink packet)";
    case DecodeError::kUnsupportedVersion:
      return "unsupported protocol version";
    case DecodeError::kBadHeaderLength:
      return "declared header_length does not match the wire format";
    case DecodeError::kUnknownPacketType:
      return "unknown packet type";
    case DecodeError::kPayloadLengthMismatch:
      return "declared payload_length disagrees with the bytes received";
  }
  return "unknown error";
}

}  // namespace swiftlink::protocol
