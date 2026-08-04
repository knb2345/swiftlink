#include "swiftlink/transfer/window.hpp"

#include <cassert>

namespace swiftlink::transfer {

// ---------------------------------------------------------------------------
// SenderWindow
// ---------------------------------------------------------------------------

SenderWindow::SenderWindow(std::uint32_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity), slots_(capacity_) {}

bool SenderWindow::is_live(const Entry& entry) const noexcept {
  const Slot& slot = slots_[entry.sequence % capacity_];
  return slot.outstanding && slot.generation == entry.generation;
}

void SenderWindow::prune() {
  while (!timers_.empty() && !is_live(timers_.top())) {
    timers_.pop();
  }
}

void SenderWindow::on_sent(TimePoint deadline) {
  assert(can_send());

  const std::uint32_t sequence = next_sequence_;
  Slot& slot = slot_for(sequence);
  slot.outstanding = true;
  ++slot.generation;

  timers_.push(Entry{deadline, sequence, slot.generation});
  ++next_sequence_;
}

bool SenderWindow::on_ack(std::uint32_t sequence) {
  // Outside the window entirely: either an ACK for something already retired,
  // or for something never sent. Both are ignored rather than trusted -- a
  // peer (or an attacker) must not be able to move our base by acknowledging a
  // packet that does not exist.
  if (sequence < base_ || sequence >= next_sequence_) {
    return false;
  }

  Slot& slot = slot_for(sequence);
  if (!slot.outstanding) {
    return false;  // duplicate ACK for a packet already retired
  }

  slot.outstanding = false;
  // Bumping the generation orphans this packet's heap entry, so the timer will
  // be discarded when it surfaces instead of causing a pointless retransmit.
  ++slot.generation;

  // Advance the base over every contiguous acknowledged packet. This is the
  // one place the window actually slides, and it is why a hole at the base
  // stalls the sender no matter how many later packets are acknowledged.
  while (base_ < next_sequence_ && !slots_[base_ % capacity_].outstanding) {
    ++base_;
  }

  return true;
}

std::optional<TimePoint> SenderWindow::earliest_deadline() {
  prune();
  if (timers_.empty()) {
    return std::nullopt;
  }
  return timers_.top().deadline;
}

std::vector<std::uint32_t> SenderWindow::expire(TimePoint now) {
  std::vector<std::uint32_t> expired;

  for (;;) {
    prune();
    if (timers_.empty() || timers_.top().deadline > now) {
      break;
    }

    const Entry entry = timers_.top();
    timers_.pop();

    // Disarm by bumping the generation, so this packet has no live timer until
    // the caller reschedules it. Without this, a packet could be handed back
    // twice from a single expire() call.
    Slot& slot = slot_for(entry.sequence);
    ++slot.generation;

    expired.push_back(entry.sequence);
  }

  return expired;
}

void SenderWindow::reschedule(std::uint32_t sequence, TimePoint deadline) {
  if (sequence < base_ || sequence >= next_sequence_) {
    return;  // acknowledged while we were retransmitting it; nothing to arm
  }

  Slot& slot = slot_for(sequence);
  if (!slot.outstanding) {
    return;
  }

  ++slot.generation;
  timers_.push(Entry{deadline, sequence, slot.generation});
}

// ---------------------------------------------------------------------------
// ReceiverWindow
// ---------------------------------------------------------------------------

ReceiverWindow::ReceiverWindow(std::uint32_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity), received_(capacity_, false) {}

ReceiverWindow::Disposition ReceiverWindow::classify(
    std::uint32_t sequence) const noexcept {
  if (sequence < base_) {
    // Below the base: definitely already stored. The sender resent it because
    // our ACK went missing, so it must be acknowledged again.
    return Disposition::kDuplicate;
  }

  if (sequence >= base_ + capacity_) {
    // Beyond what the ring can represent. A sender obeying the window size
    // cannot produce this; marking it would alias onto a live slot and corrupt
    // the bookkeeping, so it is dropped.
    return Disposition::kOutsideWindow;
  }

  return received_[sequence % capacity_] ? Disposition::kDuplicate
                                         : Disposition::kAccepted;
}

void ReceiverWindow::mark_received(std::uint32_t sequence) {
  if (classify(sequence) != Disposition::kAccepted) {
    return;
  }

  received_[sequence % capacity_] = true;

  // Slide over the contiguous prefix, clearing each slot as it leaves the
  // window so the ring is clean when those indices are reused.
  while (received_[base_ % capacity_]) {
    received_[base_ % capacity_] = false;
    ++base_;
  }
}

}  // namespace swiftlink::transfer
