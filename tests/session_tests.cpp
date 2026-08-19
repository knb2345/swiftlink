// Unit tests for the receiving side of a transfer, as a state machine.
//
// ReceiverSession does no socket I/O: it takes a decoded packet and returns
// what should be sent back. That makes the parts hardest to reach from an
// end-to-end run -- a retransmitted FIN, a rejected filename, a late DATA --
// directly drivable, instead of having to be provoked by a lossy network and
// then inferred from whether the transfer happened to succeed.
//
// The cases around completion exist because of a real defect. The server used
// to erase a session the moment it was finished, so a FIN_ACK lost on the way
// back left the sender retransmitting a FIN that nobody would answer; it then
// reported failure for a file that was complete and byte-identical on disk.
// Reproduced at 9 failures in 20 runs with 30% loss on the reverse path.

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "swiftlink/crypto/sha256.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/protocol/status.hpp"
#include "swiftlink/transfer/receiver_session.hpp"

namespace proto = swiftlink::protocol;
namespace xfer = swiftlink::transfer;
namespace crypto = swiftlink::crypto;

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

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* file, int line,
              std::string_view expression) {
  check(actual == expected, file, line, expression,
        "expected: " + std::to_string(expected) +
            "  actual: " + std::to_string(actual));
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b) check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)

// A directory that removes itself, so a failing case cannot leave files behind
// for the next one to trip over.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "swiftlink_session_XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* made = ::mkdtemp(buffer.data());
    path_ = (made != nullptr) ? made : "";
  }
  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

constexpr std::uint64_t kSession = 0xABCDEF0123456789ULL;

[[nodiscard]] proto::Packet make(proto::PacketType type, std::uint32_t sequence,
                                 std::uint64_t offset,
                                 std::vector<std::byte> payload,
                                 std::uint64_t session = kSession) {
  proto::Packet packet;
  packet.header.packet_type = type;
  packet.header.session_id = session;
  packet.header.sequence_number = sequence;
  packet.header.byte_offset = offset;
  packet.payload = std::move(payload);
  return packet;
}

[[nodiscard]] proto::Packet start_packet(std::string_view name,
                                         std::uint64_t size,
                                         std::uint64_t session = kSession) {
  return make(proto::PacketType::kStart, 0, size, bytes_of(name), session);
}

[[nodiscard]] std::vector<std::byte> digest_of(std::string_view content) {
  crypto::Sha256 hasher;
  const std::vector<std::byte> raw = bytes_of(content);
  hasher.update(std::span<const std::byte>{raw.data(), raw.size()});
  const crypto::Sha256Digest digest = hasher.finish();
  return std::vector<std::byte>(digest.begin(), digest.end());
}

// Drives a session all the way to a successful FIN_ACK, which is the starting
// point for every case about what happens *after* completion.
void transfer_one_chunk(xfer::ReceiverSession& session, std::string_view name,
                        std::string_view content) {
  const auto size = static_cast<std::uint64_t>(content.size());
  (void)session.handle_packet(start_packet(name, size));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of(content)));
  (void)session.handle_packet(
      make(proto::PacketType::kFin, 1, size, digest_of(content)));
}

// ---------------------------------------------------------------------------
// The happy path, so the cases below start from a known-good state
// ---------------------------------------------------------------------------

void test_start_data_fin_completes() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  const std::string_view content = "swiftlink";

  const auto start = session.handle_packet(start_packet("f.bin", 9));
  CHECK(start.send);
  CHECK(start.type == proto::PacketType::kStartAck);
  CHECK(session.started());
  CHECK(!session.finished());

  const auto data = session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of(content)));
  CHECK(data.send);
  CHECK(data.type == proto::PacketType::kAck);
  CHECK_EQ(data.sequence, 0U);

  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of(content)));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kFinAck);
  CHECK(session.finished());
  CHECK(session.error() == xfer::TransferError::kNone);
}

// ---------------------------------------------------------------------------
// Completion: the session must outlive its own last packet
// ---------------------------------------------------------------------------

