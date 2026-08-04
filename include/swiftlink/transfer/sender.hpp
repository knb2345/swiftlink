// Stop-and-wait file sender (milestone 2).
//
// The entire reliability mechanism is: send one DATA packet, wait for its ACK,
// retransmit on timeout, then move on. One packet is in flight at any instant,
// which makes throughput exactly one chunk per round trip regardless of how
// much bandwidth is available. That is a terrible way to move a file and it is
// meant to be -- it is the baseline the sliding window in milestone 3 is
// measured against.

#pragma once

#include <cstdint>
#include <string>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/transfer/transfer.hpp"

namespace swiftlink::transfer {

struct SenderConfig {
  std::size_t chunk_size = kDefaultChunkSize;
  std::chrono::milliseconds rto = kDefaultRto;
  int max_retries = kMaxRetriesPerChunk;

  // Packets outstanding at once. 1 reproduces milestone 2's stop-and-wait
  // behaviour exactly, which is what makes the benchmark comparison honest:
  // the same code path is measured at every window size.
  std::uint32_t window_size = kDefaultWindowSize;

  // Probability in [0, 1] that an outbound datagram is thrown away instead of
  // being handed to sendto. This models packet loss on the forward path
  // without needing a lossy network: the sender's state machine cannot tell
  // the difference, because from its point of view a packet it "sent" simply
  // never got acknowledged.
  double loss_probability = 0.0;

  // Seed for the loss generator, so a run can be reproduced exactly.
  std::uint64_t seed = 0;

  std::uint64_t session_id = 0;
};

// Sends `path` to `destination` over `socket`. The socket must already be
// open; the sender sets its own receive timeout on it.
[[nodiscard]] TransferError send_file(net::UdpSocket& socket,
                                      const net::Endpoint& destination,
                                      const std::string& path,
                                      const SenderConfig& config,
                                      TransferStats& stats);

}  // namespace swiftlink::transfer
