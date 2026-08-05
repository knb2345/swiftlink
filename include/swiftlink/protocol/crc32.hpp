// CRC-32 (IEEE 802.3), implemented from scratch.
//
// Used as a per-packet integrity check. It is an *error-detection* code, not a
// security mechanism: CRC32 is linear, so anyone who can modify a packet can
// also fix up the checksum. It catches corruption, not tampering. The
// end-to-end SHA-256 in the FIN packet is what covers deliberate modification
// of the file as a whole.
//
// Worth knowing why this is needed at all when UDP already has a checksum:
// the UDP checksum is 16 bits and optional in IPv4 (a zero value means "not
// computed"), so relying on it alone leaves both a weak check and one that may
// not be present.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace swiftlink::protocol {

// Standard CRC-32 over a byte range.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;

// CRC-32 over a serialised packet, treating the four checksum bytes at
// `checksum_offset` as zero.
//
// The checksum cannot cover itself, so both sides must agree on what to
// substitute for those bytes. Computing over a zeroed field means the sender
// and receiver run the identical calculation, and it avoids copying the whole
// packet just to blank four bytes.
[[nodiscard]] std::uint32_t crc32_excluding_checksum(
    std::span<const std::byte> packet, std::size_t checksum_offset) noexcept;

}  // namespace swiftlink::protocol
