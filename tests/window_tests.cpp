// Unit tests for the sliding-window bookkeeping.
//
// SenderWindow and ReceiverWindow deliberately touch no sockets and no files,
// so every case here is a pure function of sequence numbers and clock values.
// That is the point of having split them out: window advance, ACK handling,
// duplicate handling and timeout detection are the parts most likely to be
// subtly wrong, and they can all be driven directly instead of being inferred
// from a transfer that happened to succeed.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "swiftlink/transfer/window.hpp"

namespace xfer = swiftlink::transfer;
using xfer::Clock;
using xfer::TimePoint;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* file, int line,
           std::string_view expression, std::string_view detail = {}) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::cerr << file << ":" << line << ": FAILED: " << expression;
    if (!detail.empty()) {
      std::cerr << "\n    " << detail;
    }
    std::cerr << '\n';
  }
}

// Takes its arguments by value through a function rather than expanding them
// twice in a macro. That matters here because expire() has side effects: a
// macro that mentioned `a` in both the comparison and the failure message
// would call it twice and consume the timers it was meant to observe.
template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const char* file, int line,
              std::string_view expression) {
  check(actual == expected, file, line, expression,
        "expected: " + std::to_string(expected) +
            "  actual: " + std::to_string(actual));
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b) check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)

// A fixed origin, so deadlines are exact rather than racing the real clock.
const TimePoint kT0 = Clock::now();

[[nodiscard]] TimePoint at(int milliseconds) {
  return kT0 + std::chrono::milliseconds{milliseconds};
}

// ---------------------------------------------------------------------------
// SenderWindow: filling and base advance
// ---------------------------------------------------------------------------

void test_sender_starts_empty() {
  xfer::SenderWindow window(4);
  CHECK_EQ(window.base(), 0U);
  CHECK_EQ(window.next_sequence(), 0U);
  CHECK_EQ(window.in_flight(), 0U);
  CHECK(window.can_send());
  CHECK(window.empty());
  CHECK(!window.earliest_deadline().has_value());
}

void test_sender_fills_to_capacity_then_blocks() {
  xfer::SenderWindow window(4);
  for (int i = 0; i < 4; ++i) {
    CHECK(window.can_send());
    window.on_sent(at(100));
  }
  CHECK(!window.can_send());
  CHECK_EQ(window.in_flight(), 4U);
  CHECK_EQ(window.next_sequence(), 4U);
  CHECK_EQ(window.base(), 0U);
}

void test_in_order_acks_advance_base_one_at_a_time() {
  xfer::SenderWindow window(4);
  for (int i = 0; i < 4; ++i) {
    window.on_sent(at(100));
  }

  CHECK(window.on_ack(0));
  CHECK_EQ(window.base(), 1U);
  CHECK(window.can_send());  // one slot freed

  CHECK(window.on_ack(1));
  CHECK_EQ(window.base(), 2U);
  CHECK_EQ(window.in_flight(), 2U);
}

void test_hole_at_base_stalls_the_window() {
  // The defining behaviour of a sliding window: acknowledging everything
  // *except* the base does not let the base move. This is what makes a single
  // lost packet cap throughput no matter how large the window is.
  xfer::SenderWindow window(4);
  for (int i = 0; i < 4; ++i) {
    window.on_sent(at(100));
  }

  CHECK(window.on_ack(1));
  CHECK(window.on_ack(2));
  CHECK(window.on_ack(3));
  CHECK_EQ(window.base(), 0U);      // still stuck on the missing packet 0
  CHECK_EQ(window.in_flight(), 4U); // so no new packet may be sent
  CHECK(!window.can_send());

  // Filling the hole releases the whole run at once.
  CHECK(window.on_ack(0));
  CHECK_EQ(window.base(), 4U);
  CHECK_EQ(window.in_flight(), 0U);
  CHECK(window.empty());
}

// ---------------------------------------------------------------------------
// SenderWindow: ACK handling
// ---------------------------------------------------------------------------

