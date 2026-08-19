// A UDP relay that sits between client and server and impairs the path:
// delay, loss (optionally correlated), duplication, reordering and corruption.
//
// WHY THIS EXISTS
// ---------------
// The test plan called for `tc qdisc add dev lo root netem ...`. The WSL2
// kernel this project is developed on is built without netem:
//
//     $ zcat /proc/config.gz | grep NET_SCH_NETEM
//     # CONFIG_NET_SCH_NETEM is not set
//
// and there is no sch_netem module on disk to load, at any privilege level. A
// veth pair in a network namespace does not help: that works around loopback
// applying a qdisc twice per round trip, but it still needs the same missing
// qdisc. So the impairments are reimplemented here, in userspace, in the data
// path.
//
// HOW FAITHFUL IS IT
// ------------------
// The *models* are netem's, deliberately, so the numbers mean the same thing:
//
//   loss P% C%      netem's correlated-loss generator (see CorrelatedRandom).
//                   C=0 is plain Bernoulli and delivers exactly P.
//                   C>0 inherits netem's defect along with its model: the AR(1)
//                   recursion's stationary distribution clusters near 0.5, so
//                   P(x < P) falls far below P and the requested marginal rate
//                   is not delivered. Measured here: --loss=5% --loss-corr=25%
//                   dropped 2 of 1754 datagrams, 0.11% against a requested 5%.
//                   It is kept because it is what netem does; for clustered
//                   loss that actually arrives at its stated rate, use
//                   --burst-loss below.
//   burst-loss P%   Gilbert-Elliott (netem's `loss gemodel`), with
//   burst-len L     mean burst length L. Both the marginal rate and the burst
//                   length come out as asked, which is what makes it usable as
//                   a measurement rather than a demonstration.
//   duplicate P%    the datagram is queued twice, as netem does.
//   reorder P% C%   with probability P a datagram skips the delay queue and
//                   goes out immediately, overtaking those already waiting.
//                   This is netem's mechanism exactly, and like netem it does
//                   nothing unless a delay is configured.
//   corrupt P%      flips one uniformly chosen bit in the datagram.
//
// What it is NOT: a kernel qdisc. It adds a userspace scheduling hop, so the
// delay it adds has more jitter than netem's, and it is one process handling
// one client. Where a number depends on that, it is called out rather than
// presented as equivalent.
//
// Every impairment is counted and reported in the PROXY_STATS line at exit, so
// a run's log proves which impairments actually fired rather than only which
// were requested.
//
// TOPOLOGY
//
//     client  <--->  proxy :listen_port  <--->  server :target_port
//
// The client sends to the proxy instead of the server, and the proxy learns the
// client's address from the first datagram it receives. Each direction is
// delayed by `delay_us`, so the observed RTT is 2 x delay_us.
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
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxDatagram = 65536;

// netem's correlated random generator, reimplemented.
//
// The kernel computes, in fixed point:
//
//     answer = (value * (2^32 - rho) + last * rho) >> 32
//
// which is the convex combination `value*(1-rho) + last*rho`. Each draw is
// pulled toward the previous one, so with rho > 0 a drop makes the next drop
// more likely and losses arrive in bursts. rho = 0 degenerates to independent
// uniform draws, i.e. Bernoulli loss.
class CorrelatedRandom {
 public:
  explicit CorrelatedRandom(double rho) : rho_(rho) {}

  [[nodiscard]] double next(std::mt19937_64& rng) {
    const double value = distribution_(rng);
    if (rho_ <= 0.0) {
      return value;
    }
    last_ = value * (1.0 - rho_) + last_ * rho_;
    return last_;
  }

 private:
  double rho_;
  double last_ = 0.0;
  std::uniform_real_distribution<double> distribution_{0.0, 1.0};
};

// Gilbert-Elliott burst loss: netem's `loss gemodel`, and the model to use when
// the question is "how does this behave when losses arrive in clumps".
//
// Two states. In GOOD nothing is lost; in BAD everything is lost. `p` is the
// per-packet probability of entering BAD, `r` of leaving it. That gives a
// stationary bad-state probability of p/(p+r) and a mean burst length of 1/r,
// so both the average loss rate and the clumpiness are set independently and
// are actually achieved -- which is the whole reason to prefer this over the
// correlated-uniform model below.
//
// The transition is evaluated before the loss decision for the *entering*
// state, matching netem's loss_gilb_ell().
class GilbertElliott {
 public:
  GilbertElliott(double to_bad, double to_good)
      : to_bad_(to_bad), to_good_(to_good) {}

  [[nodiscard]] bool lost(std::mt19937_64& rng) {
    if (bad_) {
      if (distribution_(rng) < to_good_) {
        bad_ = false;
      }
      return true;  // everything in the bad state is lost
    }
    if (distribution_(rng) < to_bad_) {
      bad_ = true;
    }
    return false;  // the good state loses nothing
  }