void test_retransmitted_fin_is_answered_again() {
  // The regression guard. A sender whose FIN_ACK was dropped retransmits the
  // FIN; answering it is the only way it can ever learn the transfer worked.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  transfer_one_chunk(session, "f.bin", "swiftlink");

  const auto again = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
  CHECK(again.send);
  CHECK(again.type == proto::PacketType::kFinAck);
  CHECK_EQ(again.sequence, 1U);
}

void test_repeated_fins_are_all_answered() {
  // Not just the second one: the reverse path can drop several in a row.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  transfer_one_chunk(session, "f.bin", "swiftlink");

  for (int i = 0; i < 5; ++i) {
    const auto again = session.handle_packet(
        make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
    CHECK(again.send);
    CHECK(again.type == proto::PacketType::kFinAck);
  }
}

void test_replayed_fin_does_not_reread_the_file() {
  // The verdict is stored, not recomputed. Deleting the output file after
  // completion proves it: a session that re-hashed on every FIN would fail to
  // read it and answer ERROR instead.
  //
  // This is what stops a peer from making the server redo O(filesize) work,
  // inside its single event loop, just by repeating one packet.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  transfer_one_chunk(session, "f.bin", "swiftlink");
  session.close_file();

  std::error_code ignored;
  std::filesystem::remove(session.output_path(), ignored);
  CHECK(!std::filesystem::exists(session.output_path()));

  const auto again = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
  CHECK(again.send);
  CHECK(again.type == proto::PacketType::kFinAck);
}

void test_replay_requires_the_matching_session_id() {
  // A stray FIN from somebody else's transfer must not be answered, or the
  // session id would stop being the thing that separates two peers.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  transfer_one_chunk(session, "f.bin", "swiftlink");

  const auto stranger = session.handle_packet(make(
      proto::PacketType::kFin, 1, 9, digest_of("swiftlink"), kSession ^ 1ULL));
  CHECK(!stranger.send);
}

void test_late_data_after_completion_is_ignored() {
  // The file is closed by now. A DATA packet that arrives late has nowhere to
  // go, and answering it would invite the sender to keep talking.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  transfer_one_chunk(session, "f.bin", "swiftlink");

  const auto late = session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));
  CHECK(!late.send);
}

void test_error_is_replayed_for_a_retransmitted_start() {
  // A session that dies at START settles on an ERROR. The sender retries the
  // same START, so it must keep getting the same answer rather than silence.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());

  // A name with shell metacharacters is refused outright, unlike "../x" which
  // is contained by taking the basename.
  const auto first = session.handle_packet(start_packet("rm -rf $HOME", 9));
  CHECK(first.send);
  CHECK(first.type == proto::PacketType::kError);
  CHECK(session.finished());

  const auto again = session.handle_packet(start_packet("rm -rf $HOME", 9));
  CHECK(again.send);
  CHECK(again.type == proto::PacketType::kError);
  CHECK(!again.payload.empty());
  CHECK_EQ(static_cast<std::uint8_t>(again.payload[0]),
           static_cast<std::uint8_t>(proto::StatusCode::kInvalidFilename));
}

// ---------------------------------------------------------------------------
// Before completion
// ---------------------------------------------------------------------------

void test_retransmitted_start_does_not_truncate() {
  // Re-opening with O_TRUNC would throw away everything received so far, so a
  // duplicate START has to be re-acknowledged instead of re-run.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  const auto again = session.handle_packet(start_packet("f.bin", 9));
  CHECK(again.send);
  CHECK(again.type == proto::PacketType::kStartAck);

  // The bytes survived: the FIN over the original digest still verifies.
  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
  CHECK(fin.type == proto::PacketType::kFinAck);
}

void test_duplicate_data_is_reacknowledged_not_rewritten() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  const auto again = session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("SWIFTLINK")));
  CHECK(again.send);
  CHECK(again.type == proto::PacketType::kAck);

  // The rewrite must not have happened, so the original digest still matches.
  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
  CHECK(fin.type == proto::PacketType::kFinAck);
}

