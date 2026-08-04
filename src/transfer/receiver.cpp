#include "swiftlink/transfer/receiver.hpp"

#include <cerrno>
#include <chrono>
#include <vector>

#include "swiftlink/io/file.hpp"
#include "swiftlink/protocol/packet.hpp"

namespace swiftlink::transfer {
namespace {

namespace proto = swiftlink::protocol;
using Clock = std::chrono::steady_clock;

// Builds and sends a bare ACK for `sequence` back to `peer`.
//
// The acknowledgement is unconditional: the receiver ACKs every DATA packet it
// accepts *and* every duplicate it discards. Silently dropping a duplicate
// without re-acknowledging it would deadlock the transfer, because the reason
// the sender resent it is that the first ACK never arrived -- staying quiet
// would just make it time out again.
[[nodiscard]] bool send_ack(net::UdpSocket& socket, const net::Endpoint& peer,
                            std::uint64_t session_id, std::uint32_t sequence) {
  proto::PacketHeader ack;
  ack.packet_type = proto::PacketType::kAck;
  ack.session_id = session_id;
  ack.sequence_number = sequence;
  ack.acknowledgement_number = sequence;

  const std::vector<std::byte> wire = proto::serialize(ack, {});
  return socket.send_to(wire, peer) >= 0;
}

}  // namespace

TransferError receive_file(net::UdpSocket& socket,
                           const std::string& output_path,
                           const ReceiverConfig& config, TransferStats& stats) {
  io::File file;
  if (!file.open_write(output_path)) {
    return TransferError::kFileOpenFailed;
  }

  if (!socket.set_receive_timeout(
          std::chrono::duration_cast<std::chrono::microseconds>(
              config.idle_timeout))) {
    return TransferError::kSocketFailed;
  }

  std::vector<std::byte> buffer(proto::kHeaderWireSize + proto::kMaxPayloadSize);

  // The next sequence number we have not yet written. Under stop-and-wait this
  // advances by exactly one per accepted chunk; milestone 3 replaces it with a
  // window base.
  std::uint32_t expected = 0;
  std::uint64_t bytes_written = 0;

  bool timing_started = false;
  Clock::time_point started{};

  for (;;) {
    net::Endpoint peer;
    const std::ptrdiff_t received = socket.recv_from(buffer, peer);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return TransferError::kPeerUnresponsive;
      }
      return TransferError::kReceiveFailed;
    }

    if (!timing_started) {
      // Start the clock at the first packet, not at bind time, so the number
      // measures the transfer rather than how long the server sat idle.
      started = Clock::now();
      timing_started = true;
    }

    ++stats.packets_received;

    const auto result = proto::deserialize(
        std::span<const std::byte>{buffer.data(),
                                   static_cast<std::size_t>(received)});
    if (!result.ok()) {
      continue;  // not ours, or corrupt: ignore it
    }

    const proto::Packet& packet = result.value();
    const proto::PacketHeader& header = packet.header;

    if (header.packet_type == proto::PacketType::kFin) {
      if (!send_ack(socket, peer, header.session_id, header.sequence_number)) {
        return TransferError::kSendFailed;
      }
      if (header.byte_offset != bytes_written) {
        // The sender's declared total disagrees with what we actually wrote,
        // so the file on disk is not the file that was sent.
        return TransferError::kSizeMismatch;
      }
      break;
    }

    if (header.packet_type != proto::PacketType::kData) {
      continue;  // milestone 2 speaks only DATA, ACK and FIN
    }

    if (header.sequence_number < expected) {
      // Already written. pwrite would make rewriting it harmless, but there is
      // no reason to touch the disk again -- just re-acknowledge.
      ++stats.duplicates_received;
      if (!send_ack(socket, peer, header.session_id, header.sequence_number)) {
        return TransferError::kSendFailed;
      }
      continue;
    }

    if (header.sequence_number > expected) {
      // Cannot happen with a single packet in flight over a non-reordering
      // path, and there is nowhere to buffer it until milestone 3, so drop it
      // without acknowledging. The sender will retransmit what we actually
      // want once its timer fires.
      ++stats.out_of_order_received;
      continue;
    }

    const std::ptrdiff_t written =
        file.write_at(packet.payload, header.byte_offset);
    if (written < 0 ||
        static_cast<std::size_t>(written) != packet.payload.size()) {
      return TransferError::kFileWriteFailed;
    }

    bytes_written += packet.payload.size();
    ++expected;
    ++stats.chunks;
    stats.bytes_transferred = bytes_written;

    if (!send_ack(socket, peer, header.session_id, header.sequence_number)) {
      return TransferError::kSendFailed;
    }
  }

  if (!file.sync()) {
    return TransferError::kFileWriteFailed;
  }

  stats.elapsed_seconds =
      std::chrono::duration<double>(Clock::now() - started).count();
  return TransferError::kNone;
}

}  // namespace swiftlink::transfer
