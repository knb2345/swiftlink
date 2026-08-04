#include "swiftlink/transfer/sender.hpp"

#include <cerrno>
#include <chrono>
#include <random>
#include <vector>

#include "swiftlink/io/file.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/transfer/window.hpp"

namespace swiftlink::transfer {
namespace {

namespace proto = swiftlink::protocol;

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

// One outstanding packet: the exact bytes to resend, plus how many times we
// have already tried.
struct InFlight {
  std::vector<std::byte> wire;
  int attempts = 0;
};

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

  const std::uint64_t total_chunks =
      (file_size + config.chunk_size - 1) / config.chunk_size;
  if (total_chunks > 0xFFFFFFFFULL) {
    // sequence_number is 32 bits; a file needing more chunks than that cannot
    // be addressed by this protocol version.
    return TransferError::kProtocolViolation;
  }

  LossSimulator loss(config.loss_probability, config.seed);
  SenderWindow window(config.window_size);
  std::vector<InFlight> in_flight(window.capacity());
  std::vector<std::byte> chunk(config.chunk_size);
  std::vector<std::byte> receive_buffer(proto::kHeaderWireSize +
                                        proto::kMaxPayloadSize);

  const TimePoint started = Clock::now();

  // ------------------------------------------------------------------
  // Main loop: keep the window full, then wait for either an ACK or the
  // earliest retransmission deadline, whichever comes first.
  // ------------------------------------------------------------------
  while (window.base() < total_chunks) {
    // 1. Fill the window. This is the entire difference from stop-and-wait:
    //    up to `capacity` packets are in flight instead of one, so the link
    //    carries `capacity` chunks per RTT rather than one.
    while (window.can_send() && window.next_sequence() < total_chunks) {
      const std::uint32_t sequence = window.next_sequence();
      const std::uint64_t offset =
          static_cast<std::uint64_t>(sequence) * config.chunk_size;
      const std::uint64_t remaining = file_size - offset;
      const std::size_t want = static_cast<std::size_t>(
          remaining < config.chunk_size ? remaining : config.chunk_size);

      const std::ptrdiff_t read =
          file.read_at(std::span<std::byte>{chunk.data(), want}, offset);
      if (read < 0 || static_cast<std::size_t>(read) != want) {
        return TransferError::kFileReadFailed;
      }

      proto::PacketHeader header;
      header.packet_type = proto::PacketType::kData;
      header.session_id = config.session_id;
      header.sequence_number = sequence;
      header.byte_offset = offset;

      InFlight& slot = in_flight[sequence % window.capacity()];
      slot.wire = proto::serialize(
          header, std::span<const std::byte>{chunk.data(), want});
      slot.attempts = 1;

      if (!send_packet(socket, destination, slot.wire, loss, stats)) {
        return TransferError::kSendFailed;
      }
      window.on_sent(Clock::now() + config.rto);
    }

    // 2. Sleep until the earliest deadline, or until an ACK wakes us.
    const std::optional<TimePoint> deadline = window.earliest_deadline();
    if (!deadline.has_value()) {
      break;  // nothing outstanding and nothing left to send
    }

    auto wait = *deadline - Clock::now();
    if (wait < Clock::duration::zero()) {
      wait = Clock::duration::zero();
    }
    if (!socket.set_receive_timeout(
            std::chrono::duration_cast<std::chrono::microseconds>(wait))) {
      return TransferError::kSocketFailed;
    }

    net::Endpoint from;
    const std::ptrdiff_t received = socket.recv_from(receive_buffer, from);
    if (received >= 0) {
      ++stats.packets_received;
      const auto result = proto::deserialize(std::span<const std::byte>{
          receive_buffer.data(), static_cast<std::size_t>(received)});
      if (result.ok() &&
          result.value().header.packet_type == proto::PacketType::kAck) {
        const std::uint32_t acked =
            result.value().header.acknowledgement_number;
        if (!window.on_ack(acked)) {
          // Duplicate or out-of-window ACK. Harmless, but counted: a burst of
          // them is the signature of ACKs being retransmitted needlessly.
          ++stats.duplicates_received;
        }
      }
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return TransferError::kReceiveFailed;
    }

    // 3. Retransmit whatever has timed out. Only the packets whose individual
    //    timers fired are resent -- that is what makes this Selective Repeat
    //    rather than Go-Back-N, which would resend the whole window.
    const TimePoint now = Clock::now();
    for (const std::uint32_t sequence : window.expire(now)) {
      InFlight& slot = in_flight[sequence % window.capacity()];
      if (slot.attempts >= config.max_retries) {
        return TransferError::kPeerUnresponsive;
      }
      ++slot.attempts;
      ++stats.retransmissions;
      ++stats.timeouts;

      if (!send_packet(socket, destination, slot.wire, loss, stats)) {
        return TransferError::kSendFailed;
      }
      window.reschedule(sequence, Clock::now() + config.rto);
    }
  }

  stats.chunks = total_chunks;
  stats.bytes_transferred = file_size;

  // ------------------------------------------------------------------
  // FIN, sent stop-and-wait: there is exactly one packet, so a window would
  // buy nothing.
  // ------------------------------------------------------------------
  proto::PacketHeader fin;
  fin.packet_type = proto::PacketType::kFin;
  fin.session_id = config.session_id;
  fin.sequence_number = static_cast<std::uint32_t>(total_chunks);
  fin.byte_offset = file_size;

  const std::vector<std::byte> fin_wire = proto::serialize(fin, {});

  for (int attempt = 0; attempt <= config.max_retries; ++attempt) {
    if (attempt > 0) {
      ++stats.retransmissions;
    }
    if (!send_packet(socket, destination, fin_wire, loss, stats)) {
      return TransferError::kSendFailed;
    }

    const TimePoint deadline = Clock::now() + config.rto;
    bool acknowledged = false;

    while (!acknowledged) {
      auto wait = deadline - Clock::now();
      if (wait <= Clock::duration::zero()) {
        ++stats.timeouts;
        break;
      }
      if (!socket.set_receive_timeout(
              std::chrono::duration_cast<std::chrono::microseconds>(wait))) {
        return TransferError::kSocketFailed;
      }

      net::Endpoint from;
      const std::ptrdiff_t received = socket.recv_from(receive_buffer, from);
      if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ++stats.timeouts;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        return TransferError::kReceiveFailed;
      }

      ++stats.packets_received;
      const auto result = proto::deserialize(std::span<const std::byte>{
          receive_buffer.data(), static_cast<std::size_t>(received)});
      if (result.ok() &&
          result.value().header.packet_type == proto::PacketType::kAck &&
          result.value().header.acknowledgement_number == fin.sequence_number) {
        acknowledged = true;
      }
    }

    if (acknowledged) {
      stats.elapsed_seconds =
          std::chrono::duration<double>(Clock::now() - started).count();
      return TransferError::kNone;
    }
  }

  return TransferError::kPeerUnresponsive;
}

}  // namespace swiftlink::transfer