void test_data_before_start_is_refused() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  const auto data = session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("x")));
  CHECK(data.send);
  CHECK(data.type == proto::PacketType::kError);
  CHECK(session.finished());
}

void test_data_with_a_foreign_session_id_is_refused() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));

  const auto data = session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("x"), kSession ^ 1ULL));
  CHECK(data.send);
  CHECK(data.type == proto::PacketType::kError);
}

void test_out_of_order_chunks_land_at_their_offsets() {
  // The whole reason the receiver needs no reassembly buffer: each chunk
  // carries its own offset, so arriving backwards changes nothing about where
  // the bytes end up. The digest is over the assembled file, so it can only
  // match if that held.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  const std::string_view content = "abcdefghij";
  (void)session.handle_packet(start_packet("f.bin", content.size()));

  for (int sequence = 4; sequence >= 0; --sequence) {
    const auto offset = static_cast<std::uint64_t>(sequence) * 2;
    (void)session.handle_packet(make(proto::PacketType::kData,
                                     static_cast<std::uint32_t>(sequence),
                                     offset,
                                     bytes_of(content.substr(offset, 2))));
  }

  const auto fin = session.handle_packet(make(
      proto::PacketType::kFin, 5, content.size(), digest_of(content)));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kFinAck);
}

// ---------------------------------------------------------------------------
// FIN rejections
// ---------------------------------------------------------------------------

void test_fin_with_a_wrong_digest_fails_integrity() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("SWIFTLINK")));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kError);
  CHECK(session.error() == xfer::TransferError::kIntegrityFailed);
  CHECK_EQ(static_cast<std::uint8_t>(fin.payload[0]),
           static_cast<std::uint8_t>(proto::StatusCode::kIntegrityCheckFailed));
}

void test_fin_with_a_short_digest_fails_integrity() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, bytes_of("short")));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kError);
  CHECK(session.error() == xfer::TransferError::kIntegrityFailed);
}

void test_fin_short_of_the_declared_size_fails() {
  // Arriving early is not completion. Without this the receiver would hash a
  // truncated file and, with a matching digest, call it a success.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 20));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  const auto fin = session.handle_packet(
      make(proto::PacketType::kFin, 1, 20, digest_of("swiftlink")));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kError);
  CHECK(session.error() == xfer::TransferError::kSizeMismatch);
}

void test_empty_file_completes() {
  // Zero chunks, straight from START to FIN. The digest of nothing is still a
  // digest, and the size check has to agree that 0 == 0.
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("empty.bin", 0));

  const auto fin =
      session.handle_packet(make(proto::PacketType::kFin, 0, 0, digest_of("")));
  CHECK(fin.send);
  CHECK(fin.type == proto::PacketType::kFinAck);
  CHECK(session.error() == xfer::TransferError::kNone);
}

// ---------------------------------------------------------------------------
// The output directory only ever holds complete, verified files
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t count_entries(const std::string& dir) {
  std::size_t n = 0;
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ignored)) {
    (void)entry;
    ++n;
  }
  return n;
}

void test_an_abandoned_transfer_leaves_nothing_behind() {
  // The session is destroyed mid-transfer, which is what happens when a client
  // vanishes and the idle sweep reaps it. Nothing it wrote may survive: a
  // truncated file sitting under the name the client asked for is worse than
  // no file, because nothing about it says it is incomplete.
  TempDir dir;
  {
    xfer::ReceiverSession session(16, dir.path());
    (void)session.handle_packet(start_packet("f.bin", 9));
    (void)session.handle_packet(
        make(proto::PacketType::kData, 0, 0, bytes_of("swift")));
    CHECK(!std::filesystem::exists(dir.path() + "/f.bin"));
  }
  CHECK_EQ(count_entries(dir.path()), std::size_t{0});
}

