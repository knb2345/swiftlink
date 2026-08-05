#include "swiftlink/transfer/receiver_session.hpp"

#include <string_view>

#include "swiftlink/crypto/sha256.hpp"
#include "swiftlink/transfer/filename.hpp"

namespace swiftlink::transfer {

namespace proto = swiftlink::protocol;
namespace crypto = swiftlink::crypto;

ReceiverSession::ReceiverSession(std::uint32_t window_capacity,
                                 std::string output_directory)
    : window_(window_capacity), output_directory_(std::move(output_directory)) {}

ReceiverSession::Reply ReceiverSession::fail(proto::StatusCode code,
                                             TransferError error,
                                             std::uint32_t sequence) {
  error_ = error;
  finished_ = true;

  // ERROR payload: one status byte, then a human-readable message for logs.
  const std::string_view message = proto::to_string(code);
  std::vector<std::byte> payload;
  payload.reserve(1 + message.size());
  payload.push_back(static_cast<std::byte>(code));
  for (const char c : message) {
    payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }

  return Reply{true, proto::PacketType::kError, sequence, std::move(payload)};
}

ReceiverSession::Reply ReceiverSession::on_start(const proto::Packet& packet) {
  const proto::PacketHeader& header = packet.header;

  if (started_) {
    // A retransmitted START, because our START_ACK was lost. Re-acknowledge
    // instead of restarting: reopening the file with O_TRUNC would throw away
    // everything received so far.
    if (header.session_id == session_id_) {
      ++stats_.duplicates_received;
      return Reply{true, proto::PacketType::kStartAck, header.sequence_number, {}};
    }
    return fail(proto::StatusCode::kUnknownSession,
                TransferError::kProtocolViolation, header.sequence_number);
  }

  // The filename is entirely attacker-controlled at this point.
  const std::string_view raw(
      reinterpret_cast<const char*>(packet.payload.data()),
      packet.payload.size());

  std::string safe_name;
  const FilenameRejection rejection = sanitize_filename(raw, safe_name);
  if (rejection != FilenameRejection::kNone) {
    return fail(proto::StatusCode::kInvalidFilename,
                TransferError::kInvalidFilename, header.sequence_number);
  }

  output_path_ = join_path(output_directory_, safe_name);
  if (!file_.open_write(output_path_)) {
    return fail(proto::StatusCode::kFileOpenFailed,
                TransferError::kFileOpenFailed, header.sequence_number);
  }

  session_id_ = header.session_id;
  declared_size_ = header.byte_offset;
  started_ = true;

  return Reply{true, proto::PacketType::kStartAck, header.sequence_number, {}};
}

ReceiverSession::Reply ReceiverSession::on_data(const proto::Packet& packet) {
  const proto::PacketHeader& header = packet.header;

  switch (window_.classify(header.sequence_number)) {
    case ReceiverWindow::Disposition::kDuplicate:
      // Already on disk. Acknowledge again -- the sender resent it precisely
      // because it never saw our first ACK, so silence would deadlock it.
      ++stats_.duplicates_received;
      return Reply{true, proto::PacketType::kAck, header.sequence_number, {}};

    case ReceiverWindow::Disposition::kOutsideWindow:
      // Too far ahead for the ring to represent. A sender obeying the agreed
      // window cannot produce this, so drop it without acknowledging.
      ++stats_.out_of_order_received;
      return Reply{};

    case ReceiverWindow::Disposition::kAccepted:
      break;
  }

  // Refuse a chunk that would write past the size declared at START. Without
  // this, a peer could grow the file arbitrarily after the fact.
  if (header.byte_offset + packet.payload.size() > declared_size_) {
    return fail(proto::StatusCode::kSizeMismatch, TransferError::kSizeMismatch,
                header.sequence_number);
  }

  // Out-of-order arrivals need no buffering: the chunk carries its own byte
  // offset, so pwrite puts it exactly where it belongs whether or not the
  // chunks before it have arrived. This is the payoff from choosing positional
  // I/O back in milestone 2.
  if (header.sequence_number != window_.base()) {
    ++stats_.out_of_order_received;
  }

  const std::ptrdiff_t written =
      file_.write_at(packet.payload, header.byte_offset);
  if (written < 0 ||
      static_cast<std::size_t>(written) != packet.payload.size()) {
    return fail(proto::StatusCode::kInternalError,
                TransferError::kFileWriteFailed, header.sequence_number);
  }

  window_.mark_received(header.sequence_number);
  bytes_written_ += packet.payload.size();
  ++stats_.chunks;
  stats_.bytes_transferred = bytes_written_;

  return Reply{true, proto::PacketType::kAck, header.sequence_number, {}};
}

ReceiverSession::Reply ReceiverSession::on_fin(const proto::Packet& packet) {
  const proto::PacketHeader& header = packet.header;

  if (header.byte_offset != bytes_written_) {
    return fail(proto::StatusCode::kSizeMismatch, TransferError::kSizeMismatch,
                header.sequence_number);
  }

  // Flush before hashing: the digest must cover what is actually on disk, not
  // what is sitting in the page cache waiting to be written.
  if (!file_.sync()) {
    return fail(proto::StatusCode::kInternalError,
                TransferError::kFileWriteFailed, header.sequence_number);
  }

  // Re-read and hash what was written.
  //
  // Streaming the hash as chunks arrive is impossible here: chunks can arrive
  // out of order, and SHA-256 is sequential. Reading the file back costs one
  // extra pass but verifies the bytes that genuinely landed on disk, which is
  // a stronger check than hashing the packets as they went by -- it would also
  // catch a chunk written to the wrong offset.
  crypto::Sha256 hasher;
  std::vector<std::byte> buffer(64 * 1024);
  std::uint64_t offset = 0;
  while (offset < bytes_written_) {
    const std::uint64_t remaining = bytes_written_ - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < buffer.size() ? remaining : buffer.size());
    const std::ptrdiff_t read =
        file_.read_at(std::span<std::byte>{buffer.data(), want}, offset);
    if (read < 0 || static_cast<std::size_t>(read) != want) {
      return fail(proto::StatusCode::kInternalError,
                  TransferError::kFileWriteFailed, header.sequence_number);
    }
    hasher.update(std::span<const std::byte>{buffer.data(), want});
    offset += want;
  }
  const crypto::Sha256Digest computed = hasher.finish();

