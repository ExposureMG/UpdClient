#include <core/error.hpp>

namespace updclient {

const char *errorCodeName(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::Unknown: return "Unknown";
  case ErrorCode::InvalidArgument: return "InvalidArgument";
  case ErrorCode::Unsupported: return "Unsupported";
  case ErrorCode::NotConnected: return "NotConnected";
  case ErrorCode::ConnectFailed: return "ConnectFailed";
  case ErrorCode::Timeout: return "Timeout";
  case ErrorCode::Disconnected: return "Disconnected";
  case ErrorCode::Io: return "Io";
  case ErrorCode::Protocol: return "Protocol";
  case ErrorCode::LimitExceeded: return "LimitExceeded";
  case ErrorCode::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

std::string formatError(const Error &error) {
  std::string text = errorCodeName(error.code);
  text += ": ";
  text += error.message;
  if (error.sysError != 0) {
    text += " (os error " + std::to_string(error.sysError) + ")";
  }
  return text;
}

} // namespace updclient
