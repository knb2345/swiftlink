// Unit tests for SwiftLink packet serialisation.
//
// Hand-rolled harness: the project has a no-external-dependencies rule, which
// rules out fetching GoogleTest. All this needs to do is run every case, report
// each failure with a file and line, and exit non-zero if anything failed --
// which is the part of a test framework that actually matters here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "swiftlink/protocol/packet.hpp"

namespace proto = swiftlink::protocol;

namespace {

int g_checks = 0;
int g_failures = 0;

void report_failure(const char* file, int line, std::string_view expression,
                    std::string_view detail) {
  ++g_failures;
  std::cerr << file << ":" << line << ": FAILED: " << expression;
  if (!detail.empty()) {
    std::cerr << "\n    " << detail;
  }
  std::cerr << '\n';
}

void check(bool condition, const char* file, int line,
           std::string_view expression) {
  ++g_checks;
  if (!condition) {
    report_failure(file, line, expression, {});
  }
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* file, int line,
              std::string_view expression) {
  ++g_checks;
  if (!(actual == expected)) {
    std::string detail = "expected: " + std::to_string(expected) +
                         "\n    actual:   " + std::to_string(actual);
    report_failure(file, line, expression, detail);
  }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(actual, expected) \
  check_eq((actual), (expected), __FILE__, __LINE__, #actual " == " #expected)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

// A header with every field set to a distinct, non-zero, deliberately
// asymmetric value. Asymmetry matters: 0x1122334455667788 read backwards is a
// different number, whereas a palindromic value would let a byte-order bug pass.
[[nodiscard]] proto::PacketHeader sample_header() {
  proto::PacketHeader header;
  header.packet_type = proto::PacketType::kAck;
  header.session_id = 0x1122334455667788ULL;
  header.sequence_number = 0x0A0B0C0DU;
  header.acknowledgement_number = 0x01020304U;
  header.byte_offset = 0x00FF00FF00FF00FEULL;
  header.flags = 0xBEEFU;
  header.checksum = 0;  // milestone 1: always zero
  return header;
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

void test_round_trip_preserves_all_fields() {
  const proto::PacketHeader sent = sample_header();
  const std::vector<std::byte> payload = bytes_of("swiftlink payload");

  const std::vector<std::byte> wire = proto::serialize(sent, payload);
  CHECK_EQ(wire.size(), proto::kHeaderWireSize + payload.size());

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(result.ok());
  if (!result.ok()) {
    return;
  }

  const proto::PacketHeader& got = result.value().header;
  CHECK_EQ(got.magic, proto::kMagic);
  CHECK_EQ(got.version, proto::kVersion);
  CHECK_EQ(static_cast<unsigned>(got.packet_type),
           static_cast<unsigned>(sent.packet_type));
  CHECK_EQ(got.header_length, sent.header_length);
  CHECK_EQ(got.session_id, sent.session_id);
  CHECK_EQ(got.sequence_number, sent.sequence_number);
  CHECK_EQ(got.acknowledgement_number, sent.acknowledgement_number);
  CHECK_EQ(got.byte_offset, sent.byte_offset);
  CHECK_EQ(got.payload_length, static_cast<std::uint16_t>(payload.size()));
  CHECK_EQ(got.flags, sent.flags);
  CHECK_EQ(got.checksum, sent.checksum);

  CHECK(result.value().payload == payload);
}

void test_round_trip_with_empty_payload() {
  const std::vector<std::byte> wire = proto::serialize(sample_header(), {});
  CHECK_EQ(wire.size(), proto::kHeaderWireSize);

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(result.ok());
  if (result.ok()) {
    CHECK_EQ(result.value().header.payload_length, 0U);
    CHECK(result.value().payload.empty());
  }
}

void test_round_trip_at_maximum_payload() {
  const std::vector<std::byte> payload(proto::kMaxPayloadSize, std::byte{0xAB});
  const std::vector<std::byte> wire = proto::serialize(sample_header(), payload);
  CHECK_EQ(wire.size(), proto::kHeaderWireSize + proto::kMaxPayloadSize);

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(result.ok());
  if (result.ok()) {
    CHECK_EQ(result.value().header.payload_length, 65535U);
    CHECK(result.value().payload == payload);
  }
}

void test_oversized_payload_is_refused() {
  const std::vector<std::byte> payload(proto::kMaxPayloadSize + 1, std::byte{0});
  CHECK(proto::serialize(sample_header(), payload).empty());
}

// ---------------------------------------------------------------------------
// Byte order
// ---------------------------------------------------------------------------
//
// A round trip alone cannot catch an endianness bug: a serialiser and
// deserialiser that are wrong in the same direction agree with each other
// perfectly. Only asserting the exact bytes on the wire pins the format down.

void test_wire_layout_is_big_endian() {
  proto::PacketHeader header = sample_header();
  header.packet_type = proto::PacketType::kData;

  const std::vector<std::byte> wire = proto::serialize(header, bytes_of("xy"));
  CHECK_EQ(wire.size(), proto::kHeaderWireSize + 2);
  if (wire.size() < proto::kHeaderWireSize) {
    return;
  }

  const auto at = [&wire](std::size_t index) {
    return static_cast<unsigned>(wire[index]);
  };

  // magic 0x53574C4B == "SWLK", most significant byte first.
  CHECK_EQ(at(0), 0x53U);
  CHECK_EQ(at(1), 0x57U);
  CHECK_EQ(at(2), 0x4CU);
  CHECK_EQ(at(3), 0x4BU);

  CHECK_EQ(at(4), 1U);  // version
  CHECK_EQ(at(5), 3U);  // packet_type DATA

  // header_length 40 == 0x0028
  CHECK_EQ(at(6), 0x00U);
  CHECK_EQ(at(7), 0x28U);

  // session_id 0x1122334455667788
  CHECK_EQ(at(8), 0x11U);
  CHECK_EQ(at(9), 0x22U);
  CHECK_EQ(at(10), 0x33U);
  CHECK_EQ(at(11), 0x44U);
  CHECK_EQ(at(12), 0x55U);
  CHECK_EQ(at(13), 0x66U);
  CHECK_EQ(at(14), 0x77U);
  CHECK_EQ(at(15), 0x88U);

  // sequence_number 0x0A0B0C0D
  CHECK_EQ(at(16), 0x0AU);
  CHECK_EQ(at(17), 0x0BU);
  CHECK_EQ(at(18), 0x0CU);
  CHECK_EQ(at(19), 0x0DU);

  // acknowledgement_number 0x01020304
  CHECK_EQ(at(20), 0x01U);
  CHECK_EQ(at(21), 0x02U);
  CHECK_EQ(at(22), 0x03U);
  CHECK_EQ(at(23), 0x04U);

  // byte_offset 0x00FF00FF00FF00FE
  CHECK_EQ(at(24), 0x00U);
  CHECK_EQ(at(25), 0xFFU);
  CHECK_EQ(at(30), 0x00U);
  CHECK_EQ(at(31), 0xFEU);

  // payload_length 2
  CHECK_EQ(at(32), 0x00U);
  CHECK_EQ(at(33), 0x02U);

  // flags 0xBEEF
  CHECK_EQ(at(34), 0xBEU);
  CHECK_EQ(at(35), 0xEFU);

  // checksum 0
  CHECK_EQ(at(36), 0x00U);
  CHECK_EQ(at(39), 0x00U);

  // Payload starts immediately after the header, unmodified.
  CHECK_EQ(at(40), static_cast<unsigned>('x'));
  CHECK_EQ(at(41), static_cast<unsigned>('y'));
}

void test_header_struct_size_matches_wire_size() {
  // True on this ABI, and asserted at compile time in packet.hpp too. The
  // point of restating it here is that the equality is a coincidence of this
  // field ordering, not a guarantee -- and the serialiser must never rely on it.
  CHECK_EQ(sizeof(proto::PacketHeader), proto::kHeaderWireSize);
}

// ---------------------------------------------------------------------------
// Rejection cases
// ---------------------------------------------------------------------------

void test_rejects_bad_magic() {
  std::vector<std::byte> wire = proto::serialize(sample_header(), bytes_of("hi"));
  wire[0] = std::byte{0x00};  // 0x00574C4B is not SwiftLink

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kBadMagic);
}

void test_rejects_buffer_shorter_than_header() {
  const std::vector<std::byte> wire = proto::serialize(sample_header(), {});

  // One byte short of a header: there is not even enough to read the magic.
  const std::span<const std::byte> truncated{wire.data(),
                                             proto::kHeaderWireSize - 1};
  const proto::DecodeResult result = proto::deserialize(truncated);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kBufferTooSmall);

  // And the degenerate cases.
  CHECK(!proto::deserialize({}).ok());
  CHECK(proto::deserialize({}).error() == proto::DecodeError::kBufferTooSmall);
}

void test_rejects_truncated_payload() {
  // Header declares 32 payload bytes; only 5 arrive. This is the case that
  // matters most: a decoder that trusted payload_length here would read 27
  // bytes past the end of the datagram.
  const std::vector<std::byte> payload(32, std::byte{0x5A});
  const std::vector<std::byte> wire = proto::serialize(sample_header(), payload);

  const std::span<const std::byte> truncated{wire.data(),
                                             proto::kHeaderWireSize + 5};
  const proto::DecodeResult result = proto::deserialize(truncated);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kPayloadLengthMismatch);
}

void test_rejects_payload_longer_than_declared() {
  // The other direction: 8 bytes declared, 12 present. Accepting this would
  // mean silently discarding 4 bytes that someone meant to send.
  std::vector<std::byte> wire =
      proto::serialize(sample_header(), std::vector<std::byte>(8, std::byte{1}));
  wire.resize(wire.size() + 4, std::byte{2});

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kPayloadLengthMismatch);
}

