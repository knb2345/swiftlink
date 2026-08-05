// One receiving side of one file transfer, as a pure state machine.
//
// handle_packet() takes a decoded packet and returns what should be sent back.
// It performs no socket I/O of its own, which is what lets the same class be
// driven by milestone 3's blocking loop and milestone 5's epoll loop without
// changing a line of the reliability logic. Only the driver changes.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swiftlink/io/file.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/protocol/status.hpp"
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
    std::vector<std::byte> payload;  // ERROR packets carry a status + message
  };

  // `output_directory` is the only place this session may write. Filenames
  // from the peer are sanitised to a bare name and joined to it, so a session
  // cannot be talked into writing outside it.
  ReceiverSession(std::uint32_t window_capacity, std::string output_directory);

  [[nodiscard]] Reply handle_packet(const protocol::Packet& packet);

  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] bool finished() const noexcept { return finished_; }
  [[nodiscard]] TransferError error() const noexcept { return error_; }
  [[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }
  [[nodiscard]] const std::string& output_path() const noexcept {
    return output_path_;
  }
  [[nodiscard]] const TransferStats& stats() const noexcept { return stats_; }
  [[nodiscard]] TransferStats& stats() noexcept { return stats_; }

 private:
  [[nodiscard]] Reply on_start(const protocol::Packet& packet);
  [[nodiscard]] Reply on_data(const protocol::Packet& packet);
  [[nodiscard]] Reply on_fin(const protocol::Packet& packet);

  // Builds an ERROR reply and marks the session failed.
  [[nodiscard]] Reply fail(protocol::StatusCode code, TransferError error,
                           std::uint32_t sequence);

  io::File file_;
  ReceiverWindow window_;
  std::string output_directory_;
  std::string output_path_;

  std::uint64_t session_id_ = 0;
  std::uint64_t declared_size_ = 0;
  std::uint64_t bytes_written_ = 0;

  bool started_ = false;
  bool finished_ = false;
  TransferError error_ = TransferError::kNone;
  TransferStats stats_;
};

}  // namespace swiftlink::transfer
