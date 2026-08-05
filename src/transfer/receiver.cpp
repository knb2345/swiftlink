// Blocking driver for ReceiverSession (milestone 3).
//
// All the reliability logic lives in ReceiverSession; this file only moves
// bytes between the socket and that state machine. Milestone 5 replaces it
// with an epoll driver that runs many sessions at once, without touching the
// session logic itself.

#include "swiftlink/transfer/receiver.hpp"

#include <cerrno>
#include <chrono>
#include <vector>

#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/transfer/receiver_session.hpp"

namespace swiftlink::transfer {
namespace {

namespace proto = swiftlink::protocol;

[[nodiscard]] bool send_reply(net::UdpSocket& socket, const net::Endpoint& peer,
                             const ReceiverSession::Reply& reply,
                             std::uint64_t session_id) {
  proto::PacketHeader header;
  header.packet_type = reply.type;
  header.session_id = session_id;
  header.sequence_number = reply.sequence;
  header.acknowledgement_number = reply.sequence;

  const std::vector<std::byte> wire = proto::serialize(header, reply.payload);
  return socket.send_to(wire, peer) >= 0;
}

}  // namespace

TransferError receive_file(net::UdpSocket& socket,
                           const std::string& output_directory,
                           const ReceiverConfig& config, TransferStats& stats,
                           std::string& out_path) {
  ReceiverSession session(config.window_size, output_directory);

  if (!socket.set_receive_timeout(
          std::chrono::duration_cast<std::chrono::microseconds>(
              config.idle_timeout))) {
    return TransferError::kSocketFailed;
  }

  std::vector<std::byte> buffer(proto::kHeaderWireSize + proto::kMaxPayloadSize);

  bool timing_started = false;
  TimePoint started{};

  while (!session.finished()) {
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
      // Clock starts at the first packet, so the figure measures the transfer
      // rather than how long the server sat waiting for a client.
      started = Clock::now();
      timing_started = true;
    }

    const auto result = proto::deserialize(std::span<const std::byte>{
        buffer.data(), static_cast<std::size_t>(received)});
    if (!result.ok()) {
      continue;  // a UDP port accepts anything; ignore what we cannot parse
    }

    const proto::Packet& packet = result.value();
    const ReceiverSession::Reply reply = session.handle_packet(packet);

    if (reply.send &&
        !send_reply(socket, peer, reply, packet.header.session_id)) {
      return TransferError::kSendFailed;
    }
  }

  out_path = session.output_path();

  stats = session.stats();
  stats.elapsed_seconds =
      std::chrono::duration<double>(Clock::now() - started).count();

  if (session.error() != TransferError::kNone) {
    return session.error();
  }

  return TransferError::kNone;
}

}  // namespace swiftlink::transfer
