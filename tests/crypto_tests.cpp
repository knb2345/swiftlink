// Tests for SHA-256, CRC-32 and filename sanitisation.
//
// The SHA-256 cases are published vectors, not values produced by this
// implementation. That distinction matters: a round-trip test where both ends
// use the same code would pass even if the algorithm were wrong, because the
// two sides would be wrong identically. Only an externally-sourced expected
// value proves the implementation computes SHA-256 rather than "some hash".

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "swiftlink/crypto/sha256.hpp"
#include "swiftlink/protocol/crc32.hpp"
#include "swiftlink/transfer/filename.hpp"

namespace crypto = swiftlink::crypto;
namespace proto = swiftlink::protocol;
namespace xfer = swiftlink::transfer;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* file, int line,
           std::string_view expression, std::string_view detail = {}) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::cerr << file << ":" << line << ": FAILED: " << expression;
    if (!detail.empty()) {
      std::cerr << "\n    " << detail;
    }
    std::cerr << '\n';
  }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_MSG(cond, msg) check((cond), __FILE__, __LINE__, #cond, (msg))

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

void expect_sha256(std::string_view input, std::string_view expected_hex) {
  const std::string actual = crypto::to_hex(crypto::sha256(bytes_of(input)));
  CHECK_MSG(actual == expected_hex,
            "expected: " + std::string(expected_hex) + "\n    actual:   " + actual);
}

// ---------------------------------------------------------------------------
// SHA-256, against published FIPS 180-4 / NIST CAVP vectors
// ---------------------------------------------------------------------------

void test_sha256_empty() {
  expect_sha256(
      "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void test_sha256_abc() {
  expect_sha256(
      "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void test_sha256_two_block() {
  // 56 bytes: forces the length to spill into a second padding block, which is
  // the case a naive padding implementation gets wrong.
  expect_sha256(
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void test_sha256_exactly_one_block() {
  // 55 bytes: the largest message whose padding still fits in one block.
  expect_sha256(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
}

void test_sha256_million_a() {
  // The classic one-million-'a' vector, fed in awkward chunk sizes so the
  // incremental buffering path is exercised rather than a single update().
  crypto::Sha256 hasher;
  const std::vector<std::byte> block(1000, static_cast<std::byte>('a'));
  for (int i = 0; i < 1000; ++i) {
    hasher.update(block);
  }
  CHECK(crypto::to_hex(hasher.finish()) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_sha256_incremental_matches_oneshot() {
  const std::vector<std::byte> data = bytes_of(
      "the quick brown fox jumps over the lazy dog, repeatedly and at length");

  const crypto::Sha256Digest oneshot = crypto::sha256(data);

  // Feed it one byte at a time: worst case for the buffering logic.
  crypto::Sha256 hasher;
  for (const std::byte b : data) {
    hasher.update(std::span<const std::byte>{&b, 1});
  }
  CHECK(hasher.finish() == oneshot);
}

// ---------------------------------------------------------------------------
// CRC-32, against published values
// ---------------------------------------------------------------------------

void test_crc32_known_vectors() {
  CHECK(proto::crc32(bytes_of("")) == 0x00000000U);
  // "123456789" -> 0xCBF43926 is the standard CRC-32 check value.
  CHECK(proto::crc32(bytes_of("123456789")) == 0xCBF43926U);
  CHECK(proto::crc32(bytes_of("a")) == 0xE8B7BE43U);
}

void test_crc32_detects_single_bit_flip() {
  std::vector<std::byte> data = bytes_of("swiftlink packet payload");
  const std::uint32_t original = proto::crc32(data);

  for (std::size_t i = 0; i < data.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> flipped = data;
      flipped[i] = static_cast<std::byte>(static_cast<unsigned char>(flipped[i]) ^
                                          (1U << bit));
      CHECK(proto::crc32(flipped) != original);
    }
  }
}

void test_crc32_excluding_checksum_ignores_that_field() {
  std::vector<std::byte> packet(48, std::byte{0x5A});
  const std::uint32_t first = proto::crc32_excluding_checksum(packet, 36);

  // Scribbling on the four checksum bytes must not change the result, since
  // they are substituted with zero.
  for (std::size_t i = 36; i < 40; ++i) {
    packet[i] = std::byte{0xFF};
  }
  CHECK(proto::crc32_excluding_checksum(packet, 36) == first);

  // Any other byte must change it.
  packet[35] = std::byte{0x00};
  CHECK(proto::crc32_excluding_checksum(packet, 36) != first);
}

// ---------------------------------------------------------------------------
// Filename sanitisation
// ---------------------------------------------------------------------------

void expect_rejected(std::string_view raw) {
  std::string out;
  const auto rejection = xfer::sanitize_filename(raw, out);
  CHECK_MSG(rejection != xfer::FilenameRejection::kNone,
            "should have been rejected: '" + std::string(raw) + "'");
  CHECK(out.empty());
}

void expect_accepted(std::string_view raw, std::string_view expected) {
  std::string out;
  const auto rejection = xfer::sanitize_filename(raw, out);
  CHECK_MSG(rejection == xfer::FilenameRejection::kNone,
            "should have been accepted: '" + std::string(raw) + "' (" +
                std::string(xfer::to_string(rejection)) + ")");
  CHECK_MSG(out == expected,
            "expected: '" + std::string(expected) + "'  actual: '" + out + "'");
}

void test_filename_accepts_ordinary_names() {
  expect_accepted("report.pdf", "report.pdf");
  expect_accepted("archive.tar.gz", "archive.tar.gz");
  expect_accepted("my_file-2026.bin", "my_file-2026.bin");
  expect_accepted("UPPER_case.TXT", "UPPER_case.TXT");
}

void test_filename_strips_directories() {
  // Taking the basename is what defuses traversal.
  expect_accepted("/etc/passwd", "passwd");
  expect_accepted("../../../../etc/shadow", "shadow");
  expect_accepted("subdir/nested/file.bin", "file.bin");
  expect_accepted("./file.bin", "file.bin");
}

void test_filename_rejects_traversal_and_tricks() {
  expect_rejected("..");
  expect_rejected(".");
  expect_rejected("../..");            // basename is ".."
  expect_rejected("....//....//etc/"); // ends in a separator
  expect_rejected("/");
  expect_rejected("dir/");
  expect_rejected("");
}

void test_filename_rejects_dangerous_characters() {
  expect_rejected(std::string_view("evil\0.txt", 9));  // embedded NUL
  expect_rejected("file\nname");
  expect_rejected("file with spaces");
  expect_rejected("file;rm -rf /");
  expect_rejected("$(whoami)");
  expect_rejected("back\\slash");
  expect_rejected("quote'name");
  expect_rejected("-rf");  // would be read as an option
  expect_rejected(std::string(300, 'a'));
}

void test_join_path_cannot_escape() {
  std::string safe;
  CHECK(xfer::sanitize_filename("../../etc/passwd", safe) ==
        xfer::FilenameRejection::kNone);
  const std::string joined = xfer::join_path("/var/spool/swiftlink", safe);
  CHECK(joined == "/var/spool/swiftlink/passwd");
  CHECK(joined.find("..") == std::string::npos);
}

// ---------------------------------------------------------------------------

struct TestCase {
  const char* name;
  void (*run)();
};

constexpr TestCase kTests[] = {
    {"sha256_empty", test_sha256_empty},
    {"sha256_abc", test_sha256_abc},
    {"sha256_two_block", test_sha256_two_block},
    {"sha256_exactly_one_block", test_sha256_exactly_one_block},
    {"sha256_million_a", test_sha256_million_a},
    {"sha256_incremental_matches_oneshot",
     test_sha256_incremental_matches_oneshot},
    {"crc32_known_vectors", test_crc32_known_vectors},
    {"crc32_detects_single_bit_flip", test_crc32_detects_single_bit_flip},
    {"crc32_excluding_checksum_ignores_that_field",
     test_crc32_excluding_checksum_ignores_that_field},
    {"filename_accepts_ordinary_names", test_filename_accepts_ordinary_names},
    {"filename_strips_directories", test_filename_strips_directories},
    {"filename_rejects_traversal_and_tricks",
     test_filename_rejects_traversal_and_tricks},
    {"filename_rejects_dangerous_characters",
     test_filename_rejects_dangerous_characters},
    {"join_path_cannot_escape", test_join_path_cannot_escape},
};

}  // namespace

int main() {
  int failed_cases = 0;
  for (const TestCase& test : kTests) {
    const int before = g_failures;
    test.run();
    const bool passed = (g_failures == before);
    if (!passed) {
      ++failed_cases;
    }
    std::cout << (passed ? "[  PASS  ] " : "[  FAIL  ] ") << test.name << '\n';
  }

  std::cout << "\n"
            << (sizeof(kTests) / sizeof(kTests[0])) << " cases, " << g_checks
            << " checks, " << failed_cases << " failed cases, " << g_failures
            << " failed checks\n";
  return failed_cases == 0 ? 0 : 1;
}