void test_a_failed_integrity_check_leaves_nothing_behind() {
  TempDir dir;
  {
    xfer::ReceiverSession session(16, dir.path());
    (void)session.handle_packet(start_packet("f.bin", 9));
    (void)session.handle_packet(
        make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));
    const auto fin = session.handle_packet(
        make(proto::PacketType::kFin, 1, 9, digest_of("SWIFTLINK")));
    CHECK(fin.type == proto::PacketType::kError);
  }
  CHECK_EQ(count_entries(dir.path()), std::size_t{0});
}

void test_the_file_appears_only_once_it_is_verified() {
  TempDir dir;
  xfer::ReceiverSession session(16, dir.path());
  (void)session.handle_packet(start_packet("f.bin", 9));
  (void)session.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of("swiftlink")));

  // All nine bytes are on disk, but under a private name, not this one.
  CHECK(!std::filesystem::exists(dir.path() + "/f.bin"));

  (void)session.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of("swiftlink")));
  CHECK(std::filesystem::exists(dir.path() + "/f.bin"));
  CHECK_EQ(count_entries(dir.path()), std::size_t{1});
}

void test_two_sessions_claiming_one_name_do_not_corrupt_each_other() {
  // Two clients, same advertised filename, interleaved writes. They used to
  // share a file descriptor's worth of destiny and shred each other's bytes.
  // Each now writes to a name derived from its own session id, so both
  // transfers verify; the shared name is simply claimed by whoever finishes
  // last.
  TempDir dir;
  xfer::ReceiverSession first(16, dir.path());
  xfer::ReceiverSession second(16, dir.path());

  constexpr std::uint64_t kOther = kSession ^ 0xFFFFULL;
  const std::string_view a = "aaaaaaaaa";
  const std::string_view b = "bbbbbbbbb";

  (void)first.handle_packet(start_packet("same.bin", 9));
  (void)second.handle_packet(start_packet("same.bin", 9, kOther));

  (void)first.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of(a)));
  (void)second.handle_packet(
      make(proto::PacketType::kData, 0, 0, bytes_of(b), kOther));

  // Each verifies against its own bytes, which is only possible if the writes
  // never met.
  const auto fin_a =
      first.handle_packet(make(proto::PacketType::kFin, 1, 9, digest_of(a)));
  const auto fin_b = second.handle_packet(
      make(proto::PacketType::kFin, 1, 9, digest_of(b), kOther));
  CHECK(fin_a.type == proto::PacketType::kFinAck);
  CHECK(fin_b.type == proto::PacketType::kFinAck);

  // One name, one file, and it is whole. Which of the two won is last-writer-
  // wins and is not asserted here -- see docs/todo.md.
  CHECK_EQ(count_entries(dir.path()), std::size_t{1});
}

// ---------------------------------------------------------------------------
// The 32-bit sequence-number cap
// ---------------------------------------------------------------------------

void test_chunk_count_covers_the_ordinary_cases() {
  CHECK(xfer::chunk_count(0, 1200) == std::optional<std::uint32_t>{0});
  CHECK(xfer::chunk_count(1, 1200) == std::optional<std::uint32_t>{1});
  CHECK(xfer::chunk_count(1199, 1200) == std::optional<std::uint32_t>{1});
  CHECK(xfer::chunk_count(1200, 1200) == std::optional<std::uint32_t>{1});
  CHECK(xfer::chunk_count(1201, 1200) == std::optional<std::uint32_t>{2});
  CHECK(xfer::chunk_count(2400, 1200) == std::optional<std::uint32_t>{2});
  CHECK(xfer::chunk_count(2401, 1200) == std::optional<std::uint32_t>{3});
}

