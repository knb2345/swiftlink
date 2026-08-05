// SHA-256 (FIPS 180-4), implemented from scratch.
//
// No OpenSSL, no libcrypto: the project takes no external dependencies, and
// implementing it is the point of the exercise. This is a straightforward
// reference implementation -- correct and readable, not constant-time and not
// hardware-accelerated. It is used for end-to-end file verification, where the
// input is not secret and there is no attacker-controlled timing to leak, so
// side-channel hardening would buy nothing.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace swiftlink::crypto {

inline constexpr std::size_t kSha256DigestSize = 32;

using Sha256Digest = std::array<std::byte, kSha256DigestSize>;

// Incremental hashing, so a large file can be hashed without being held in
// memory all at once.
class Sha256 {
 public:
  Sha256() = default;

  void update(std::span<const std::byte> data);

  // Appends the padding and length, and returns the digest. The object must
  // not be updated afterwards.
  [[nodiscard]] Sha256Digest finish();

 private:
  void compress(const std::byte* block);

  // Initial hash values: the first 32 bits of the fractional parts of the
  // square roots of the first eight primes.
  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<std::byte, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bits_ = 0;
};

// Convenience wrapper for a single buffer.
[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data);

// Lowercase hex, 64 characters.
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

}  // namespace swiftlink::crypto
