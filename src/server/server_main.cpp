// SwiftLink server: receive one file, write it, print statistics, exit.
//
//   swiftlink_server <port> <output-file> [--idle-ms=N] [--quiet]
//
// One transfer per process, sequentially. Concurrency arrives in milestone 5
// with epoll; until then the point is the reliability logic, not the plumbing.

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "swiftlink/net/udp_socket.hpp"
#include "swiftlink/transfer/receiver.hpp"
#include "swiftlink/transfer/transfer.hpp"

namespace {

namespace xfer = swiftlink::transfer;

struct Options {
  std::uint16_t port = 9000;
  std::string output_path;
  xfer::ReceiverConfig receiver;
  bool quiet = false;
};

[[nodiscard]] bool parse_unsigned(std::string_view text, unsigned long long& out) {
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
      } else if (name == "--idle-ms") {
        unsigned long long ms = 0;
        if (!parse_unsigned(value, ms) || ms == 0) {
          std::cerr << "--idle-ms expects a positive integer\n";
          return false;
        }
        options.receiver.idle_timeout =
            std::chrono::milliseconds{static_cast<long>(ms)};
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return false;
      }
      continue;
    }

    switch (positional++) {
      case 0: {
        unsigned long long port = 0;
        if (!parse_unsigned(arg, port) || port == 0 || port > 65535) {
          std::cerr << "invalid port: " << arg << '\n';
          return false;
        }
        options.port = static_cast<std::uint16_t>(port);
        break;
      }
      case 1:
        options.output_path = arg;
        break;
      default:
        std::cerr << "unexpected argument: " << arg << '\n';
        return false;
    }
  }

  if (positional < 2) {
    std::cerr << "usage: swiftlink_server <port> <output-file> "
                 "[--idle-ms=N] [--quiet]\n";
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

  swiftlink::net::UdpSocket socket;
  if (!socket.open()) {
    std::cerr << std::format("socket() failed: {}\n", std::strerror(errno));
    return 1;
  }
  if (!socket.bind(options.port)) {
    std::cerr << std::format("bind({}) failed: {}\n", options.port,
                             std::strerror(errno));
    return 1;
  }

  if (!options.quiet) {
    std::cout << std::format("swiftlink server on 0.0.0.0:{} -> {}\n",
                             options.port, options.output_path);
  }
  // Unbuffered, so a benchmark script can wait for this line before starting
  // the client and know the socket is genuinely bound.
  std::cout << "READY" << std::endl;

  xfer::TransferStats stats;
  const xfer::TransferError error = xfer::receive_file(
      socket, options.output_path, options.receiver, stats);

  if (error != xfer::TransferError::kNone) {
    std::cerr << std::format("receive failed: {}\n", xfer::to_string(error));
    return 1;
  }

  std::cout << std::format(
      "STATS elapsed_s={:.4f} throughput_mbps={:.3f} bytes={} chunks={} "
      "packets_received={} duplicates={} out_of_order={}\n",
      stats.elapsed_seconds, stats.throughput_mbps(), stats.bytes_transferred,
      stats.chunks, stats.packets_received, stats.duplicates_received,
      stats.out_of_order_received);

  return 0;
}
