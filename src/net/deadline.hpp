#pragma once

// Private to the library. Adding an unbounded caller-supplied timeout to
// steady_clock::now() overflows the clock's tick count, so every deadline is
// computed here, with the wait capped at Endpoint::kMaxTimeout.

#include <net/endpoint.hpp>

#include <algorithm>
#include <chrono>

namespace updclient::net {

inline std::chrono::milliseconds boundedWait(std::chrono::milliseconds timeout) noexcept {
  return std::clamp(timeout, std::chrono::milliseconds(0), Endpoint::kMaxTimeout);
}

inline std::chrono::steady_clock::time_point deadlineAfter(std::chrono::milliseconds timeout) {
  return std::chrono::steady_clock::now() + boundedWait(timeout);
}

} // namespace updclient::net
