#include <net/transport.hpp>

#include <algorithm>
#include <utility>

namespace updclient::net {

namespace {

// The helpers checked isOpen() before their first call, so a primitive that later
// finds the transport closed means close() ran while the helper was in progress.
unexpected<Error> interrupted(Error error) {
  if (error.code == ErrorCode::NotConnected) {
    error.code = ErrorCode::Cancelled;
    error.message = "cancelled: the transport was closed";
  }
  return unexpected<Error>(std::move(error));
}

} // namespace

ITransport::~ITransport() = default;

Result<void> ITransport::writeAll(std::span<const uint8_t> data) {
  if (!isOpen()) {
    return fail(ErrorCode::NotConnected, "transport is not open");
  }
  while (!data.empty()) {
    auto written = writeSome(data);
    if (!written) return interrupted(written.error());
    if (*written == 0) {
      return fail(ErrorCode::Io, "write made no progress");
    }
    data = data.subspan(*written);
  }
  return {};
}

Result<void> ITransport::readExact(std::span<uint8_t> buffer) {
  if (!isOpen()) {
    return fail(ErrorCode::NotConnected, "transport is not open");
  }
  const size_t total = buffer.size();
  size_t received = 0;
  while (received < total) {
    auto n = readSome(buffer.subspan(received));
    if (!n) return interrupted(n.error());
    if (*n == 0) {
      return fail(ErrorCode::Disconnected, "connection closed after " + std::to_string(received) +
                                               " of " + std::to_string(total) + " bytes");
    }
    received += *n;
  }
  return {};
}

Result<std::vector<uint8_t>> ITransport::readUntilEof(size_t maxBytes) {
  if (!isOpen()) {
    return fail(ErrorCode::NotConnected, "transport is not open");
  }
  constexpr size_t kChunk = 16 * 1024;
  std::vector<uint8_t> out;
  for (;;) {
    const size_t remaining = maxBytes - out.size();
    // One byte past the limit is enough to detect overflow.
    const size_t want = std::min(kChunk, remaining == static_cast<size_t>(-1) ? remaining : remaining + 1);
    const size_t offset = out.size();
    out.resize(offset + want);
    auto n = readSome(std::span<uint8_t>(out.data() + offset, want));
    if (!n) return interrupted(n.error());
    out.resize(offset + *n);
    if (*n == 0) return out;
    if (out.size() > maxBytes) {
      return fail(ErrorCode::LimitExceeded,
                  "stream exceeded " + std::to_string(maxBytes) + " bytes");
    }
  }
}

} // namespace updclient::net
