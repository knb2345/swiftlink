// SwiftLink client: send a file with Selective Repeat reliability.
//
//   swiftlink_client <host> <port> <file> [options]
//     --simulate-loss=P   drop a fraction P of outbound datagrams (0.0 - 1.0)
//     --seed=N            seed the loss generator so a run is reproducible
//     --rto-ms=N          retransmission timeout (default 300)
//     --chunk=N           payload bytes per packet (default 1200)
//     --window=N          packets in flight (default 32; 1 = stop-and-wait)
//     --name=NAME         filename to advertise (default: basename of <file>)
//     --quiet             print only the machine-readable STATS line

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/protocol/packet.hpp"
#include "swiftlink/protocol/status.hpp"
#include "swiftlink/transfer/sender.hpp"
#include "swiftlink/transfer/transfer.hpp"

namespace {

namespace xfer = swiftlink::transfer;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9000;
  std::string path;
  xfer::SenderConfig sender;
  bool quiet = false;
};

[[nodiscard]] bool parse_unsigned(std::string_view text, unsigned long long& out) {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_double(std::string_view text, double& out) {
  // from_chars for floating point is available in libstdc++ 11+, and unlike
  // strtod it does not consult the locale -- so "0.05" parses the same way on a
  // machine whose locale uses a decimal comma.
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  int positional = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg.starts_with("--")) {
      const std::size_t equals = arg.find('=');
      const std::string_view name = arg.substr(0, equals);
      const std::string_view value =
          (equals == std::string_view::npos) ? std::string_view{}
                                             : arg.substr(equals + 1);

      if (name == "--quiet") {
        options.quiet = true;
      } else if (name == "--simulate-loss") {
        double probability = 0.0;
        if (!parse_double(value, probability) || probability < 0.0 ||
            probability > 1.0) {
          std::cerr << "--simulate-loss expects a probability in [0, 1]\n";
          return false;
        }
        options.sender.loss_probability = probability;
      } else if (name == "--seed") {
        unsigned long long seed = 0;
        if (!parse_unsigned(value, seed)) {
          std::cerr << "--seed expects an integer\n";
          return false;
        }
        options.sender.seed = seed;
      } else if (name == "--rto-ms") {
        unsigned long long ms = 0;
        if (!parse_unsigned(value, ms) || ms == 0) {
          std::cerr << "--rto-ms expects a positive integer\n";
          return false;
        }
        options.sender.rto = std::chrono::milliseconds{static_cast<long>(ms)};
      } else if (name == "--window") {
        unsigned long long window = 0;
        if (!parse_unsigned(value, window) || window == 0 ||
            window > swiftlink::transfer::kDefaultReceiverWindowSize) {
          std::cerr << "--window expects 1.."
                    << swiftlink::transfer::kDefaultReceiverWindowSize << '\n';
          return false;
        }
        options.sender.window_size = static_cast<std::uint32_t>(window);
      } else if (name == "--name") {
        options.sender.remote_name = std::string(value);
      } else if (name == "--chunk") {
        unsigned long long size = 0;
        if (!parse_unsigned(value, size) || size == 0 ||
            size > swiftlink::protocol::kMaxPayloadSize) {
          std::cerr << "--chunk expects 1.." << swiftlink::protocol::kMaxPayloadSize
                    << '\n';
          return false;
        }
        options.sender.chunk_size = static_cast<std::size_t>(size);
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return false;
      }
      continue;
    }

    switch (positional++) {
      case 0:
        options.host = arg;
        break;
      case 1: {
        unsigned long long port = 0;
        if (!parse_unsigned(arg, port) || port == 0 || port > 65535) {
          std::cerr << "invalid port: " << arg << '\n';
          return false;
        }
        options.port = static_cast<std::uint16_t>(port);
        break;
      }
      case 2:
        options.path = arg;
        break;
      default:
        std::cerr << "unexpected argument: " << arg << '\n';
        return false;
    }
  }

  if (positional < 3) {
    std::cerr << "usage: swiftlink_client <host> <port> <file> "
                 "[--simulate-loss=P] [--seed=N] [--rto-ms=N] [--chunk=N] "
                 "[--window=N] [--quiet]\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }

  // An unseeded run still needs a definite seed, so that the value can be
  // printed and the run reproduced later.
  if (options.sender.seed == 0) {
    options.sender.seed = std::random_device{}();
  }
  // A fresh random session id per transfer. The receiver requires every DATA
  // and FIN packet to carry it, so a stale datagram from a previous transfer
  // to the same port cannot be mistaken for part of this one.
  if (options.sender.session_id == 0) {
    std::mt19937_64 rng(std::random_device{}());
    do {
      options.sender.session_id = rng();
    } while (options.sender.session_id == 0);
  }

  // Advertise only the basename; the receiver sanitises it again regardless.
  if (options.sender.remote_name.empty()) {
    const std::size_t slash = options.path.find_last_of('/');
    options.sender.remote_name = (slash == std::string::npos)
                                     ? options.path
                                     : options.path.substr(slash + 1);
  }

  swiftlink::net::UdpSocket socket;
  if (!socket.open()) {
    std::cerr << std::format("socket() failed: {}\n", std::strerror(errno));
    return 1;
  }

  const swiftlink::net::Endpoint destination{options.host, options.port};

  if (!options.quiet) {
    std::cout << std::format("sending {} to {}:{}\n", options.path, options.host,
                             options.port);
    std::cout << std::format(
        "  chunk={} window={} rto={}ms simulate_loss={} seed={}\n",
        options.sender.chunk_size, options.sender.window_size,
        options.sender.rto.count(), options.sender.loss_probability,
        options.sender.seed);
  }

  xfer::TransferStats stats;
  const xfer::TransferError error = xfer::send_file(
      socket, destination, options.path, options.sender, stats);

  if (error != xfer::TransferError::kNone) {
    if (error == xfer::TransferError::kRemoteError) {
      std::cerr << std::format(
          "transfer failed: peer reported: {}\n",
          swiftlink::protocol::to_string(stats.remote_status));
    } else {
      std::cerr << std::format("transfer failed: {}\n", xfer::to_string(error));
    }
    return 1;
  }

  // One machine-readable line, so scripts/benchmark.sh never has to parse prose.
  std::cout << std::format(
      "STATS elapsed_s={:.4f} throughput_mbps={:.3f} bytes={} chunks={} "
      "packets_sent={} retransmissions={} simulated_drops={} timeouts={} "
      "window={} seed={}\n",
      stats.elapsed_seconds, stats.throughput_mbps(), stats.bytes_transferred,
      stats.chunks, stats.packets_sent, stats.retransmissions,
      stats.simulated_drops, stats.timeouts, options.sender.window_size,
      options.sender.seed);

  return 0;
}
