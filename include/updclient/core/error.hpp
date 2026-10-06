#pragma once

#include <updclient/core/expected.hpp>

#include <string>
#include <utility>

namespace updclient {

enum class ErrorCode {
  Unknown,
  InvalidArgument,
  Unsupported,
  NotConnected,
  ConnectFailed,
  Timeout,
  Disconnected,
  Io,
  Protocol,
  LimitExceeded
};

struct Error {
  ErrorCode code = ErrorCode::Unknown;
  std::string message;
  int sysError = 0;
};

template <class T> using Result = expected<T, Error>;

inline Error makeError(ErrorCode code, std::string message, int sysError = 0) {
  return Error{code, std::move(message), sysError};
}

inline unexpected<Error> fail(ErrorCode code, std::string message, int sysError = 0) {
  return unexpected<Error>(makeError(code, std::move(message), sysError));
}

inline const char *errorCodeName(ErrorCode code) noexcept {
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
  }
  return "Unknown";
}

inline std::string formatError(const Error &error) {
  std::string text = errorCodeName(error.code);
  text += ": ";
  text += error.message;
  if (error.sysError != 0) {
    text += " (os error " + std::to_string(error.sysError) + ")";
  }
  return text;
}

} // namespace updclient
