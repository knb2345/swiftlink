// Unit tests for the UDP socket wrapper.
//
// Mostly this class is a thin shell over the syscalls, and thin shells do not
// usually earn tests. The receive timeout does, because SO_RCVTIMEO has an
// edge that is silent and fatal: a zero timeval means "block forever", the
// opposite of what a caller asking for zero wants. A sender computing
// `deadline - now` reaches zero routinely under load, and the resulting hang
// looks exactly like a stalled network rather than like a bug.
//
// That is not hypothetical here. It hung a real transfer during the impairment
// matrix -- the sender sat in recvfrom() indefinitely with a complete window
// of packets owed a retransmission, while the harness waited out its timeout.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include "swiftlink/net/udp_socket.hpp"

namespace net = swiftlink::net;
using Clock = std::chrono::steady_clock;

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

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)

// Returns how long a receive on an empty socket actually took. Anything that
// blocks is caught by the caller comparing against a bound, rather than by the
// test suite hanging with no explanation.
[[nodiscard]] std::chrono::milliseconds time_a_receive(
    net::UdpSocket& socket) {
  std::vector<std::byte> buffer(64);
  net::Endpoint from;
  const Clock::time_point started = Clock::now();
  const std::ptrdiff_t received = socket.recv_from(buffer, from);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - started);
  (void)received;
  return elapsed;
}

void test_zero_timeout_does_not_block_forever() {
  // The regression guard. Under SO_RCVTIMEO a {0, 0} timeval disables the
  // timeout entirely, so this must be clamped rather than passed through.
  net::UdpSocket socket;
  CHECK(socket.open());
  CHECK(socket.bind(0));
  CHECK(socket.set_receive_timeout(std::chrono::microseconds{0}));

  const auto elapsed = time_a_receive(socket);
  CHECK(elapsed < std::chrono::milliseconds{500});
  CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
}

void test_negative_timeout_does_not_block_forever() {
  // `deadline - now` goes negative as easily as it hits zero.
  net::UdpSocket socket;
  CHECK(socket.open());
  CHECK(socket.bind(0));
  CHECK(socket.set_receive_timeout(std::chrono::microseconds{-1000}));

  const auto elapsed = time_a_receive(socket);
  CHECK(elapsed < std::chrono::milliseconds{500});
}

void test_a_real_timeout_is_honoured() {
  // The clamp must not have flattened every timeout to "return immediately":
  // a sender that never waits would spin instead of sleeping between
  // retransmissions.
  net::UdpSocket socket;
  CHECK(socket.open());
  CHECK(socket.bind(0));
  CHECK(socket.set_receive_timeout(std::chrono::milliseconds{150}));

  const auto elapsed = time_a_receive(socket);
  CHECK(elapsed >= std::chrono::milliseconds{100});
  CHECK(elapsed < std::chrono::milliseconds{2000});
}

void test_a_datagram_survives_the_round_trip() {
  net::UdpSocket receiver;
  CHECK(receiver.open());
  CHECK(receiver.bind(0));
  CHECK(receiver.set_receive_timeout(std::chrono::milliseconds{2000}));

  // bind(0) let the kernel choose, so ask it which port that was.
  net::Endpoint probe_from;
  std::uint16_t port = 0;
  {
    // Sending to ourselves needs the real port; recover it by sending from a
    // second socket to each candidate is unreasonable, so read it back.
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    CHECK(::getsockname(receiver.fd(),
                        reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port = ntohs(address.sin_port);
  }
  CHECK(port != 0);

  net::UdpSocket sender;
  CHECK(sender.open());

  const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xFF},
                                       std::byte{'S'}, std::byte{'L'}};
  CHECK(sender.send_to(payload, net::Endpoint{"127.0.0.1", port}) ==
        static_cast<std::ptrdiff_t>(payload.size()));

  std::vector<std::byte> buffer(64);
  const std::ptrdiff_t received = receiver.recv_from(buffer, probe_from);
  CHECK(received == static_cast<std::ptrdiff_t>(payload.size()));
  for (std::size_t i = 0; i < payload.size(); ++i) {
    CHECK(buffer[i] == payload[i]);
  }
}

void test_operations_on_a_closed_socket_fail_cleanly() {
  net::UdpSocket socket;
  CHECK(!socket.valid());
  CHECK(!socket.set_receive_timeout(std::chrono::milliseconds{10}));
  CHECK(errno == EBADF);
  CHECK(!socket.set_nonblocking(true));
  CHECK(errno == EBADF);
}

struct TestCase {
  const char* name;
  void (*run)();
};

constexpr TestCase kTests[] = {
    {"zero_timeout_does_not_block_forever",
     test_zero_timeout_does_not_block_forever},
    {"negative_timeout_does_not_block_forever",
     test_negative_timeout_does_not_block_forever},
    {"a_real_timeout_is_honoured", test_a_real_timeout_is_honoured},
    {"a_datagram_survives_the_round_trip",
     test_a_datagram_survives_the_round_trip},
    {"operations_on_a_closed_socket_fail_cleanly",
     test_operations_on_a_closed_socket_fail_cleanly},
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