  if (packet.payload.size() != crypto::kSha256DigestSize) {
    return fail(proto::StatusCode::kIntegrityCheckFailed,
                TransferError::kIntegrityFailed, header.sequence_number);
  }

  for (std::size_t i = 0; i < crypto::kSha256DigestSize; ++i) {
    if (computed[i] != packet.payload[i]) {
      return fail(proto::StatusCode::kIntegrityCheckFailed,
                  TransferError::kIntegrityFailed, header.sequence_number);
    }
  }

  finished_ = true;
  return Reply{true, proto::PacketType::kFinAck, header.sequence_number, {}};
}

ReceiverSession::Reply ReceiverSession::handle_packet(
    const proto::Packet& packet) {
  const proto::PacketHeader& header = packet.header;
  ++stats_.packets_received;

  if (header.packet_type == proto::PacketType::kStart) {
    return on_start(packet);
  }

  if (!started_) {
    // DATA or FIN before the handshake. Nothing is open, so there is nowhere
    // to put it.
    return fail(proto::StatusCode::kUnknownSession,
                TransferError::kProtocolViolation, header.sequence_number);
  }

  // Every later packet must carry the session id agreed at START. This is what
  // stops a stray or spoofed datagram from injecting bytes into somebody
  // else's file.
  if (header.session_id != session_id_) {
    return fail(proto::StatusCode::kUnknownSession,
                TransferError::kProtocolViolation, header.sequence_number);
  }

  if (header.packet_type == proto::PacketType::kData) {
    return on_data(packet);
  }
  if (header.packet_type == proto::PacketType::kFin) {
    return on_fin(packet);
  }

  return Reply{};  // ACK, START_ACK, FIN_ACK, ERROR: not ours to act on
}

}  // namespace swiftlink::transfer
