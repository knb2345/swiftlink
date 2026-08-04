#include "swiftlink/transfer/receiver_session.hpp"

namespace swiftlink::transfer {

namespace proto = swiftlink::protocol;

ReceiverSession::ReceiverSession(std::uint32_t window_capacity)
    : window_(window_capacity) {}

bool ReceiverSession::open(const std::string& output_path) {
  if (!file_.open_write(output_path)) {
    error_ = TransferError::kFileOpenFailed;
    return false;
  }
  return true;
}

ReceiverSession::Reply ReceiverSession::handle_packet(
    const proto::Packet& packet) {
  const proto::PacketHeader& header = packet.header;
  ++stats_.packets_received;

  if (header.packet_type == proto::PacketType::kFin) {
    // The sender only sends FIN once every chunk has been acknowledged, so by
    // the time it arrives everything is on disk. The size check is a
    // cross-check on that invariant rather than a routine condition.
    if (header.byte_offset != bytes_written_) {
      error_ = TransferError::kSizeMismatch;
    }
    finished_ = true;
    return Reply{true, proto::PacketType::kAck, header.sequence_number};
  }

  if (header.packet_type != proto::PacketType::kData) {
    return Reply{};  // milestone 3 speaks DATA, ACK and FIN only
  }

  switch (window_.classify(header.sequence_number)) {
    case ReceiverWindow::Disposition::kDuplicate:
      // Already on disk. Acknowledge again -- the sender resent it precisely
      // because it never saw our first ACK, so silence would deadlock it.
      ++stats_.duplicates_received;
      return Reply{true, proto::PacketType::kAck, header.sequence_number};

    case ReceiverWindow::Disposition::kOutsideWindow:
      // Too far ahead for the ring to represent. A sender obeying the agreed
      // window cannot produce this, so drop it without acknowledging.
      ++stats_.out_of_order_received;
      return Reply{};

    case ReceiverWindow::Disposition::kAccepted:
      break;
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
    error_ = TransferError::kFileWriteFailed;
    finished_ = true;
    return Reply{};
  }

  window_.mark_received(header.sequence_number);
  bytes_written_ += packet.payload.size();
  ++stats_.chunks;
  stats_.bytes_transferred = bytes_written_;

  return Reply{true, proto::PacketType::kAck, header.sequence_number};
}

bool ReceiverSession::finalize() {
  if (!file_.sync()) {
    error_ = TransferError::kFileWriteFailed;
    return false;
  }
  return true;
}

}  // namespace swiftlink::transfer