void test_chunk_count_refuses_what_a_32_bit_sequence_cannot_address() {
  // Exactly 2^32 chunks is the last addressable count, since sequence numbers
  // run 0..2^32-1. One chunk more must be refused rather than wrapped: aliasing
  // chunk 2^32 onto chunk 0 would corrupt the file silently, which is worse
  // than refusing the transfer.
  //
  // Reaching this from send_file() would take a ~5 TB file. Here it costs a
  // multiplication.
  constexpr std::uint64_t kLimit = 0xFFFFFFFFULL;
  constexpr std::size_t kChunk = 1200;

  CHECK(xfer::chunk_count(kLimit * kChunk, kChunk) ==
        std::optional<std::uint32_t>{0xFFFFFFFFU});
  CHECK(!xfer::chunk_count(kLimit * kChunk + 1, kChunk).has_value());
  CHECK(!xfer::chunk_count(UINT64_MAX / 2, kChunk).has_value());

  // A larger chunk moves the boundary rather than removing it.
  CHECK(xfer::chunk_count(kLimit * 65535ULL, 65535).has_value());
  CHECK(!xfer::chunk_count(kLimit * 65535ULL + 1, 65535).has_value());

  // A one-byte chunk caps the transfer at 4 GiB, which is the cheapest way to
  // see that the limit is on the chunk count and not on the byte offset.
  CHECK(xfer::chunk_count(kLimit, 1) == std::optional<std::uint32_t>{0xFFFFFFFFU});
  CHECK(!xfer::chunk_count(kLimit + 1, 1).has_value());
}

void test_chunk_count_refuses_a_zero_chunk_size() {
  // Would divide by zero rather than merely being wrong.
  CHECK(!xfer::chunk_count(1000, 0).has_value());
}

struct TestCase {
  const char* name;
  void (*run)();
};

constexpr TestCase kTests[] = {
    {"start_data_fin_completes", test_start_data_fin_completes},
    {"retransmitted_fin_is_answered_again",
     test_retransmitted_fin_is_answered_again},
    {"repeated_fins_are_all_answered", test_repeated_fins_are_all_answered},
    {"replayed_fin_does_not_reread_the_file",
     test_replayed_fin_does_not_reread_the_file},
    {"replay_requires_the_matching_session_id",
     test_replay_requires_the_matching_session_id},
    {"late_data_after_completion_is_ignored",
     test_late_data_after_completion_is_ignored},
    {"error_is_replayed_for_a_retransmitted_start",
     test_error_is_replayed_for_a_retransmitted_start},
    {"retransmitted_start_does_not_truncate",
     test_retransmitted_start_does_not_truncate},
    {"duplicate_data_is_reacknowledged_not_rewritten",
     test_duplicate_data_is_reacknowledged_not_rewritten},
    {"data_before_start_is_refused", test_data_before_start_is_refused},
    {"data_with_a_foreign_session_id_is_refused",
     test_data_with_a_foreign_session_id_is_refused},
    {"out_of_order_chunks_land_at_their_offsets",
     test_out_of_order_chunks_land_at_their_offsets},
    {"fin_with_a_wrong_digest_fails_integrity",
     test_fin_with_a_wrong_digest_fails_integrity},
    {"fin_with_a_short_digest_fails_integrity",
     test_fin_with_a_short_digest_fails_integrity},
    {"fin_short_of_the_declared_size_fails",
     test_fin_short_of_the_declared_size_fails},
    {"empty_file_completes", test_empty_file_completes},
    {"an_abandoned_transfer_leaves_nothing_behind",
     test_an_abandoned_transfer_leaves_nothing_behind},
    {"a_failed_integrity_check_leaves_nothing_behind",
     test_a_failed_integrity_check_leaves_nothing_behind},
    {"the_file_appears_only_once_it_is_verified",
     test_the_file_appears_only_once_it_is_verified},
    {"two_sessions_claiming_one_name_do_not_corrupt_each_other",
     test_two_sessions_claiming_one_name_do_not_corrupt_each_other},
    {"chunk_count_covers_the_ordinary_cases",
     test_chunk_count_covers_the_ordinary_cases},
    {"chunk_count_refuses_what_a_32_bit_sequence_cannot_address",
     test_chunk_count_refuses_what_a_32_bit_sequence_cannot_address},
    {"chunk_count_refuses_a_zero_chunk_size",
     test_chunk_count_refuses_a_zero_chunk_size},
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