void test_duplicate_ack_is_rejected_and_moves_nothing() {
  xfer::SenderWindow window(4);
  window.on_sent(at(100));
  window.on_sent(at(100));

  CHECK(window.on_ack(0));
  CHECK_EQ(window.base(), 1U);

  // The same ACK again: reported as not-newly-retiring, and the base must not
  // drift. A window that advanced on duplicates could be walked forward by a
  // peer replaying one ACK.
  CHECK(!window.on_ack(0));
  CHECK_EQ(window.base(), 1U);
  CHECK(!window.on_ack(0));
  CHECK_EQ(window.base(), 1U);
}

void test_ack_below_base_is_rejected() {
  xfer::SenderWindow window(4);
  window.on_sent(at(100));
  window.on_sent(at(100));
  CHECK(window.on_ack(0));
  CHECK(window.on_ack(1));
  CHECK_EQ(window.base(), 2U);

  CHECK(!window.on_ack(0));
  CHECK(!window.on_ack(1));
  CHECK_EQ(window.base(), 2U);
}

void test_ack_for_unsent_packet_is_rejected() {
  // An ACK for something never sent must not move the base. Otherwise a peer
  // could acknowledge a far-future sequence number and skip the sender past
  // data it never transmitted.
  xfer::SenderWindow window(4);
  window.on_sent(at(100));

  CHECK(!window.on_ack(1));
  CHECK(!window.on_ack(99));
  CHECK(!window.on_ack(0xFFFFFFFFU));
  CHECK_EQ(window.base(), 0U);
  CHECK_EQ(window.in_flight(), 1U);
}

// ---------------------------------------------------------------------------
// SenderWindow: timers
// ---------------------------------------------------------------------------

void test_earliest_deadline_is_the_minimum() {
  xfer::SenderWindow window(4);
  window.on_sent(at(300));
  window.on_sent(at(100));  // earlier
  window.on_sent(at(200));

  const auto earliest = window.earliest_deadline();
  CHECK(earliest.has_value());
  CHECK(*earliest == at(100));
}

void test_expire_returns_only_packets_past_their_deadline() {
  xfer::SenderWindow window(4);
  window.on_sent(at(100));
  window.on_sent(at(200));
  window.on_sent(at(300));

  const std::vector<std::uint32_t> due = window.expire(at(250));
  CHECK_EQ(due.size(), 2U);
  if (due.size() == 2) {
    CHECK_EQ(due[0], 0U);
    CHECK_EQ(due[1], 1U);
  }

  // Packet 2 is not due yet and remains the next deadline.
  const auto earliest = window.earliest_deadline();
  CHECK(earliest.has_value());
  CHECK(*earliest == at(300));
}

void test_expire_does_not_return_the_same_packet_twice() {
  // Each expire() disarms what it hands back, so a caller that has not yet
  // rescheduled cannot be told about the same packet again.
  xfer::SenderWindow window(4);
  window.on_sent(at(100));

  CHECK_EQ(window.expire(at(150)).size(), 1U);
  CHECK_EQ(window.expire(at(150)).size(), 0U);
  CHECK_EQ(window.expire(at(999)).size(), 0U);
}

void test_acknowledged_packets_never_expire() {
  // This is the lazy-deletion path. Packet 0's heap entry is still physically
  // in the heap after the ACK; it must be recognised as a ghost and dropped
  // rather than causing a pointless retransmission.
  xfer::SenderWindow window(4);
  window.on_sent(at(100));
  window.on_sent(at(200));

  CHECK(window.on_ack(0));

  const std::vector<std::uint32_t> due = window.expire(at(500));
  CHECK_EQ(due.size(), 1U);
  if (due.size() == 1) {
    CHECK_EQ(due[0], 1U);  // only the unacknowledged one
  }
}

void test_reschedule_rearms_a_timer() {
  xfer::SenderWindow window(4);
  window.on_sent(at(100));

  CHECK_EQ(window.expire(at(150)).size(), 1U);
  CHECK(!window.earliest_deadline().has_value());  // disarmed

  window.reschedule(0, at(400));
  const auto earliest = window.earliest_deadline();
  CHECK(earliest.has_value());
  CHECK(*earliest == at(400));

  CHECK_EQ(window.expire(at(500)).size(), 1U);
}

