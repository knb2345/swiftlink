// SwiftLink server: concurrent, epoll-driven, many sessions at once.
//
//   swiftlink_server <port> <output-dir> [options]
//     --idle-ms=N     abandon a session after N ms of silence (default 30000)
//     --sweep-ms=N    how often to sweep for idle sessions (default 1000)
//     --linger-ms=N   keep a completed session this long, so a retransmitted
//                     FIN is still answered (default 20000; 0 disables)
//     --max-sessions=N  refuse new sessions past this many in progress
//                     (default 256)
//     --window=N      duplicate-detection window (default 1024)
//     --quiet         suppress per-session chatter
//
// Runs until SIGINT or SIGTERM, then shuts down cleanly. The filename comes
// from each client's START packet and is sanitised to a bare name before being
// joined to <output-dir>, so a client cannot write outside it.

#include <sys/stat.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "swiftlink/server/epoll_server.hpp"

namespace {

[[nodiscard]] bool parse_unsigned(std::string_view text, unsigned long long& out) {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_options(int argc, char** argv,
                                 swiftlink::server::ServerConfig& config) {
  int positional = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg.starts_with("--")) {
      const std::size_t equals = arg.find('=');
      const std::string_view name = arg.substr(0, equals);
      const std::string_view value =
          (equals == std::string_view::npos) ? std::string_view{}
                                             : arg.substr(equals + 1);
      unsigned long long number = 0;

      if (name == "--quiet") {
        config.verbose = false;
      } else if (name == "--idle-ms") {
        if (!parse_unsigned(value, number) || number == 0) {
          std::cerr << "--idle-ms expects a positive integer\n";
          return false;
        }
        config.session_idle_timeout =
            std::chrono::milliseconds{static_cast<long>(number)};
      } else if (name == "--sweep-ms") {
        if (!parse_unsigned(value, number) || number == 0) {
          std::cerr << "--sweep-ms expects a positive integer\n";
          return false;
        }
        config.sweep_interval =
            std::chrono::milliseconds{static_cast<long>(number)};
      } else if (name == "--linger-ms") {
        if (!parse_unsigned(value, number)) {
          std::cerr << "--linger-ms expects a non-negative integer\n";
          return false;
        }
        config.session_linger =
            std::chrono::milliseconds{static_cast<long>(number)};
      } else if (name == "--max-sessions") {
        if (!parse_unsigned(value, number) || number == 0) {
          std::cerr << "--max-sessions expects a positive integer\n";
          return false;
        }
        config.max_sessions = static_cast<std::size_t>(number);
      } else if (name == "--window") {
        if (!parse_unsigned(value, number) || number == 0) {
          std::cerr << "--window expects a positive integer\n";
          return false;
        }
        config.window_size = static_cast<std::uint32_t>(number);
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
        config.port = static_cast<std::uint16_t>(port);
        break;
      }
      case 1:
        config.output_directory = arg;
        break;
      default:
        std::cerr << "unexpected argument: " << arg << '\n';
        return false;
    }
  }

  if (positional < 2) {
    std::cerr << "usage: swiftlink_server <port> <output-dir> "
                 "[--idle-ms=N] [--sweep-ms=N] [--linger-ms=N] "
                 "[--max-sessions=N] [--window=N] "
                 "[--quiet]\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  swiftlink::server::ServerConfig config;
  if (!parse_options(argc, argv, config)) {
    return 2;
  }

  if (::mkdir(config.output_directory.c_str(), 0755) != 0 && errno != EEXIST) {
    std::cerr << std::format("mkdir({}) failed: {}\n", config.output_directory,
                             std::strerror(errno));
    return 1;
  }

  return swiftlink::server::run(config);
}
