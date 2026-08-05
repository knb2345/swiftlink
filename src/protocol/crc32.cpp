#include "swiftlink/protocol/crc32.hpp"

#include <array>

namespace swiftlink::protocol {
namespace {

// The reflected form of the IEEE 802.3 polynomial
// x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10 + x^8 + x^7 + x^5 +
// x^4 + x^2 + x + 1. Reflected because the standard processes each byte
// least-significant bit first.
constexpr std::uint32_t kPolynomial = 0xEDB88320U;

// A 256-entry lookup table turns the per-bit loop into one table lookup per
// byte. Built at compile time, so it costs no startup work and lives in
// read-only memory.
constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1) ^ kPolynomial : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

[[nodiscard]] std::uint32_t update(std::uint32_t crc, std::byte b) noexcept {
  const auto index = static_cast<std::uint8_t>(
      (crc ^ static_cast<std::uint32_t>(b)) & 0xFFU);
  return kTable[index] ^ (crc >> 8);
}

}  // namespace

std::uint32_t crc32(std::span<const std::byte> data) noexcept {
  // Starting at all-ones and inverting at the end are part of the standard.
  // They make the CRC sensitive to leading zero bytes, which a plain zero
  // seed would ignore.
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::byte b : data) {
    crc = update(crc, b);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32_excluding_checksum(std::span<const std::byte> packet,
                                       std::size_t checksum_offset) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;

  for (std::size_t i = 0; i < packet.size(); ++i) {
    // Substitute zero for the four bytes the checksum itself occupies.
    const bool in_checksum =
        i >= checksum_offset && i < checksum_offset + 4;
    crc = update(crc, in_checksum ? std::byte{0} : packet[i]);
  }

  return crc ^ 0xFFFFFFFFU;
}

}  // namespace swiftlink::protocol
