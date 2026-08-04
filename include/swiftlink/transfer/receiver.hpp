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
};

// Receives one file into `output_path`. `socket` must already be bound.
[[nodiscard]] TransferError receive_file(net::UdpSocket& socket,
                                         const std::string& output_path,
                                         const ReceiverConfig& config,
                                         TransferStats& stats);

}  // namespace swiftlink::transfer
