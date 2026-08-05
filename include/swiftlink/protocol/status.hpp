// Status codes carried in ERROR packets.
//
// A structured code rather than a bare string, so the peer can branch on the
// failure without parsing prose. The human-readable message rides alongside it
// in the same payload for logs.
//
// ERROR payload layout:  [0] = StatusCode, [1..] = UTF-8 message (no NUL)

#pragma once

#include <cstdint>
#include <string_view>

namespace swiftlink::protocol {

enum class StatusCode : std::uint8_t {
  kOk = 0,
  kUnsupportedVersion = 1,
  kInvalidFilename = 2,     // failed sanitisation; likely a traversal attempt
  kFileOpenFailed = 3,
  kIntegrityCheckFailed = 4,  // SHA-256 of the received file did not match
  kSizeMismatch = 5,          // byte count disagrees with the declared size
  kUnknownSession = 6,        // packet for a session we have no record of
  kInternalError = 7,
};

[[nodiscard]] bool is_valid_status_code(std::uint8_t raw) noexcept;
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

}  // namespace swiftlink::protocol