 private:
  double to_bad_;
  double to_good_;
  bool bad_ = false;
  std::uniform_real_distribution<double> distribution_{0.0, 1.0};
};

// One impairment profile. Probabilities are fractions in [0, 1].
struct Impairment {
  double loss = 0.0;
  double loss_correlation = 0.0;
  double burst_loss = 0.0;    // marginal loss rate for the Gilbert-Elliott path
  double burst_length = 4.0;  // mean packets lost per burst
  double duplicate = 0.0;
  double reorder = 0.0;
  double reorder_correlation = 0.0;
  double corrupt = 0.0;

  // Gilbert-Elliott transition probabilities implied by burst_loss and
  // burst_length. Mean burst length L means leaving BAD with probability 1/L;
  // a stationary bad fraction of P then forces p = (P/(1-P))/L.
  [[nodiscard]] double gilbert_to_good() const { return 1.0 / burst_length; }
  [[nodiscard]] double gilbert_to_bad() const {
    if (burst_loss >= 1.0) {
      return 1.0;
    }
    return (burst_loss / (1.0 - burst_loss)) * gilbert_to_good();
  }
};

// The mutable per-direction state: one correlated generator per impairment,
// because each needs its own memory of its previous draw.
struct Generators {
  explicit Generators(const Impairment& impairment)
      : loss(impairment.loss_correlation),
        reorder(impairment.reorder_correlation),
        duplicate(0.0),
        corrupt(0.0),
        burst(impairment.gilbert_to_bad(), impairment.gilbert_to_good()) {}

  CorrelatedRandom loss;
  CorrelatedRandom reorder;
  CorrelatedRandom duplicate;
  CorrelatedRandom corrupt;
  GilbertElliott burst;
};

struct DirectionStats {
  std::uint64_t received = 0;
  std::uint64_t forwarded = 0;
  std::uint64_t dropped = 0;
  std::uint64_t burst_dropped = 0;  // subset of dropped, from Gilbert-Elliott
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;
  std::uint64_t corrupted = 0;
};

// A datagram waiting for its release time.
struct Pending {
  Clock::time_point release;
  std::uint64_t order = 0;  // arrival sequence, used only to break ties
  bool to_server = true;    // false => back to the client
  std::vector<std::byte> bytes;

  // std::priority_queue is a max-heap, so invert the comparison to pop the
  // earliest deadline first.
  //
  // The `order` tiebreaker is not cosmetic. Two datagrams queued during the
  // same clock tick get identical release times, and without a tiebreaker the
  // heap would emit them in whatever order it happens to hold them -- which
  // means the proxy would reorder traffic even with --reorder=0. Reordering
  // has to be something this tool does only when asked, or a clean run proves
  // nothing.
  bool operator<(const Pending& other) const {
    if (release != other.release) {
      return release > other.release;
    }
    return order > other.order;
  }
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

// Accepts either a fraction ("0.05") or a percentage ("5%"), because netem is
// specified in percent and copying a netem invocation across should not
// silently mean something 100x different.
[[nodiscard]] bool parse_probability(std::string_view text, double& out) {
  bool percent = false;
  if (!text.empty() && text.back() == '%') {
    percent = true;
    text.remove_suffix(1);
  }
  double value = 0.0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  if (percent) {
    value /= 100.0;
  }
  if (value < 0.0 || value > 1.0) {
    return false;
  }
  out = value;
  return true;
}

enum class Direction { kBoth, kToServer, kToClient };

// Set from a signal handler, so a harness can stop the proxy and still get the
// PROXY_STATS line. Without this the counters die with the process on SIGTERM
// and every condition becomes unverifiable -- the run would only record what
// was *requested*, never what was applied.
volatile sig_atomic_t g_stop = 0;

extern "C" void on_signal(int) { g_stop = 1; }

void usage() {
  std::cerr
      << "usage: delay_proxy <listen-port> <target-port> <one-way-delay-us> "
         "[idle-exit-ms] [options]\n"
         "  RTT observed by the endpoints is 2 x one-way-delay\n"
         "\n"
         "  --loss=P[%]        drop probability            (default 0)\n"
         "  --loss-corr=P[%]   loss correlation, netem's   (default 0)\n"
         "                     NOTE: netem's correlated-loss model does not\n"
         "                     deliver the requested marginal rate; use\n"
         "                     --burst-loss for clustered loss that does.\n"
         "  --burst-loss=P[%]  Gilbert-Elliott loss rate   (default 0)\n"
         "  --burst-len=N      mean packets per burst      (default 4)\n"
         "  --duplicate=P[%]   duplication probability     (default 0)\n"
         "  --reorder=P[%]     skip-the-delay probability  (default 0)\n"
         "  --reorder-corr=P[%] reorder correlation        (default 0)\n"
         "  --corrupt=P[%]     single-bit-flip probability (default 0)\n"
         "  --impair=WHICH     both | to-server | to-client (default both)\n"
         "  --seed=N           seed the generators, so a run reproduces\n";
}

}  // namespace

