// A UDP relay that holds every datagram for a fixed one-way delay before
// forwarding it, giving a controllable artificial RTT.
//
// WHY THIS EXISTS
// ---------------
// The benchmark plan called for `tc qdisc add dev lo root netem delay 25ms`.
// The WSL2 kernel this project is developed on is built without netem:
//
//     $ zcat /proc/config.gz | grep NET_SCH_NETEM
//     # CONFIG_NET_SCH_NETEM is not set
//
// so netem is unavailable at any privilege level. This relay is the substitute.
// It is not equivalent to netem -- it adds a userspace scheduling hop, and it
// cannot reorder, duplicate, or corrupt packets -- but for the one property the
// benchmark needs (a known, stable round-trip time) it is a faithful stand-in,
// and the delay it produces is measured rather than assumed.
//
// TOPOLOGY
//
//     client  <--->  proxy :listen_port  <--->  server :target_port
//
// The client sends to the proxy instead of the server, and the proxy learns the
// client's address from the first datagram it receives. Each direction is
// delayed by `delay_ms`, so the observed RTT is 2 x delay_ms.
//
// Note this differs from what `netem delay 25ms` on lo would have produced.
// Loopback traffic traverses the qdisc in both directions, so that command
// yields roughly a 50ms RTT, not 25ms. This tool is configured by *one-way*
// delay so the resulting RTT is stated explicitly rather than inferred.
//
// Single-threaded: a min-heap ordered by release time, and ppoll() sleeping
// exactly until either a socket is readable or the earliest queued datagram
// comes due. No busy-waiting, and no per-packet thread.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxDatagram = 65536;

// A datagram waiting for its release time.
struct Pending {
  Clock::time_point release;
  bool to_server;  // false => back to the client
  std::vector<std::byte> bytes;

  // std::priority_queue is a max-heap, so invert the comparison to pop the
  // earliest deadline first.
  bool operator<(const Pending& other) const { return release > other.release; }
};

[[nodiscard]] int make_socket(std::uint16_t bind_port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (bind_port != 0) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(bind_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

[[nodiscard]] bool parse_u64(std::string_view text, unsigned long long& out) {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: delay_proxy <listen-port> <target-port> "
                 "<one-way-delay-us> [idle-exit-ms]\n"
                 "  RTT observed by the endpoints is 2 x one-way-delay\n";
    return 2;
  }

  unsigned long long listen_port = 0;
  unsigned long long target_port = 0;
  unsigned long long delay_us = 0;
  unsigned long long idle_exit_ms = 15000;
  if (!parse_u64(argv[1], listen_port) || !parse_u64(argv[2], target_port) ||
      !parse_u64(argv[3], delay_us) ||
      (argc > 4 && !parse_u64(argv[4], idle_exit_ms))) {
    std::cerr << "arguments must be integers\n";
    return 2;
  }

  const int front = make_socket(static_cast<std::uint16_t>(listen_port));
  const int back = make_socket(0);  // ephemeral; talks to the server
  if (front < 0 || back < 0) {
    std::cerr << "socket setup failed: " << std::strerror(errno) << '\n';
    return 1;
  }

  sockaddr_in server_address{};
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons(static_cast<std::uint16_t>(target_port));
  ::inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

  sockaddr_in client_address{};
  bool client_known = false;

  const auto delay = std::chrono::microseconds{delay_us};
  std::priority_queue<Pending> queue;
  std::vector<std::byte> buffer(kMaxDatagram);

  std::cout << "delay_proxy :" << listen_port << " -> :" << target_port
            << " one-way " << (static_cast<double>(delay_us) / 1000.0)
            << "ms (RTT " << (static_cast<double>(delay_us) / 500.0) << "ms)"
            << std::endl;
  std::cout << "READY" << std::endl;

  std::uint64_t forwarded = 0;
  Clock::time_point last_activity = Clock::now();

  for (;;) {
    pollfd fds[2] = {{front, POLLIN, 0}, {back, POLLIN, 0}};

    // Sleep until the earliest queued datagram is due, or until a socket
    // becomes readable, whichever happens first.
    timespec timeout{};
    timespec* timeout_ptr = nullptr;
    if (!queue.empty()) {
      auto wait = queue.top().release - Clock::now();
      if (wait < Clock::duration::zero()) {
        wait = Clock::duration::zero();
      }
      const auto ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count();
      timeout.tv_sec = static_cast<time_t>(ns / 1000000000);
      timeout.tv_nsec = static_cast<long>(ns % 1000000000);
      timeout_ptr = &timeout;
    } else {
      // Nothing queued: wake up periodically to check the idle deadline.
      timeout.tv_sec = 1;
      timeout.tv_nsec = 0;
      timeout_ptr = &timeout;
    }

    const int ready = ::ppoll(fds, 2, timeout_ptr, nullptr);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "ppoll failed: " << std::strerror(errno) << '\n';
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const ssize_t n =
          ::recvfrom(front, buffer.data(), buffer.size(), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n >= 0) {
        client_address = from;
        client_known = true;
        queue.push(Pending{Clock::now() + delay, true,
                           std::vector<std::byte>(
                               buffer.begin(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(n))});
        last_activity = Clock::now();
      }
    }

    if ((fds[1].revents & POLLIN) != 0) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const ssize_t n =
          ::recvfrom(back, buffer.data(), buffer.size(), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n >= 0) {
        queue.push(Pending{Clock::now() + delay, false,
                           std::vector<std::byte>(
                               buffer.begin(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(n))});
        last_activity = Clock::now();
      }
    }

    // Release everything now due. Popping in a loop matters: several datagrams
    // can come due during a single sleep once there is a window in flight.
    const Clock::time_point now = Clock::now();
    while (!queue.empty() && queue.top().release <= now) {
      const Pending& item = queue.top();
      if (item.to_server) {
        (void)::sendto(back, item.bytes.data(), item.bytes.size(), 0,
                       reinterpret_cast<const sockaddr*>(&server_address),
                       sizeof(server_address));
      } else if (client_known) {
        (void)::sendto(front, item.bytes.data(), item.bytes.size(), 0,
                       reinterpret_cast<const sockaddr*>(&client_address),
                       sizeof(client_address));
      }
      ++forwarded;
      queue.pop();
    }

    if (queue.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_activity)
                .count() > static_cast<long long>(idle_exit_ms)) {
      break;  // transfer finished (or never started); let the script move on
    }
  }

  std::cout << "PROXY_STATS forwarded=" << forwarded << std::endl;
  ::close(front);
  ::close(back);
  return 0;
}
