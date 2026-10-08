#pragma once

#include <core/expected.hpp>
#include <core/export.hpp>

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
  LimitExceeded,
  // An operation in progress was abandoned because the transport was closed
  // locally, typically by close() from another thread.
  Cancelled,
  // The name an operation would create or take is already in use (a file that
  // appeared during an upload, the new name of a rename).
  AlreadyExists
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

UPDCLIENT_API const char *errorCodeName(ErrorCode code) noexcept;

// "Code: message" plus the OS error number when there is one.
UPDCLIENT_API std::string formatError(const Error &error);

} // namespace updclient
