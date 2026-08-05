// Field-by-field conversion between PacketHeader and its big-endian wire form.
//
// -------------------------------------------------------------------------
// Why this file does not just memcpy the struct
// -------------------------------------------------------------------------
// The tempting one-liner is:
//
//     std::memcpy(buffer.data(), &header, sizeof(header));   // DO NOT
//
// It is wrong for five independent reasons, any one of which is fatal:
//
// 1. Byte order. memcpy copies the host's representation. On x86-64 (little
//    endian) magic 0x53574C4B goes out as 4B 4C 57 53. A big-endian receiver
//    reads 0x4B4C5753 and rejects it. Two little-endian machines would
//    interoperate and hide the bug until the day one peer is not x86 -- the
//    worst possible time to find out.
//
// 2. Layout is an ABI promise, not a language promise. #pragma pack is a
//    compiler extension, spelled differently by different compilers, and the
//    standard says nothing about the resulting layout. Compile the two ends
//    with different compilers, or on a target whose ABI orders or aligns
//    differently, and the structs stop agreeing. A wire format must be defined
//    in bytes and offsets, not "whatever my compiler did today".
//
// 3. Padding bytes are uninitialised. If a future edit reorders fields into a
//    padded layout, memcpy sends whatever stale stack or heap bytes happened
//    to sit in the gap: non-deterministic packets, and a genuine information
//    leak onto the network.
//
// 4. The reverse direction is worse. Decoding by memcpy-ing (or worse,
//    reinterpret_cast-ing) a received buffer into a PacketHeader means reading
//    a hostile, attacker-controlled buffer as a struct. reinterpret_cast of a
//    char buffer to a struct pointer breaks strict aliasing and the pointer is
//    very likely misaligned -- undefined behaviour, a SIGBUS on strict
//    alignment architectures like SPARC or some ARM configurations, and a
//    UBSan report on x86.
//
// 5. Taking the address of a packed member (&header.session_id) yields an
//    under-aligned pointer. Passing it anywhere that expects a properly
//    aligned uint64_t* is undefined behaviour even on x86.
//
// Writing byte i explicitly costs a few lines and removes all five problems.
// The functions below produce identical output on every machine because they
// are defined in terms of arithmetic (shifts and masks), not in terms of how
// this CPU happens to store integers.

#include <cstring>
#include <utility>

#include "swiftlink/protocol/crc32.hpp"
#include "swiftlink/protocol/packet.hpp"

namespace swiftlink::protocol {
namespace {

// ---------------------------------------------------------------------------
// Big-endian primitives
// ---------------------------------------------------------------------------
//
// These are host-endianness-agnostic on purpose. `value >> 8` means "divide by
// 256" in the abstract machine -- it is arithmetic on a number, not a
// statement about which byte sits at which address. So the same source
// produces the same bytes on a little-endian and a big-endian host with no
// #ifdef anywhere.
//
// libc's htons()/htonl() do the same job for 16- and 32-bit values, but there
// is no portable 64-bit equivalent (htonll is not in POSIX; glibc offers
// htobe64 from <endian.h>, which is a glibc extension). Since two fields here
// are 64-bit, a uniform set of helpers is cleaner than mixing htonl with a
// hand-rolled 64-bit path.

void write_be16(std::byte* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::byte>((value >> 8) & 0xFFU);
  out[1] = static_cast<std::byte>(value & 0xFFU);
}

void write_be32(std::byte* out, std::uint32_t value) noexcept {
  out[0] = static_cast<std::byte>((value >> 24) & 0xFFU);
  out[1] = static_cast<std::byte>((value >> 16) & 0xFFU);
  out[2] = static_cast<std::byte>((value >> 8) & 0xFFU);
  out[3] = static_cast<std::byte>(value & 0xFFU);
}

void write_be64(std::byte* out, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    const unsigned shift = static_cast<unsigned>(56 - 8 * i);
    out[i] = static_cast<std::byte>((value >> shift) & 0xFFU);
  }
}

[[nodiscard]] std::uint8_t read_be8(const std::byte* in) noexcept {
  return static_cast<std::uint8_t>(in[0]);
}

[[nodiscard]] std::uint16_t read_be16(const std::byte* in) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) |
                                    static_cast<std::uint16_t>(in[1]));
}

[[nodiscard]] std::uint32_t read_be32(const std::byte* in) noexcept {
  return (static_cast<std::uint32_t>(in[0]) << 24) |
         (static_cast<std::uint32_t>(in[1]) << 16) |
         (static_cast<std::uint32_t>(in[2]) << 8) |
         static_cast<std::uint32_t>(in[3]);
}

[[nodiscard]] std::uint64_t read_be64(const std::byte* in) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(in[i]);
  }
  return value;
}

// Byte offsets of each field on the wire. Named so that serialise and
// deserialise cannot drift apart: both index the same constants.
namespace offset {
inline constexpr std::size_t kMagic = 0;
inline constexpr std::size_t kVersion = 4;
inline constexpr std::size_t kPacketType = 5;
inline constexpr std::size_t kHeaderLength = 6;
inline constexpr std::size_t kSessionId = 8;
inline constexpr std::size_t kSequenceNumber = 16;
inline constexpr std::size_t kAckNumber = 20;
inline constexpr std::size_t kByteOffset = 24;
inline constexpr std::size_t kPayloadLength = 32;
inline constexpr std::size_t kFlags = 34;
inline constexpr std::size_t kChecksum = 36;
inline constexpr std::size_t kEnd = 40;
}  // namespace offset

