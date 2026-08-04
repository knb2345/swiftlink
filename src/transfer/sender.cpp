#include "swiftlink/transfer/sender.hpp"

#include <cerrno>
#include <chrono>
#include <random>
#include <vector>

#include "swiftlink/io/file.hpp"
#include "swiftlink/protocol/packet.hpp"

namespace swiftlink::transfer {
namespace {

namespace proto = swiftlink::protocol;
using Clock = std::chrono::steady_clock;

// Decides whether an outbound datagram is "lost". Kept in one place so the
// send path has a single branch and the counter cannot drift from reality.
class LossSimulator {
 public:
  LossSimulator(double probability, std::uint64_t seed)
      : probability_(probability), rng_(seed) {}

  [[nodiscard]] bool should_drop() {
    if (probability_ <= 0.0) {
      return false;
    }
    return distribution_(rng_) < probability_;
  }

 private:
  double probability_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> distribution_{0.0, 1.0};
};

// Sends one datagram, honouring the loss simulation. Returns false only for a
// real socket error -- a simulated drop is reported as success, because the
// sender is supposed to believe the packet went out.
[[nodiscard]] bool send_packet(net::UdpSocket& socket,
                               const net::Endpoint& destination,
                               const std::vector<std::byte>& wire,
                               LossSimulator& loss, TransferStats& stats) {
  ++stats.packets_sent;

  if (loss.should_drop()) {
    ++stats.simulated_drops;
    return true;
  }

  return socket.send_to(wire, destination) >= 0;
}

// Waits for an ACK whose acknowledgement_number is `expected`, until `deadline`.
//
// Returns:
//    1  the expected ACK arrived
//    0  the deadline passed (caller should retransmit)
//   -1  a socket error
//
// Anything else that arrives -- a stale ACK for an earlier chunk, a malformed
// datagram, a packet from an unrelated sender -- is counted and ignored, and
// the wait resumes with whatever time is left. Restarting the full timeout on
// every stray packet would let a chatty third party postpone a retransmission
// indefinitely.
[[nodiscard]] int await_ack(net::UdpSocket& socket, std::uint32_t expected,
                            Clock::time_point deadline, TransferStats& stats) {
  std::vector<std::byte> buffer(proto::kHeaderWireSize + proto::kMaxPayloadSize);

  for (;;) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      ++stats.timeouts;
      return 0;
    }

    if (!socket.set_receive_timeout(
            std::chrono::duration_cast<std::chrono::microseconds>(remaining))) {
      return -1;
    }

    net::Endpoint from;
    const std::ptrdiff_t received = socket.recv_from(buffer, from);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ++stats.timeouts;
        return 0;  // SO_RCVTIMEO fired: nothing arrived in time
      }
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }

    ++stats.packets_received;

    const auto result = proto::deserialize(
        std::span<const std::byte>{buffer.data(),
                                   static_cast<std::size_t>(received)});
    if (!result.ok()) {
      continue;  // garbage on the port; a UDP socket accepts anything
    }

    const proto::PacketHeader& header = result.value().header;
    if (header.packet_type != proto::PacketType::kAck) {
      continue;
    }

    if (header.acknowledgement_number == expected) {
      return 1;
    }

    // A duplicate or stale ACK. Under stop-and-wait this happens when an ACK we
    // already acted on was delayed rather than lost.
    ++stats.duplicates_received;
  }
}

// Sends one packet and blocks until it is acknowledged, retransmitting on each
// timeout. This is the whole of stop-and-wait, and both DATA and FIN use it.
[[nodiscard]] TransferError send_reliably(net::UdpSocket& socket,
                                          const net::Endpoint& destination,
                                          const std::vector<std::byte>& wire,
                                          std::uint32_t sequence,
                                          const SenderConfig& config,
                                          LossSimulator& loss,
                                          TransferStats& stats) {
  for (int attempt = 0; attempt <= config.max_retries; ++attempt) {
    if (attempt > 0) {
      ++stats.retransmissions;
    }

    if (!send_packet(socket, destination, wire, loss, stats)) {
      return TransferError::kSendFailed;
    }

    const Clock::time_point deadline = Clock::now() + config.rto;
    const int outcome = await_ack(socket, sequence, deadline, stats);
    if (outcome == 1) {
      return TransferError::kNone;
    }
    if (outcome < 0) {
      return TransferError::kReceiveFailed;
    }
    // outcome == 0: the RTO expired, so go round again and retransmit.
  }

  return TransferError::kPeerUnresponsive;
}

}  // namespace

TransferError send_file(net::UdpSocket& socket,
                        const net::Endpoint& destination,
                        const std::string& path, const SenderConfig& config,
                        TransferStats& stats) {
  io::File file;
  if (!file.open_read(path)) {
    return TransferError::kFileOpenFailed;
  }

  std::uint64_t file_size = 0;
  if (!file.size(file_size)) {
    return TransferError::kFileOpenFailed;
  }

  LossSimulator loss(config.loss_probability, config.seed);
  std::vector<std::byte> chunk(config.chunk_size);

  const Clock::time_point started = Clock::now();

  std::uint64_t offset = 0;
  std::uint32_t sequence = 0;

  while (offset < file_size) {
    const std::uint64_t remaining = file_size - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < config.chunk_size ? remaining : config.chunk_size);

    const std::ptrdiff_t read = file.read_at(
        std::span<std::byte>{chunk.data(), want}, offset);
    if (read < 0 || static_cast<std::size_t>(read) != want) {
      return TransferError::kFileReadFailed;
    }

    proto::PacketHeader header;
    header.packet_type = proto::PacketType::kData;
    header.session_id = config.session_id;
    header.sequence_number = sequence;
    header.byte_offset = offset;

    const std::vector<std::byte> wire = proto::serialize(
        header, std::span<const std::byte>{chunk.data(), want});

    const TransferError error = send_reliably(socket, destination, wire,
                                              sequence, config, loss, stats);
    if (error != TransferError::kNone) {
      return error;
    }

    offset += want;
    ++sequence;
    ++stats.chunks;
    stats.bytes_transferred = offset;
  }

  // FIN carries the total size in byte_offset so the receiver can confirm it
  // has every byte, and takes the sequence number one past the last chunk so
  // the same ACK-matching logic works unchanged.
  proto::PacketHeader fin;
  fin.packet_type = proto::PacketType::kFin;
  fin.session_id = config.session_id;
  fin.sequence_number = sequence;
  fin.byte_offset = file_size;

  const std::vector<std::byte> fin_wire = proto::serialize(fin, {});
  const TransferError error =
      send_reliably(socket, destination, fin_wire, sequence, config, loss, stats);
  if (error != TransferError::kNone) {
    return error;
  }

  stats.elapsed_seconds =
      std::chrono::duration<double>(Clock::now() - started).count();
  return TransferError::kNone;
}

}  // namespace swiftlink::transfer
