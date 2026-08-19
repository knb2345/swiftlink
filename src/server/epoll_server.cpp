#include "swiftlink/server/epoll_server.hpp"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "swiftlink/io/unique_fd.hpp"
#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/transfer/receiver_session.hpp"

namespace swiftlink::server {
namespace {

namespace proto = swiftlink::protocol;
namespace xfer = swiftlink::transfer;
using Clock = std::chrono::steady_clock;

// A session is identified by the client's address *and* the session id it
// chose.
//
// The address alone is not enough: NAT can put several clients behind one
// address, and one client may run two transfers at once from different source
// ports. The session id alone is not enough either -- it is chosen by the
// client, so two clients could pick the same one, and an off-path attacker who
// guessed an id could otherwise write into somebody else's transfer. Requiring
// both means an attacker must guess the id *and* forge the source address.
struct SessionKey {
  std::string address;
  std::uint16_t port = 0;
  std::uint64_t session_id = 0;

  // std::map needs an ordering. A hash map would need a hash for this type;
  // an ordered map needs only this, and the session count is small enough that
  // the log(N) lookup is irrelevant next to a pwrite.
  bool operator<(const SessionKey& other) const noexcept {
    if (address != other.address) return address < other.address;
    if (port != other.port) return port < other.port;
    return session_id < other.session_id;
  }
};

struct Session {
  explicit Session(std::uint32_t window, std::string directory)
      : receiver(window, std::move(directory)) {}

  xfer::ReceiverSession receiver;
  net::Endpoint peer;
  Clock::time_point last_activity;
  Clock::time_point started;

  // Set once the transfer has settled. The session stays in the map after
  // that, holding no file descriptor, purely so a retransmitted FIN still
  // finds someone to answer it.
  bool lingering = false;
  Clock::time_point completed_at;
};

// Answers a packet we are declining to keep any state about. Written out here
// rather than through a ReceiverSession, because the entire point is that no
// session was allocated.
void send_error(net::UdpSocket& socket, const net::Endpoint& peer,
                const proto::PacketHeader& request, proto::StatusCode code) {
  const std::string_view message = proto::to_string(code);
  std::vector<std::byte> payload;
  payload.reserve(1 + message.size());
  payload.push_back(static_cast<std::byte>(code));
  for (const char c : message) {
    payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }

  proto::PacketHeader header;
  header.packet_type = proto::PacketType::kError;
  header.session_id = request.session_id;
  header.sequence_number = request.sequence_number;
  header.acknowledgement_number = request.sequence_number;

  const std::vector<std::byte> wire = proto::serialize(header, payload);
  if (socket.send_to(wire, peer) < 0 && errno != EAGAIN) {
    std::cerr << std::format("sendto failed: {}\n", std::strerror(errno));
  }
}

[[nodiscard]] bool add_to_epoll(int epoll_fd, int fd) {
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = fd;
  return ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

// Blocks the signals we intend to read as data, then opens a descriptor that
// delivers them.
//
// Blocking first is essential and easy to get wrong: signalfd does not stop
// normal delivery. Without sigprocmask the default disposition still applies,
// so the first SIGINT would kill the process before the loop ever read it.
[[nodiscard]] io::UniqueFd make_signal_fd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);

  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
    return io::UniqueFd{};
  }
  return io::UniqueFd{::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)};
}

[[nodiscard]] io::UniqueFd make_timer_fd(std::chrono::milliseconds interval) {
  io::UniqueFd fd{::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)};
  if (!fd.valid()) {
    return fd;
  }

  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(interval);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      interval - seconds);

  itimerspec spec{};
  spec.it_interval.tv_sec = static_cast<time_t>(seconds.count());
  spec.it_interval.tv_nsec = static_cast<long>(nanos.count());
  spec.it_value = spec.it_interval;  // first expiry after one full interval

  if (::timerfd_settime(fd.get(), 0, &spec, nullptr) != 0) {
    return io::UniqueFd{};
  }
  return fd;
}

}  // namespace