void test_reschedule_of_an_acked_packet_is_ignored() {
  // Race that really happens: the timer fires, and while the caller is
  // retransmitting, the ACK arrives. Re-arming afterwards would leave a timer
  // on a packet that is already retired.
  xfer::SenderWindow window(4);
  window.on_sent(at(100));
  CHECK_EQ(window.expire(at(150)).size(), 1U);

  CHECK(window.on_ack(0));
  window.reschedule(0, at(400));

  CHECK(!window.earliest_deadline().has_value());
  CHECK_EQ(window.expire(at(999)).size(), 0U);
}

void test_repeated_timeouts_do_not_grow_the_live_timer_set() {
  // Retransmitting the same packet many times pushes many heap entries, but
  // only the newest is live. Exercises the generation counter under churn.
  xfer::SenderWindow window(2);
  window.on_sent(at(0));

  for (int i = 1; i <= 20; ++i) {
    const std::vector<std::uint32_t> due = window.expire(at(i * 10));
    CHECK_EQ(due.size(), 1U);
    window.reschedule(0, at(i * 10 + 10));
  }

  CHECK(window.on_ack(0));
  CHECK_EQ(window.expire(at(100000)).size(), 0U);
  CHECK(window.empty());
}

void test_slot_reuse_after_base_advances() {
  // With capacity 2, sequence 2 reuses sequence 0's ring slot. If the slot were
  // not cleaned on advance, the new packet would inherit stale state.
  xfer::SenderWindow window(2);
  window.on_sent(at(100));
  window.on_sent(at(100));
  CHECK(window.on_ack(0));
  CHECK(window.on_ack(1));
  CHECK_EQ(window.base(), 2U);

  CHECK(window.can_send());
  window.on_sent(at(200));  // sequence 2, slot 0
  CHECK_EQ(window.in_flight(), 1U);

  const std::vector<std::uint32_t> due = window.expire(at(300));
  CHECK_EQ(due.size(), 1U);
  if (due.size() == 1) {
    CHECK_EQ(due[0], 2U);
  }
}

// ---------------------------------------------------------------------------
// ReceiverWindow
// ---------------------------------------------------------------------------

void test_receiver_accepts_in_order() {
  xfer::ReceiverWindow window(8);
  for (std::uint32_t i = 0; i < 5; ++i) {
    CHECK(window.classify(i) == xfer::ReceiverWindow::Disposition::kAccepted);
    window.mark_received(i);
    CHECK_EQ(window.base(), i + 1);
  }
}

void test_receiver_accepts_out_of_order_and_advances_on_fill() {
  xfer::ReceiverWindow window(8);

  // 2 and 1 arrive before 0. The base cannot move yet.
  window.mark_received(2);
  CHECK_EQ(window.base(), 0U);
  window.mark_received(1);
  CHECK_EQ(window.base(), 0U);

  // 0 arrives and the whole contiguous run is released in one step.
  window.mark_received(0);
  CHECK_EQ(window.base(), 3U);
}

void test_receiver_reports_duplicates() {
  xfer::ReceiverWindow window(8);
  window.mark_received(0);
  window.mark_received(1);

  // Below the base: certainly already stored.
  CHECK(window.classify(0) == xfer::ReceiverWindow::Disposition::kDuplicate);
  CHECK(window.classify(1) == xfer::ReceiverWindow::Disposition::kDuplicate);

  // Inside the window but already marked.
  window.mark_received(4);
  CHECK(window.classify(4) == xfer::ReceiverWindow::Disposition::kDuplicate);
  CHECK(window.classify(3) == xfer::ReceiverWindow::Disposition::kAccepted);
}

void test_receiver_marking_a_duplicate_does_not_move_the_base() {
  xfer::ReceiverWindow window(8);
  window.mark_received(0);
  CHECK_EQ(window.base(), 1U);

  window.mark_received(0);  // replay
  CHECK_EQ(window.base(), 1U);
}

