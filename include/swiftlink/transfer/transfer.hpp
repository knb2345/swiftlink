// Types shared by the sending and receiving halves of a file transfer.

#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "swiftlink/protocol/status.hpp"

namespace swiftlink::transfer {

// 1200 bytes of payload, so a full packet is 1240 bytes on the wire (40-byte
// header). That sits comfortably under the 1500-byte Ethernet MTU even after
// 20 bytes of IPv4 header and 8 of UDP, and leaves headroom for a VPN or PPPoE
// encapsulation. Staying under the path MTU matters because an IP-fragmented
// datagram is lost in its entirety if any one fragment is lost, which would
// multiply the effective loss rate for no benefit.
//
// Loopback has a 65536-byte MTU so nothing here is forced to fragment during
// local benchmarking; the limit is chosen for the real-network case.
inline constexpr std::size_t kDefaultChunkSize = 1200;

// Fixed retransmission timeout for milestone 2. A real implementation derives
// this from measured RTT (Jacobson/Karels); a fixed value is deliberately
// simple so the baseline has one fewer moving part, and its inadequacy is
// visible in the benchmark numbers.
inline constexpr std::chrono::milliseconds kDefaultRto{300};

// Give up on a chunk after this many consecutive timeouts, so a dead peer ends
// the transfer instead of hanging forever.
inline constexpr int kMaxRetriesPerChunk = 50;

// Packets the sender may have outstanding at once. 32 x 1200 bytes = 38.4 KB
// in flight, which fills a 12 Mbps path at a 25 ms RTT.
inline constexpr std::uint32_t kDefaultWindowSize = 32;

// The receiver's window is deliberately much larger than the sender's default.
// Its only job is to recognise duplicates, and a sequence number beyond
// base + capacity has to be discarded because marking it would alias onto a
// live slot in the ring. Sizing it generously means a client configured with a
// larger window than the server still interoperates, at a cost of one bit per
// slot. The sender's window must not exceed this.
inline constexpr std::uint32_t kDefaultReceiverWindowSize = 1024;

enum class TransferError : std::uint8_t {
  kNone = 0,
  kFileOpenFailed,
  kFileReadFailed,
  kFileWriteFailed,
  kSocketFailed,
  kSendFailed,
  kReceiveFailed,
  kPeerUnresponsive,   // exceeded kMaxRetriesPerChunk
  kProtocolViolation,  // peer sent something the state machine cannot accept
  kSizeMismatch,       // bytes received disagree with what the sender declared
  kHandshakeFailed,    // no START_ACK before the retry budget ran out
  kRemoteError,        // peer sent an ERROR packet; see TransferStats::remote_status
  kIntegrityFailed,    // SHA-256 of the received file did not match
  kInvalidFilename,    // the advertised name failed sanitisation
};

[[nodiscard]] std::string_view to_string(TransferError error) noexcept;

// Counters collected during a transfer. Everything here is measured, not
// derived, so the benchmark numbers come straight out of the implementation.
struct TransferStats {
  std::uint64_t bytes_transferred = 0;
  std::uint64_t chunks = 0;

  // Every datagram handed to sendto, retransmissions included. Simulated
  // losses are counted as "sent" from the protocol's point of view because the
  // sender believes it sent them -- that is the whole point of the simulation.
  std::uint64_t packets_sent = 0;
  std::uint64_t retransmissions = 0;
  std::uint64_t simulated_drops = 0;

  std::uint64_t packets_received = 0;
  std::uint64_t duplicates_received = 0;
  std::uint64_t out_of_order_received = 0;
  std::uint64_t timeouts = 0;

  double elapsed_seconds = 0.0;

  // Set when the peer replied with an ERROR packet, so the caller can report
  // *why* the far end gave up rather than just that it did.
  protocol::StatusCode remote_status = protocol::StatusCode::kOk;

  // Megabits per second, decimal (10^6), which is the convention for network
  // throughput. Not mebibits.
  [[nodiscard]] double throughput_mbps() const noexcept {
    if (elapsed_seconds <= 0.0) {
      return 0.0;
    }
    return (static_cast<double>(bytes_transferred) * 8.0) /
           elapsed_seconds / 1e6;
  }
};

}  // namespace swiftlink::transfer