static_assert(offset::kEnd == kHeaderWireSize,
              "field offset table and kHeaderWireSize disagree");

}  // namespace

std::vector<std::byte> serialize(const PacketHeader& header,
                                 std::span<const std::byte> payload) {
  if (payload.size() > kMaxPayloadSize) {
    // payload_length physically cannot describe this. Refuse rather than
    // truncate silently: a caller that oversized its chunk has a bug we should
    // not paper over.
    return {};
  }

  std::vector<std::byte> buffer(kHeaderWireSize + payload.size());
  std::byte* out = buffer.data();

  write_be32(out + offset::kMagic, header.magic);

  // Single-byte fields need no conversion: byte order is only a question of
  // how a multi-byte value is split across addresses.
  out[offset::kVersion] = static_cast<std::byte>(header.version);
  out[offset::kPacketType] = static_cast<std::byte>(header.packet_type);

  write_be16(out + offset::kHeaderLength, header.header_length);
  write_be64(out + offset::kSessionId, header.session_id);
  write_be32(out + offset::kSequenceNumber, header.sequence_number);
  write_be32(out + offset::kAckNumber, header.acknowledgement_number);
  write_be64(out + offset::kByteOffset, header.byte_offset);

  // Derived from the payload actually being sent, not copied from the caller's
  // header. This makes "declared length disagrees with real length" impossible
  // to produce by accident on the sending side.
  write_be16(out + offset::kPayloadLength,
             static_cast<std::uint16_t>(payload.size()));

  write_be16(out + offset::kFlags, header.flags);

  // The checksum field must be zero while the CRC is computed, because it
  // cannot cover itself. It already is -- the buffer was value-initialised --
  // but this states the requirement rather than relying on it.
  write_be32(out + offset::kChecksum, 0);

  if (!payload.empty()) {
    // memcpy is fine here: this is an opaque byte sequence, not a struct. No
    // layout, alignment, or endianness question exists for a run of bytes.
    std::memcpy(out + kHeaderWireSize, payload.data(), payload.size());
  }

  // Computed last, over header *and* payload, so corruption anywhere in the
  // datagram is detected.
  write_be32(out + offset::kChecksum,
             crc32_excluding_checksum(buffer, offset::kChecksum));

  return buffer;
}

DecodeResult deserialize(std::span<const std::byte> buffer) {
  // Validation order matters: each check makes the next one safe to perform.
  // We cannot read the magic before knowing 4 bytes exist, and we should not
  // interpret any field before knowing this is a SwiftLink packet of a version
  // whose layout we actually know.

  if (buffer.size() < kHeaderWireSize) {
    return DecodeResult::failure(DecodeError::kBufferTooSmall);
  }

  const std::byte* in = buffer.data();

  PacketHeader header{};
  header.magic = read_be32(in + offset::kMagic);
  if (header.magic != kMagic) {
    return DecodeResult::failure(DecodeError::kBadMagic);
  }

  header.version = read_be8(in + offset::kVersion);
  if (header.version != kVersion) {
    return DecodeResult::failure(DecodeError::kUnsupportedVersion);
  }

  const std::uint8_t raw_type = read_be8(in + offset::kPacketType);
  if (!is_valid_packet_type(raw_type)) {
    return DecodeResult::failure(DecodeError::kUnknownPacketType);
  }
  header.packet_type = static_cast<PacketType>(raw_type);

  header.header_length = read_be16(in + offset::kHeaderLength);
  if (header.header_length != kHeaderWireSize) {
    // Version 1 has exactly one header size. When header extensions arrive,
    // this becomes a >= check plus a skip of the extra bytes.
    return DecodeResult::failure(DecodeError::kBadHeaderLength);
  }

  header.session_id = read_be64(in + offset::kSessionId);
  header.sequence_number = read_be32(in + offset::kSequenceNumber);
  header.acknowledgement_number = read_be32(in + offset::kAckNumber);
  header.byte_offset = read_be64(in + offset::kByteOffset);
  header.payload_length = read_be16(in + offset::kPayloadLength);
  header.flags = read_be16(in + offset::kFlags);
  header.checksum = read_be32(in + offset::kChecksum);

  // The declared length must match the bytes actually present -- exactly, in
  // both directions. Trusting a length field from the network and copying that
  // many bytes out of a shorter buffer is the classic remote read overflow;
  // accepting a longer buffer would mean silently ignoring trailing data.
  const std::size_t actual_payload = buffer.size() - kHeaderWireSize;
  if (actual_payload != header.payload_length) {
    return DecodeResult::failure(DecodeError::kPayloadLengthMismatch);
  }

  // Checked only once the length is known to be consistent, so the CRC is
  // computed over exactly the bytes the sender covered -- no more, no less.
  if (crc32_excluding_checksum(buffer, offset::kChecksum) != header.checksum) {
    return DecodeResult::failure(DecodeError::kChecksumMismatch);
  }

  Packet packet;
  packet.header = header;
  packet.payload.assign(buffer.begin() + static_cast<std::ptrdiff_t>(kHeaderWireSize),
                        buffer.end());

  return DecodeResult::success(std::move(packet));
}

}  // namespace swiftlink::protocol
