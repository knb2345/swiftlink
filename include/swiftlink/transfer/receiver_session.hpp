// One receiving side of one file transfer, as a pure state machine.
//
// handle_packet() takes a decoded packet and returns what should be sent back.
// It performs no socket I/O of its own, which is what lets the same class be
// driven by milestone 3's blocking loop and milestone 5's epoll loop without
// changing a line of the reliability logic. Only the driver changes.

#pragma once

#include <cstdint>
#include <string>

#include "swiftlink/io/file.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/transfer/transfer.hpp"
#include "swiftlink/transfer/window.hpp"

namespace swiftlink::transfer {

class ReceiverSession {
 public:
  // What the driver should transmit in response to a packet.
  struct Reply {
    bool send = false;
    protocol::PacketType type = protocol::PacketType::kAck;
    std::uint32_t sequence = 0;
  };

  explicit ReceiverSession(std::uint32_t window_capacity);

  // Opens the output file. Must succeed before handle_packet is called.
  [[nodiscard]] bool open(const std::string& output_path);

  [[nodiscard]] Reply handle_packet(const protocol::Packet& packet);

  [[nodiscard]] bool finished() const noexcept { return finished_; }
  [[nodiscard]] TransferError error() const noexcept { return error_; }
  [[nodiscard]] const TransferStats& stats() const noexcept { return stats_; }
  [[nodiscard]] TransferStats& stats() noexcept { return stats_; }

  // Flushes to disk. Called once the transfer is finished.
  [[nodiscard]] bool finalize();

 private:
  io::File file_;
  ReceiverWindow window_;
  std::uint64_t bytes_written_ = 0;
  bool finished_ = false;
  TransferError error_ = TransferError::kNone;
  TransferStats stats_;
};

}  // namespace swiftlink::transfer
