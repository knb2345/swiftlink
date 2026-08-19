// Concurrent SwiftLink server: one thread, one epoll loop, many sessions.
//
// WHY ONE THREAD
// --------------
// A transfer session is almost entirely I/O wait. Giving each one a thread
// would spend most of the memory and scheduler pressure on sleeping, and would
// force locking around anything shared. A single epoll loop handles all of them
// because the work per packet is small and bounded: decode, one pwrite, one
// sendto. There is no shared mutable state to protect because there is only one
// thread touching it.
//
// WHY epoll RATHER THAN select/poll
// ---------------------------------
// select and poll take the entire watch list on every call, so the kernel
// re-scans every descriptor each time: O(N) per wait, plus copying the set in
// and out. epoll registers interest once and returns only the descriptors that
// are actually ready, which is O(ready) rather than O(watched). With one UDP
// socket the difference is academic; the structure matters because the same
// loop also waits on a timerfd and a signalfd, and would extend to per-session
// descriptors without changing shape.
//
// EVERYTHING IS A FILE DESCRIPTOR
// -------------------------------
// Time and signals are both turned into descriptors -- timerfd and signalfd --
// so the loop has exactly one blocking point. The classic alternative, a signal
// handler setting a volatile flag, is subject to the async-signal-safety rules
// and races with the blocking call itself: a signal arriving just before
// epoll_wait would not interrupt it. Reading the signal as data removes both
// problems.

#pragma once

#include <cstdint>
#include <string>

#include "swiftlink/transfer/transfer.hpp"

namespace swiftlink::server {

struct ServerConfig {
  std::uint16_t port = 9000;
  std::string output_directory = ".";

  // A session with no packets for this long is abandoned and its partial file
  // closed. Without it, a client that vanishes mid-transfer leaks a session
  // entry and an open descriptor forever.
  std::chrono::milliseconds session_idle_timeout{30000};

  // How often the timerfd fires to sweep for idle sessions. Sweeping on a
  // timer rather than on every packet keeps the per-packet path free of
  // bookkeeping that is not needed at packet rate.
  std::chrono::milliseconds sweep_interval{1000};

  // How long a completed session is kept after its FIN_ACK has been sent.
  //
  // Erasing on completion loses the ability to answer a retransmitted FIN, and
  // then a sender whose FIN_ACK was dropped retries until its budget runs out
  // and reports failure for a file that arrived intact. The default outlives
  // that budget -- kMaxRetriesPerChunk x kDefaultRto is 15 s -- so the server
  // is still there for any sender that is still asking.
  std::chrono::milliseconds session_linger{20000};

  // Ceiling on simultaneous in-progress sessions.
  //
  // Only START may allocate, which stops the cheapest attack, but nothing else
  // stopped a flood of STARTs from one stranger from allocating without bound:
  // a measured 4000 STARTs produced 4000 sessions and 4000 files. This bounds
  // the damage. It is a mitigation and not the real fix, which is a stateless
  // SYN-cookie-style handshake -- see docs/todo.md.
  std::size_t max_sessions = 256;

  std::uint32_t window_size = transfer::kDefaultReceiverWindowSize;
  bool verbose = true;
};

// Runs until SIGINT or SIGTERM. Returns a process exit code.
[[nodiscard]] int run(const ServerConfig& config);

}  // namespace swiftlink::server
