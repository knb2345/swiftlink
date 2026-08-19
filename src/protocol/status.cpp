#include "swiftlink/protocol/status.hpp"

namespace swiftlink::protocol {

bool is_valid_status_code(std::uint8_t raw) noexcept {
  switch (static_cast<StatusCode>(raw)) {
    case StatusCode::kOk:
    case StatusCode::kUnsupportedVersion:
    case StatusCode::kInvalidFilename:
    case StatusCode::kFileOpenFailed:
    case StatusCode::kIntegrityCheckFailed:
    case StatusCode::kSizeMismatch:
    case StatusCode::kUnknownSession:
    case StatusCode::kInternalError:
    case StatusCode::kServerBusy:
      return true;
  }
  return false;
}

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk:
      return "ok";
    case StatusCode::kUnsupportedVersion:
      return "unsupported protocol version";
    case StatusCode::kInvalidFilename:
      return "filename rejected";
    case StatusCode::kFileOpenFailed:
      return "could not open the output file";
    case StatusCode::kIntegrityCheckFailed:
      return "SHA-256 of the received file did not match the sender's";
    case StatusCode::kSizeMismatch:
      return "received byte count disagrees with the declared size";
    case StatusCode::kUnknownSession:
      return "no such session";
    case StatusCode::kInternalError:
      return "internal error";
    case StatusCode::kServerBusy:
      return "server is at its session limit";
  }
  return "unknown status";
}

}  // namespace swiftlink::protocol
