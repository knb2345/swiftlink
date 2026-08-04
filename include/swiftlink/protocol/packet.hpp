// SwiftLink packet header definition and (de)serialisation interface.
//
// This header is deliberately free of any socket/network-API dependency. It
// describes the *format* of a SwiftLink packet and how to turn one into a byte
// buffer and back. Moving those bytes is somebody else's job (see
// swiftlink/net/udp_socket.hpp). Keeping the two apart means the protocol can
// be unit-tested without opening a file descriptor.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace swiftlink::protocol {

// ASCII "SWLK". Lets a receiver reject stray datagrams cheaply: UDP has no
// connection, so anything on the planet can send us a packet on this port.
inline constexpr std::uint32_t kMagic = 0x53574C4BU;

// Bumped whenever the wire format changes incompatibly.
inline constexpr std::uint8_t kVersion = 1;

// The number of bytes the header occupies *on the wire*. This is the number
// serialisation and deserialisation use. It is deliberately NOT
// sizeof(PacketHeader) -- see the note above the struct.
inline constexpr std::size_t kHeaderWireSize = 40;

// payload_length is a uint16_t, so this is a hard format limit, independent of
// any MTU or datagram-size consideration.
inline constexpr std::size_t kMaxPayloadSize = 65535;

enum class PacketType : std::uint8_t {
  kStart = 1,
  kStartAck = 2,
  kData = 3,
  kAck = 4,
  kFin = 5,
  kFinAck = 6,
  kError = 7,
};

// True if `raw` is one of the values above. Used during decoding: a scoped enum
// with a fixed underlying type will happily hold 200, so the type system alone
// does not keep garbage out.
[[nodiscard]] bool is_valid_packet_type(std::uint8_t raw) noexcept;

[[nodiscard]] std::string_view to_string(PacketType type) noexcept;

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------
//
// #pragma pack(1) tells the compiler to insert no padding between members.
//
// Honest note, because this is worth being precise about: with *this* field
// ordering there is no padding to remove on the x86-64 System V ABI anyway.
// Every field already lands on a naturally aligned offset (magic at 0,
// session_id at 8, byte_offset at 24, ...), so sizeof(PacketHeader) is 40 with
// or without the pragma. The pragma is a guard against a future edit
// reordering the fields, not a fix for a problem this layout currently has.
//
// What padding *would* do if the fields were reordered: swap header_length and
// session_id, and the compiler must insert 2 bytes of dead space before
// session_id so it stays 8-byte aligned. sizeof() becomes 40 while the fields
// carrying meaning still total 38, and the 2 dead bytes hold whatever was in
// that memory before.
//
// Even with the pragma, this struct is a *host-side* convenience type. It is
// never blitted onto the wire. See serializer.cpp for why.
#pragma pack(push, 1)
struct PacketHeader {
  std::uint32_t magic = kMagic;
  std::uint8_t version = kVersion;
  PacketType packet_type = PacketType::kData;  // underlying type is uint8_t
  std::uint16_t header_length = static_cast<std::uint16_t>(kHeaderWireSize);
  std::uint64_t session_id = 0;
  std::uint32_t sequence_number = 0;
  std::uint32_t acknowledgement_number = 0;
  std::uint64_t byte_offset = 0;
  std::uint16_t payload_length = 0;
  std::uint16_t flags = 0;
  std::uint32_t checksum = 0;  // always 0 in milestone 1; CRC32 arrives later
};
#pragma pack(pop)

// A tripwire, not a licence to use sizeof() for wire arithmetic. If someone
// adds a field or reorders one into a padded position, the build breaks here
// instead of silently emitting malformed packets.
static_assert(sizeof(PacketHeader) == kHeaderWireSize,
              "PacketHeader gained padding or a field; the wire format and "
              "kHeaderWireSize must be updated deliberately, not by accident");

// A decoded packet: the header plus a copy of the payload bytes.
struct Packet {
  PacketHeader header{};
  std::vector<std::byte> payload{};
};

// ---------------------------------------------------------------------------
// Decoding results
// ---------------------------------------------------------------------------

enum class DecodeError : std::uint8_t {
  kNone = 0,
  kBufferTooSmall,          // fewer bytes than a header
  kBadMagic,                // not a SwiftLink packet
  kUnsupportedVersion,      // SwiftLink, but a version we do not speak
  kBadHeaderLength,         // header_length disagrees with kHeaderWireSize
  kUnknownPacketType,       // packet_type is not in the PacketType enum
  kPayloadLengthMismatch,   // declared payload size != bytes actually present
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

// Either a Packet or a DecodeError, never a half-built packet and never an
// exception. The caller must check ok() before touching value(); a failed
// result holds a default-constructed Packet that carries no meaning.
//
// (std::expected would express this exactly, but it is C++23 and we are on
// C++20, so this is the two-page version of it.)
class DecodeResult {
 public:
  [[nodiscard]] static DecodeResult success(Packet packet) noexcept {
    DecodeResult result;
    result.packet_ = std::move(packet);
    result.error_ = DecodeError::kNone;
    return result;
  }

  [[nodiscard]] static DecodeResult failure(DecodeError error) noexcept {
    DecodeResult result;
    result.error_ = error;
    return result;
  }

  [[nodiscard]] bool ok() const noexcept { return error_ == DecodeError::kNone; }
  [[nodiscard]] DecodeError error() const noexcept { return error_; }

  // Precondition: ok(). Calling this on a failed result gives you a
  // zero-initialised packet, which is why you must check first.
  [[nodiscard]] const Packet& value() const& noexcept { return packet_; }
  [[nodiscard]] Packet&& value() && noexcept { return std::move(packet_); }

 private:
  DecodeResult() = default;

  Packet packet_{};
  DecodeError error_ = DecodeError::kNone;
};

// ---------------------------------------------------------------------------
// Wire conversion
// ---------------------------------------------------------------------------

// Produces kHeaderWireSize + payload.size() bytes: header fields written
// big-endian in the documented order, then the payload verbatim.
//
// `header.payload_length` is ignored and recomputed from payload.size(), so the
// two can never disagree on the wire. A payload longer than kMaxPayloadSize
// cannot be described by the format at all, so it is refused: the function
// returns an empty vector. Callers are expected to chunk before reaching here
// (chunking is a later milestone).
[[nodiscard]] std::vector<std::byte> serialize(const PacketHeader& header,
                                               std::span<const std::byte> payload);

// Validates and decodes a received datagram. Never throws.
[[nodiscard]] DecodeResult deserialize(std::span<const std::byte> buffer);

}  // namespace swiftlink::protocol