void test_rejects_unsupported_version() {
  std::vector<std::byte> wire = proto::serialize(sample_header(), {});
  wire[4] = std::byte{99};

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kUnsupportedVersion);
}

void test_rejects_unknown_packet_type() {
  std::vector<std::byte> wire = proto::serialize(sample_header(), {});
  wire[5] = std::byte{200};

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kUnknownPacketType);
}

void test_rejects_bad_header_length() {
  std::vector<std::byte> wire = proto::serialize(sample_header(), {});
  wire[6] = std::byte{0x00};
  wire[7] = std::byte{0x30};  // claims 48

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  CHECK(result.error() == proto::DecodeError::kBadHeaderLength);
}

void test_failed_result_does_not_expose_a_half_built_packet() {
  std::vector<std::byte> wire = proto::serialize(sample_header(), bytes_of("hi"));
  wire[0] = std::byte{0x00};

  const proto::DecodeResult result = proto::deserialize(wire);
  CHECK(!result.ok());
  // The fields parsed before the failure must not leak out: a failed decode
  // yields a default-constructed packet, not a partially populated one.
  CHECK_EQ(result.value().header.session_id, 0ULL);
  CHECK(result.value().payload.empty());
}

// ---------------------------------------------------------------------------

struct TestCase {
  const char* name;
  void (*run)();
};