void test_receiver_rejects_beyond_the_window() {
  xfer::ReceiverWindow window(4);
  CHECK(window.classify(3) == xfer::ReceiverWindow::Disposition::kAccepted);
  // base is 0 and capacity 4, so 4 and beyond would alias onto a live slot.
  CHECK(window.classify(4) == xfer::ReceiverWindow::Disposition::kOutsideWindow);
  CHECK(window.classify(100) ==
        xfer::ReceiverWindow::Disposition::kOutsideWindow);

  // Once the base moves, the far edge moves with it.
  window.mark_received(0);
  CHECK_EQ(window.base(), 1U);
  CHECK(window.classify(4) == xfer::ReceiverWindow::Disposition::kAccepted);
}

void test_receiver_ring_is_clean_after_wraparound() {
  // Slots are reused as the base advances; a slot left marked would make a
  // later, genuinely new packet look like a duplicate.
  xfer::ReceiverWindow window(4);
  for (std::uint32_t i = 0; i < 12; ++i) {
    CHECK(window.classify(i) == xfer::ReceiverWindow::Disposition::kAccepted);
    window.mark_received(i);
  }
  CHECK_EQ(window.base(), 12U);
}

// ---------------------------------------------------------------------------

struct TestCase {
  const char* name;
  void (*run)();
};

constexpr TestCase kTests[] = {
    {"sender_starts_empty", test_sender_starts_empty},
    {"sender_fills_to_capacity_then_blocks",
     test_sender_fills_to_capacity_then_blocks},
    {"in_order_acks_advance_base_one_at_a_time",
     test_in_order_acks_advance_base_one_at_a_time},
    {"hole_at_base_stalls_the_window", test_hole_at_base_stalls_the_window},
    {"duplicate_ack_is_rejected_and_moves_nothing",
     test_duplicate_ack_is_rejected_and_moves_nothing},
    {"ack_below_base_is_rejected", test_ack_below_base_is_rejected},
    {"ack_for_unsent_packet_is_rejected",
     test_ack_for_unsent_packet_is_rejected},
    {"earliest_deadline_is_the_minimum", test_earliest_deadline_is_the_minimum},
    {"expire_returns_only_packets_past_their_deadline",
     test_expire_returns_only_packets_past_their_deadline},
    {"expire_does_not_return_the_same_packet_twice",
     test_expire_does_not_return_the_same_packet_twice},
    {"acknowledged_packets_never_expire",
     test_acknowledged_packets_never_expire},
    {"reschedule_rearms_a_timer", test_reschedule_rearms_a_timer},
    {"reschedule_of_an_acked_packet_is_ignored",
     test_reschedule_of_an_acked_packet_is_ignored},
    {"repeated_timeouts_do_not_grow_the_live_timer_set",
     test_repeated_timeouts_do_not_grow_the_live_timer_set},
    {"slot_reuse_after_base_advances", test_slot_reuse_after_base_advances},
    {"receiver_accepts_in_order", test_receiver_accepts_in_order},
    {"receiver_accepts_out_of_order_and_advances_on_fill",
     test_receiver_accepts_out_of_order_and_advances_on_fill},
    {"receiver_reports_duplicates", test_receiver_reports_duplicates},
    {"receiver_marking_a_duplicate_does_not_move_the_base",
     test_receiver_marking_a_duplicate_does_not_move_the_base},
    {"receiver_rejects_beyond_the_window",
     test_receiver_rejects_beyond_the_window},
    {"receiver_ring_is_clean_after_wraparound",
     test_receiver_ring_is_clean_after_wraparound},
};

}  // namespace

int main() {
  int failed_cases = 0;
  for (const TestCase& test : kTests) {
    const int before = g_failures;
    test.run();
    const bool passed = (g_failures == before);
    if (!passed) {
      ++failed_cases;
    }
    std::cout << (passed ? "[  PASS  ] " : "[  FAIL  ] ") << test.name << '\n';
  }

  std::cout << "\n"
            << (sizeof(kTests) / sizeof(kTests[0])) << " cases, " << g_checks
            << " checks, " << failed_cases << " failed cases, " << g_failures
            << " failed checks\n";
  return failed_cases == 0 ? 0 : 1;
}
