#include "swiftlink/transfer/transfer.hpp"

namespace swiftlink::transfer {

std::string_view to_string(TransferError error) noexcept {
  switch (error) {
    case TransferError::kNone:
      return "ok";
    case TransferError::kFileOpenFailed:
      return "could not open file";
    case TransferError::kFileReadFailed:
      return "read from file failed";
    case TransferError::kFileWriteFailed:
      return "write to file failed";
    case TransferError::kSocketFailed:
      return "socket operation failed";
    case TransferError::kSendFailed:
      return "sendto failed";
    case TransferError::kReceiveFailed:
      return "recvfrom failed";
    case TransferError::kPeerUnresponsive:
      return "peer stopped responding";
    case TransferError::kProtocolViolation:
      return "peer violated the protocol";
    case TransferError::kSizeMismatch:
      return "received byte count disagrees with the declared file size";
    case TransferError::kHandshakeFailed:
      return "no START_ACK from the peer";
    case TransferError::kRemoteError:
      return "peer reported an error";
    case TransferError::kIntegrityFailed:
      return "SHA-256 verification failed";
    case TransferError::kInvalidFilename:
      return "filename rejected by the receiver";
  }
  return "unknown transfer error";
}

}  // namespace swiftlink::transfer
