// Sliding-window bookkeeping for Selective Repeat.
//
// Deliberately free of sockets, files and packets: it tracks sequence numbers
// and deadlines and nothing else. That is what makes the window advance, ACK
// handling, duplicate handling and timeout detection directly unit-testable
// without moving a byte over a wire.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <queue>
#include <vector>

namespace swiftlink::transfer {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---------------------------------------------------------------------------
// SenderWindow
// ---------------------------------------------------------------------------
//
// Holds up to `capacity` unacknowledged packets and owns their retransmission
// timers.
//
// TIMER STRUCTURE
//
// The obvious implementation keeps a deadline per outstanding packet and, once
// per loop iteration, walks all of them looking for expiries. That is O(W) per
// iteration, and since the loop runs at least once per packet, O(N*W) for a
// transfer -- the cost of the timer scan grows with the very window size you
// increased to go faster.
//
// Instead the deadlines live in a binary min-heap. Finding the next deadline is
// O(1) (peek the root), and each schedule/expire is O(log W).
//
// The problem with a heap is that ACKs and retransmissions invalidate entries
// that are buried in the middle, and a binary heap cannot delete from the
// middle cheaply. The fix is *lazy deletion with generation counters*: every
// slot carries a generation, every heap entry records the generation it was
// created with, and cancelling simply bumps the slot's generation. A popped
// entry whose generation no longer matches its slot is a ghost and is
// discarded. Nothing is ever removed from the middle of the heap; stale
// entries are filtered on the way out.
//
// Each packet contributes at most one live entry plus one ghost per
// retransmission, so the heap stays O(W + retransmissions) and every operation
// stays logarithmic.
class SenderWindow {
 public:
  explicit SenderWindow(std::uint32_t capacity);

  // True while the window has room for another packet.
  [[nodiscard]] bool can_send() const noexcept {
    return (next_sequence_ - base_) < capacity_;
  }

  // Records that `sequence` (which must be next_sequence()) has gone out, and
  // arms its retransmission timer.
  void on_sent(TimePoint deadline);

  // Applies an ACK. Returns true if this ACK retired a packet that was still
  // outstanding; false if it was a duplicate, or referred to a packet outside
  // the window (both of which are normal and must not disturb the window).
  [[nodiscard]] bool on_ack(std::uint32_t sequence);

  // Earliest live deadline, or nullopt when nothing is outstanding. Prunes
  // ghost entries off the top of the heap as a side effect, which is why it is
  // not const.
  [[nodiscard]] std::optional<TimePoint> earliest_deadline();

  // Sequence numbers whose timers have expired at `now`. Each returned packet
  // has had its timer disarmed; the caller retransmits it and calls
  // reschedule() to arm a fresh one.
  [[nodiscard]] std::vector<std::uint32_t> expire(TimePoint now);

  // Re-arms the timer for a packet that has just been retransmitted.
  void reschedule(std::uint32_t sequence, TimePoint deadline);

  [[nodiscard]] std::uint32_t base() const noexcept { return base_; }
  [[nodiscard]] std::uint32_t next_sequence() const noexcept {
    return next_sequence_;
  }
  [[nodiscard]] std::uint32_t in_flight() const noexcept {
    return next_sequence_ - base_;
  }
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return base_ == next_sequence_; }

 private:
  struct Slot {
    bool outstanding = false;
    std::uint64_t generation = 0;
  };

  struct Entry {
    TimePoint deadline;
    std::uint32_t sequence;
    std::uint64_t generation;

    // std::priority_queue is a max-heap; invert so the earliest deadline is on
    // top.
    bool operator<(const Entry& other) const noexcept {
      return deadline > other.deadline;
    }
  };

  // True if this heap entry still describes a live timer.
  [[nodiscard]] bool is_live(const Entry& entry) const noexcept;

  // Discards ghost entries sitting on top of the heap.
  void prune();

  [[nodiscard]] Slot& slot_for(std::uint32_t sequence) noexcept {
    return slots_[sequence % capacity_];
  }

  std::uint32_t capacity_;
  std::uint32_t base_ = 0;
  std::uint32_t next_sequence_ = 0;
  std::vector<Slot> slots_;
  std::priority_queue<Entry> timers_;
};

// ---------------------------------------------------------------------------
// ReceiverWindow
// ---------------------------------------------------------------------------
//
// Classifies incoming sequence numbers. Note that it does not buffer payloads:
// because chunks are written with pwrite at their own byte offset, an
// out-of-order chunk can go straight to disk at the right place. The window
// only has to remember *which* sequence numbers have already been stored, so a
// retransmission is recognised as a duplicate instead of being counted twice.
class ReceiverWindow {
 public:
  enum class Disposition : std::uint8_t {
    kAccepted,       // new, inside the window: store it and acknowledge
    kDuplicate,      // already stored, or below the base: acknowledge, do not store
    kOutsideWindow,  // too far ahead to track: ignore entirely
  };

  explicit ReceiverWindow(std::uint32_t capacity);

  [[nodiscard]] Disposition classify(std::uint32_t sequence) const noexcept;

  // Records `sequence` as stored and advances the base over any newly
  // contiguous prefix. Only valid for a sequence that classify() called
  // kAccepted.
  void mark_received(std::uint32_t sequence);

  [[nodiscard]] std::uint32_t base() const noexcept { return base_; }
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

 private:
  std::uint32_t capacity_;
  std::uint32_t base_ = 0;
  std::vector<bool> received_;  // ring indexed by sequence % capacity
};

}  // namespace swiftlink::transfer
