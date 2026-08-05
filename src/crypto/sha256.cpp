#include "swiftlink/crypto/sha256.hpp"

#include <cstring>

namespace swiftlink::crypto {
namespace {

// Round constants: the first 32 bits of the fractional parts of the cube roots
// of the first sixty-four primes.
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                   unsigned bits) noexcept {
  return (value >> bits) | (value << (32 - bits));
}

// SHA-256 defines its message schedule in big-endian terms, the same reason
// the packet header does: the specification is written in bytes, so the code
// converts explicitly rather than trusting host layout.
[[nodiscard]] std::uint32_t read_be32(const std::byte* in) noexcept {
  return (static_cast<std::uint32_t>(in[0]) << 24) |
         (static_cast<std::uint32_t>(in[1]) << 16) |
         (static_cast<std::uint32_t>(in[2]) << 8) |
         static_cast<std::uint32_t>(in[3]);
}

void write_be32(std::byte* out, std::uint32_t value) noexcept {
  out[0] = static_cast<std::byte>((value >> 24) & 0xFFU);
  out[1] = static_cast<std::byte>((value >> 16) & 0xFFU);
  out[2] = static_cast<std::byte>((value >> 8) & 0xFFU);
  out[3] = static_cast<std::byte>(value & 0xFFU);
}

}  // namespace

void Sha256::compress(const std::byte* block) {
  std::array<std::uint32_t, 64> w{};

  // First 16 words come straight from the block.
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = read_be32(block + i * 4);
  }

  // The remaining 48 are derived, which is what spreads each input bit across
  // the whole schedule.
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^
                             rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^
                             rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[i] + w[i];
    const std::uint32_t s0 =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) {
  total_bits_ += static_cast<std::uint64_t>(data.size()) * 8;

  std::size_t offset = 0;

  // Top up a partially filled block first.
  if (buffered_ > 0) {
    const std::size_t want = 64 - buffered_;
    const std::size_t take = (data.size() < want) ? data.size() : want;
    std::memcpy(buffer_.data() + buffered_, data.data(), take);
    buffered_ += take;
    offset = take;

    if (buffered_ < 64) {
      return;  // still not a full block
    }
    compress(buffer_.data());
    buffered_ = 0;
  }

  // Then consume whole blocks straight from the input.
  while (offset + 64 <= data.size()) {
    compress(data.data() + offset);
    offset += 64;
  }

  // Whatever is left starts the next partial block.
  if (offset < data.size()) {
    buffered_ = data.size() - offset;
    std::memcpy(buffer_.data(), data.data() + offset, buffered_);
  }
}

Sha256Digest Sha256::finish() {
  // Padding: a single 1 bit, then zeroes, then the message length in bits as a
  // big-endian 64-bit value, ending on a 64-byte boundary. Encoding the length
  // is what stops two different messages sharing a digest by one being a
  // padded prefix of the other.
  const std::uint64_t length_bits = total_bits_;

  buffer_[buffered_++] = std::byte{0x80};

  if (buffered_ > 56) {
    // No room for the length in this block: fill it, compress, start another.
    while (buffered_ < 64) {
      buffer_[buffered_++] = std::byte{0};
    }
    compress(buffer_.data());
    buffered_ = 0;
  }

  while (buffered_ < 56) {
    buffer_[buffered_++] = std::byte{0};
  }

  for (int i = 7; i >= 0; --i) {
    buffer_[buffered_++] =
        static_cast<std::byte>((length_bits >> (8 * i)) & 0xFFU);
  }
  compress(buffer_.data());

  Sha256Digest digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    write_be32(digest.data() + i * 4, state_[i]);
  }
  return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

std::string to_hex(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(kSha256DigestSize * 2);
  for (const std::byte b : digest) {
    const auto value = static_cast<unsigned char>(b);
    text.push_back(kHex[value >> 4]);
    text.push_back(kHex[value & 0x0F]);
  }
  return text;
}

}  // namespace swiftlink::crypto