int run(const ServerConfig& config) {
  net::UdpSocket socket;
  if (!socket.open()) {
    std::cerr << std::format("socket() failed: {}\n", std::strerror(errno));
    return 1;
  }
  if (!socket.bind(config.port)) {
    std::cerr << std::format("bind({}) failed: {}\n", config.port,
                             std::strerror(errno));
    return 1;
  }
  // Non-blocking is what makes a single loop safe: recv_from returns EAGAIN
  // when the queue is empty instead of parking the thread, so one quiet socket
  // can never stall the sessions behind it.
  if (!socket.set_nonblocking(true)) {
    std::cerr << std::format("set_nonblocking failed: {}\n",
                             std::strerror(errno));
    return 1;
  }

  io::UniqueFd epoll_fd{::epoll_create1(EPOLL_CLOEXEC)};
  io::UniqueFd signal_fd = make_signal_fd();
  io::UniqueFd timer_fd = make_timer_fd(config.sweep_interval);

  if (!epoll_fd.valid() || !signal_fd.valid() || !timer_fd.valid()) {
    std::cerr << std::format("epoll/signalfd/timerfd setup failed: {}\n",
                             std::strerror(errno));
    return 1;
  }

  if (!add_to_epoll(epoll_fd.get(), socket.fd()) ||
      !add_to_epoll(epoll_fd.get(), signal_fd.get()) ||
      !add_to_epoll(epoll_fd.get(), timer_fd.get())) {
    std::cerr << std::format("epoll_ctl failed: {}\n", std::strerror(errno));
    return 1;
  }

  std::map<SessionKey, std::unique_ptr<Session>> sessions;
  std::vector<std::byte> buffer(proto::kHeaderWireSize + proto::kMaxPayloadSize);

  std::cout << std::format("swiftlink epoll server on 0.0.0.0:{} -> {}/\n",
                           config.port, config.output_directory);
  std::cout << "READY" << std::endl;

  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t expired = 0;
  std::uint64_t refused = 0;
  bool running = true;

  while (running) {
    epoll_event events[8];
    const int ready =
        ::epoll_wait(epoll_fd.get(), events, 8, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << std::format("epoll_wait failed: {}\n", std::strerror(errno));
      return 1;
    }

    for (int i = 0; i < ready; ++i) {
      const int fd = events[i].data.fd;

      // ---------------------------------------------------------------
      // Shutdown
      // ---------------------------------------------------------------
      if (fd == signal_fd.get()) {
        signalfd_siginfo info{};
        while (::read(signal_fd.get(), &info, sizeof(info)) == sizeof(info)) {
          std::cout << std::format("\nreceived signal {}, shutting down\n",
                                   info.ssi_signo);
        }
        running = false;
        continue;
      }

      // ---------------------------------------------------------------
      // Idle sweep
      // ---------------------------------------------------------------
      if (fd == timer_fd.get()) {
        std::uint64_t expirations = 0;
        // The timerfd must be read or it stays readable and epoll spins.
        while (::read(timer_fd.get(), &expirations, sizeof(expirations)) ==
               sizeof(expirations)) {
        }

        const Clock::time_point now = Clock::now();
        for (auto it = sessions.begin(); it != sessions.end();) {
          // A settled session is not idle, it is done. It leaves on the linger
          // clock and is not reported as a timeout, because nothing about it
          // failed.
          if (it->second->lingering) {
            const auto held =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second->completed_at);
            if (held > config.session_linger) {
              it = sessions.erase(it);
            } else {
              ++it;
            }
            continue;
          }

          const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - it->second->last_activity);
          if (idle > config.session_idle_timeout) {
            if (config.verbose) {
              std::cout << std::format(
                  "session {:016x} from {}:{} timed out after {} ms idle\n",
                  it->first.session_id, it->first.address, it->first.port,
                  idle.count());
            }
            ++expired;
            it = sessions.erase(it);  // erase closes the file via ~ReceiverSession
          } else {
            ++it;
          }
        }
        continue;
      }

      // ---------------------------------------------------------------
      // Datagrams
      // ---------------------------------------------------------------
      if (fd != socket.fd()) {
        continue;
      }

      // Drain the socket. epoll here is level-triggered, so stopping early
      // would simply mean another wakeup, but draining in one go avoids a
      // syscall per packet's worth of loop overhead.
      for (;;) {
        net::Endpoint peer;
        const std::ptrdiff_t received = socket.recv_from(buffer, peer);
        if (received < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;  // queue drained
          }
          if (errno == EINTR) {
            continue;
          }
          std::cerr << std::format("recvfrom failed: {}\n",
                                   std::strerror(errno));
          break;
        }

        const auto result = proto::deserialize(std::span<const std::byte>{
            buffer.data(), static_cast<std::size_t>(received)});
        if (!result.ok()) {
          // Malformed, corrupt, or not ours. A public UDP port receives plenty
          // of this; it must never be able to disturb a live session.
          continue;
        }

        const proto::Packet& packet = result.value();
        const SessionKey key{peer.address, peer.port,
                             packet.header.session_id};

        auto it = sessions.find(key);
        if (it == sessions.end()) {
          // Only START may create a session. Anything else for an unknown key
          // is ignored rather than answered, so a stray datagram cannot make
          // us allocate state.
          if (packet.header.packet_type != proto::PacketType::kStart) {
            continue;
          }

          // Admission control. Lingering sessions are already finished and do
          // not count against the limit; they are retired early if the table
          // is under pressure, since answering a retransmitted FIN matters
          // less than being able to accept new work at all.
          std::size_t active = 0;
          for (const auto& entry : sessions) {
            if (!entry.second->lingering) {
              ++active;
            }
          }
          if (active >= config.max_sessions) {
            ++refused;
            if (config.verbose) {
              std::cerr << std::format(
                  "refused session {:016x} from {}:{}: at the {} session "
                  "limit\n",
                  key.session_id, peer.address, peer.port, config.max_sessions);
            }
            send_error(socket, peer, packet.header,
                       proto::StatusCode::kServerBusy);
            continue;
          }
          if (sessions.size() >= config.max_sessions * 2) {
            for (auto scan = sessions.begin(); scan != sessions.end();) {
              scan = scan->second->lingering ? sessions.erase(scan)
                                             : std::next(scan);
            }
          }

          auto session = std::make_unique<Session>(config.window_size,
                                                   config.output_directory);
          session->peer = peer;
          session->started = Clock::now();
          it = sessions.emplace(key, std::move(session)).first;

          if (config.verbose) {
            std::cout << std::format("session {:016x} opened from {}:{}\n",
                                     key.session_id, peer.address, peer.port);
          }
        }

        Session& session = *it->second;
        session.last_activity = Clock::now();

        const xfer::ReceiverSession::Reply reply =
            session.receiver.handle_packet(packet);

        if (reply.send) {
          proto::PacketHeader header;
          header.packet_type = reply.type;
          header.session_id = packet.header.session_id;
          header.sequence_number = reply.sequence;
          header.acknowledgement_number = reply.sequence;

          const std::vector<std::byte> wire =
              proto::serialize(header, reply.payload);
          if (socket.send_to(wire, session.peer) < 0 && errno != EAGAIN) {
            std::cerr << std::format("sendto failed: {}\n",
                                     std::strerror(errno));
          }
        }

        if (session.receiver.finished() && !session.lingering) {
          const xfer::TransferStats& stats = session.receiver.stats();
          const double elapsed =
              std::chrono::duration<double>(Clock::now() - session.started)
                  .count();

          if (session.receiver.error() == xfer::TransferError::kNone) {
            ++completed;
            std::cout << std::format(
                "session {:016x} complete: {} ({} bytes, {} chunks, {:.4f}s, "
                "{:.3f} Mbps, {} duplicates)\n",
                key.session_id, session.receiver.output_path(),
                stats.bytes_transferred, stats.chunks, elapsed,
                elapsed > 0 ? (static_cast<double>(stats.bytes_transferred) *
                               8.0 / elapsed / 1e6)
                            : 0.0,
                stats.duplicates_received);
          } else {
            ++failed;
            std::cerr << std::format("session {:016x} failed: {}\n",
                                     key.session_id,
                                     xfer::to_string(session.receiver.error()));
          }

          // Deliberately not erased. The file is closed here because the
          // bytes are already fsynced, but the session entry stays until the
          // sweep retires it, so a retransmitted FIN is answered rather than
          // ignored.
          session.receiver.close_file();
          session.lingering = true;
          session.completed_at = Clock::now();
        }
      }
    }
  }

  // Sessions still open at shutdown are incomplete by definition: their files
  // are partial. They are reported rather than silently dropped. Lingering
  // sessions are not among them -- they have already been counted as completed
  // or failed, and are only still present to answer a retransmitted FIN.
  std::uint64_t abandoned = 0;
  for (const auto& [key, session] : sessions) {
    if (session->lingering) {
      continue;
    }
    ++abandoned;
    std::cerr << std::format(
        "session {:016x} from {}:{} abandoned at shutdown ({} bytes written)\n",
        key.session_id, key.address, key.port,
        session->receiver.stats().bytes_transferred);
  }

  std::cout << std::format(
      "shutdown: {} completed, {} failed, {} timed out, {} abandoned, "
      "{} refused\n",
      completed, failed, expired, abandoned, refused);

  return 0;
}

}  // namespace swiftlink::server