constexpr TestCase kTests[] = {
    {"round_trip_preserves_all_fields", test_round_trip_preserves_all_fields},
    {"round_trip_with_empty_payload", test_round_trip_with_empty_payload},
    {"round_trip_at_maximum_payload", test_round_trip_at_maximum_payload},
    {"oversized_payload_is_refused", test_oversized_payload_is_refused},
    {"wire_layout_is_big_endian", test_wire_layout_is_big_endian},
    {"header_struct_size_matches_wire_size",
     test_header_struct_size_matches_wire_size},
    {"rejects_bad_magic", test_rejects_bad_magic},
    {"rejects_buffer_shorter_than_header",
     test_rejects_buffer_shorter_than_header},
    {"rejects_truncated_payload", test_rejects_truncated_payload},
    {"rejects_payload_longer_than_declared",
     test_rejects_payload_longer_than_declared},
    {"rejects_unsupported_version", test_rejects_unsupported_version},
    {"rejects_unknown_packet_type", test_rejects_unknown_packet_type},
    {"rejects_bad_header_length", test_rejects_bad_header_length},
    {"failed_result_does_not_expose_a_half_built_packet",
     test_failed_result_does_not_expose_a_half_built_packet},
};

}  // namespace

int main() {
  int failed_cases = 0;

  for (const TestCase& test : kTests) {
    const int failures_before = g_failures;
    test.run();
    const bool passed = (g_failures == failures_before);
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
