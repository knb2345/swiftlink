// Stop-and-wait file receiver (milestone 2).
//
// Accepts DATA packets, writes each chunk at its declared byte offset with
// pwrite, acknowledges every packet it accepts, and finishes when it sees a
// FIN. Handles duplicates, because a lost ACK makes the sender retransmit a
// chunk the receiver has already written.

#pragma once

#include <cstdint>
#include <string>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/transfer/transfer.hpp"

namespace swiftlink::transfer {

struct ReceiverConfig {
  // How long to wait for the next packet before concluding the peer is gone.
  std::chrono::milliseconds idle_timeout{10000};

  // Duplicate-detection window. Must be at least as large as the sender's.
  std::uint32_t window_size = kDefaultReceiverWindowSize;
};

// Receives one file into `output_directory`. `socket` must already be bound.
// The filename comes from the peer's START packet and is sanitised before use;
// the path actually written is reported in `out_path`.
[[nodiscard]] TransferError receive_file(net::UdpSocket& socket,
                                         const std::string& output_directory,
                                         const ReceiverConfig& config,
                                         TransferStats& stats,
                                         std::string& out_path);

}  // namespace swiftlink::transfer