int main(int argc, char** argv) {
  unsigned long long listen_port = 0;
  unsigned long long target_port = 0;
  unsigned long long delay_us = 0;
  unsigned long long idle_exit_ms = 15000;
  unsigned long long seed = 0;
  Impairment impairment;
  Direction impair_direction = Direction::kBoth;

  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};

    if (arg.starts_with("--")) {
      const std::size_t equals = arg.find('=');
      const std::string_view name = arg.substr(0, equals);
      const std::string_view value = (equals == std::string_view::npos)
                                         ? std::string_view{}
                                         : arg.substr(equals + 1);

      bool ok = true;
      if (name == "--loss") {
        ok = parse_probability(value, impairment.loss);
      } else if (name == "--loss-corr") {
        ok = parse_probability(value, impairment.loss_correlation);
      } else if (name == "--burst-loss") {
        ok = parse_probability(value, impairment.burst_loss);
      } else if (name == "--burst-len") {
        double length = 0.0;
        ok = std::from_chars(value.data(), value.data() + value.size(), length)
                     .ec == std::errc{} &&
             length >= 1.0;
        if (ok) {
          impairment.burst_length = length;
        }
      } else if (name == "--duplicate") {
        ok = parse_probability(value, impairment.duplicate);
      } else if (name == "--reorder") {
        ok = parse_probability(value, impairment.reorder);
      } else if (name == "--reorder-corr") {
        ok = parse_probability(value, impairment.reorder_correlation);
      } else if (name == "--corrupt") {
        ok = parse_probability(value, impairment.corrupt);
      } else if (name == "--seed") {
        ok = parse_u64(value, seed);
      } else if (name == "--impair") {
        if (value == "both") {
          impair_direction = Direction::kBoth;
        } else if (value == "to-server") {
          impair_direction = Direction::kToServer;
        } else if (value == "to-client") {
          impair_direction = Direction::kToClient;
        } else {
          ok = false;
        }
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        usage();
        return 2;
      }

      if (!ok) {
        std::cerr << "bad value for " << name << ": " << value << '\n';
        return 2;
      }
      continue;
    }

    unsigned long long number = 0;
    if (!parse_u64(arg, number)) {
      std::cerr << "expected an integer, got: " << arg << '\n';
      usage();
      return 2;
    }
    switch (positional++) {
      case 0: listen_port = number; break;
      case 1: target_port = number; break;
      case 2: delay_us = number; break;
      case 3: idle_exit_ms = number; break;
      default:
        std::cerr << "unexpected argument: " << arg << '\n';
        return 2;
    }
  }

  if (positional < 3) {
    usage();
    return 2;
  }

  if (impairment.reorder > 0.0 && delay_us == 0) {
    // Same rule netem has. Reordering here means "jump ahead of what is already
    // queued"; with no delay nothing is ever queued, so the option would look
    // like it applied and do nothing at all.
    std::cerr << "--reorder needs a nonzero one-way delay to have any effect\n";
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

  if (seed == 0) {
    seed = std::random_device{}();
  }
  std::mt19937_64 rng(seed);

  // One generator set per direction: correlation is a property of a path, and
  // sharing the state between directions would make a forward-path loss burst
  // bleed into the ACK path.
  Generators to_server_generators(impairment);
  Generators to_client_generators(impairment);
  DirectionStats to_server_stats;
  DirectionStats to_client_stats;

  const auto delay = std::chrono::microseconds{delay_us};
  std::priority_queue<Pending> queue;
  std::vector<std::byte> buffer(kMaxDatagram);
  std::uint64_t arrival_counter = 0;

  std::cout << "delay_proxy :" << listen_port << " -> :" << target_port
            << " one-way " << (static_cast<double>(delay_us) / 1000.0)
            << "ms (RTT " << (static_cast<double>(delay_us) / 500.0) << "ms)"
            << std::endl;
  std::cout << "impair loss=" << impairment.loss
            << " loss_corr=" << impairment.loss_correlation
            << " burst_loss=" << impairment.burst_loss
            << " burst_len=" << impairment.burst_length
            << " ge_p=" << impairment.gilbert_to_bad()
            << " ge_r=" << impairment.gilbert_to_good()
            << " duplicate=" << impairment.duplicate
            << " reorder=" << impairment.reorder
            << " corrupt=" << impairment.corrupt
            << " direction="
            << (impair_direction == Direction::kBoth        ? "both"
                : impair_direction == Direction::kToServer ? "to-server"
                                                            : "to-client")
            << " seed=" << seed << std::endl;
  std::cout << "READY" << std::endl;

  struct sigaction action{};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  Clock::time_point last_activity = Clock::now();

  // Applies the impairment profile to one datagram and queues whatever
  // survives. Returns nothing: everything observable lands in `stats`.
  const auto accept = [&](bool to_server, const std::byte* data,
                          std::size_t length) {
    DirectionStats& stats = to_server ? to_server_stats : to_client_stats;
    Generators& generators =
        to_server ? to_server_generators : to_client_generators;
    ++stats.received;

    // Activity is measured on arrival, not on forwarding. A datagram we chose
    // to drop still means the path is in use, and the idle deadline exists to
    // notice that the transfer ended -- not to give up during a long loss
    // burst, which is exactly when the run is most worth observing.
    last_activity = Clock::now();

    const bool impaired =
        impair_direction == Direction::kBoth ||
        (to_server && impair_direction == Direction::kToServer) ||
        (!to_server && impair_direction == Direction::kToClient);

    if (impaired && impairment.loss > 0.0 &&
        generators.loss.next(rng) < impairment.loss) {
      ++stats.dropped;
      return;
    }

    if (impaired && impairment.burst_loss > 0.0 &&
        generators.burst.lost(rng)) {
      ++stats.dropped;
      ++stats.burst_dropped;
      return;
    }

    std::vector<std::byte> bytes(data, data + length);

    if (impaired && impairment.corrupt > 0.0 &&
        generators.corrupt.next(rng) < impairment.corrupt && length > 0) {
      // One uniformly chosen bit anywhere in the datagram, header included --
      // the point is to prove the receiver rejects it, and a corrupt magic or
      // length field is as valid a way in as a corrupt payload byte.
      std::uniform_int_distribution<std::size_t> byte_pick(0, length - 1);
      std::uniform_int_distribution<int> bit_pick(0, 7);
      const std::size_t index = byte_pick(rng);
      bytes[index] ^= static_cast<std::byte>(1U << bit_pick(rng));
      ++stats.corrupted;
    }

    // netem's reorder: with probability P the datagram skips the delay queue
    // and leaves immediately, overtaking everything already waiting.
    Clock::time_point release = Clock::now() + delay;
    if (impaired && impairment.reorder > 0.0 &&
        generators.reorder.next(rng) < impairment.reorder) {
      release = Clock::now();
      ++stats.reordered;
    }

    queue.push(Pending{release, arrival_counter++, to_server, bytes});

    if (impaired && impairment.duplicate > 0.0 &&
        generators.duplicate.next(rng) < impairment.duplicate) {
      // A second copy on the same schedule. The receiver must recognise it as
      // a duplicate and re-acknowledge rather than write it twice.
      queue.push(Pending{release, arrival_counter++, to_server,
                         std::move(bytes)});
      ++stats.duplicated;
    }
  };

  while (g_stop == 0) {
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
        accept(true, buffer.data(), static_cast<std::size_t>(n));
      }
    }

    if ((fds[1].revents & POLLIN) != 0) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const ssize_t n =
          ::recvfrom(back, buffer.data(), buffer.size(), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n >= 0) {
        accept(false, buffer.data(), static_cast<std::size_t>(n));
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
        ++to_server_stats.forwarded;
      } else if (client_known) {
        (void)::sendto(front, item.bytes.data(), item.bytes.size(), 0,
                       reinterpret_cast<const sockaddr*>(&client_address),
                       sizeof(client_address));
        ++to_client_stats.forwarded;
      }
      queue.pop();
    }

    if (queue.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_activity)
                .count() > static_cast<long long>(idle_exit_ms)) {
      break;  // transfer finished (or never started); let the script move on
    }
  }

  // Reported per direction so a log proves what the path actually did. A run
  // claiming "5% loss" whose stats show zero drops did not test what it says.
  std::cout << "PROXY_STATS"
            << " fwd_received=" << to_server_stats.received
            << " fwd_forwarded=" << to_server_stats.forwarded
            << " fwd_dropped=" << to_server_stats.dropped
            << " fwd_burst_dropped=" << to_server_stats.burst_dropped
            << " fwd_duplicated=" << to_server_stats.duplicated
            << " fwd_reordered=" << to_server_stats.reordered
            << " fwd_corrupted=" << to_server_stats.corrupted
            << " rev_received=" << to_client_stats.received
            << " rev_forwarded=" << to_client_stats.forwarded
            << " rev_dropped=" << to_client_stats.dropped
            << " rev_burst_dropped=" << to_client_stats.burst_dropped
            << " rev_duplicated=" << to_client_stats.duplicated
            << " rev_reordered=" << to_client_stats.reordered
            << " rev_corrupted=" << to_client_stats.corrupted << std::endl;
  ::close(front);
  ::close(back);
  return 0;
}
