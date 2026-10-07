#pragma once

#include <core/error.hpp>
#include <net/transport.hpp>

#include "support/test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ut {

class MockTransport;

// The script behind a MockTransport: canned reads, optional expected writes, fault
// injection and a record of what the code under test did. It is shared by
// shared_ptr, so a test keeps inspecting it after the transport moved into a client.
//
// Reads are a queue of steps. A data step is never merged with its neighbour, so
// each step boundary is a short read, and setMaxRead() splits steps further. When the
// queue runs dry the stream reports end-of-file (or a timeout, see setTimeoutWhenDrained).
class MockScript : public std::enable_shared_from_this<MockScript> {
public:
  static std::shared_ptr<MockScript> create() { return std::shared_ptr<MockScript>(new MockScript()); }

  MockScript &reply(std::span<const uint8_t> data) {
    if (!data.empty()) reads_.push_back(ReadStep{ReadStep::Data, Bytes(data.begin(), data.end()), {}});
    return *this;
  }
  MockScript &reply(const Bytes &data) { return reply(std::span<const uint8_t>(data)); }
  MockScript &reply(std::string_view text) { return reply(bytesOf(text)); }
  MockScript &reply(const char *text) { return reply(std::string_view(text)); }

  // The data is delivered as separate steps of at most chunkSize bytes.
  MockScript &replyChunked(std::span<const uint8_t> data, size_t chunkSize) {
    for (size_t pos = 0; pos < data.size(); pos += chunkSize) {
      reply(data.subspan(pos, std::min(chunkSize, data.size() - pos)));
    }
    return *this;
  }
  MockScript &replyChunked(const Bytes &data, size_t chunkSize) {
    return replyChunked(std::span<const uint8_t>(data), chunkSize);
  }

  MockScript &replyTimeout() {
    reads_.push_back(ReadStep{ReadStep::Timeout, {}, {}});
    return *this;
  }
  MockScript &replyEof() {
    reads_.push_back(ReadStep{ReadStep::Eof, {}, {}});
    return *this;
  }
  MockScript &replyError(updclient::Error error) {
    reads_.push_back(ReadStep{ReadStep::Error, {}, std::move(error)});
    return *this;
  }

  // Upper bound for the size of every read / write, to force short IO.
  MockScript &setMaxRead(size_t bytes) {
    maxRead_ = bytes;
    return *this;
  }
  MockScript &setMaxWrite(size_t bytes) {
    maxWrite_ = bytes;
    return *this;
  }
  // false: an exhausted script times out instead of reporting end-of-file.
  MockScript &setEofWhenDrained(bool eof) {
    eofWhenDrained_ = eof;
    return *this;
  }

  // Appends to the stream of bytes the code under test is expected to write. Once
  // any expectation exists, a write that deviates is rejected with an Io error and
  // recorded; problems() describes it.
  MockScript &expectWrite(std::span<const uint8_t> data) {
    expectActive_ = true;
    expected_.insert(expected_.end(), data.begin(), data.end());
    return *this;
  }
  MockScript &expectWrite(const Bytes &data) { return expectWrite(std::span<const uint8_t>(data)); }
  MockScript &expectWrite(std::string_view text) { return expectWrite(bytesOf(text)); }
  MockScript &expectWrite(const char *text) { return expectWrite(std::string_view(text)); }

  // After this many bytes were accepted in total, every write fails with error.
  MockScript &failWritesAfter(size_t acceptedBytes, updclient::Error error) {
    failAfter_ = acceptedBytes;
    writeError_ = std::move(error);
    return *this;
  }
  MockScript &failWrites(updclient::Error error) { return failWritesAfter(0, std::move(error)); }
  // writeSome reports zero bytes written (no progress).
  MockScript &stallWrites(bool stall = true) {
    stallWrites_ = stall;
    return *this;
  }

  const Bytes &written() const noexcept { return written_; }
  std::string writtenText() const { return textOf(written_); }
  size_t readCalls() const noexcept { return readCalls_; }
  size_t writeCalls() const noexcept { return writeCalls_; }
  bool closed() const noexcept { return closed_; }
  std::chrono::milliseconds lastTimeout() const noexcept { return lastTimeout_; }
  bool timeoutWasSet() const noexcept { return timeoutSet_; }

  // Bytes of queued data that were never read.
  size_t unreadBytes() const noexcept {
    size_t total = 0;
    for (const auto &step : reads_) {
      if (step.kind == ReadStep::Data) total += step.data.size() - step.offset;
    }
    return total;
  }
  bool readsDrained() const noexcept { return reads_.empty(); }

  // Empty when every write matched the expectations and all expected bytes arrived.
  std::string problems() const {
    if (!mismatch_.empty()) return mismatch_;
    if (expectActive_ && written_.size() < expected_.size()) {
      return "only " + std::to_string(written_.size()) + " of " + std::to_string(expected_.size()) +
             " expected bytes were written";
    }
    return {};
  }

  std::unique_ptr<MockTransport> transport();

private:
  friend class MockTransport;

  struct ReadStep {
    enum Kind { Data, Timeout, Eof, Error } kind;
    Bytes data;
    updclient::Error error;
    size_t offset = 0;
  };

  MockScript() = default;

  std::deque<ReadStep> reads_;
  size_t maxRead_ = std::numeric_limits<size_t>::max();
  size_t maxWrite_ = std::numeric_limits<size_t>::max();
  bool eofWhenDrained_ = true;

  Bytes written_;
  Bytes expected_;
  bool expectActive_ = false;
  std::string mismatch_;
  size_t failAfter_ = std::numeric_limits<size_t>::max();
  updclient::Error writeError_;
  bool stallWrites_ = false;

  size_t readCalls_ = 0;
  size_t writeCalls_ = 0;
  std::atomic<bool> closed_{false};
  bool timeoutSet_ = false;
  std::chrono::milliseconds lastTimeout_{0};
};

// close() may be called from another thread. Reads and writes never block, so
// there is nothing for it to wake: a call that starts after it fails with NotConnected.
// For a transport that blocks, and so for cancellation tests, use MemoryPipe.
class MockTransport final : public updclient::net::ITransport {
public:
  explicit MockTransport(std::shared_ptr<MockScript> script) : script_(std::move(script)) {}

  bool isOpen() const noexcept override { return !script_->closed_; }
  void close() noexcept override { script_->closed_ = true; }
  std::string describe() const override { return "mock"; }

  updclient::Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    script_->timeoutSet_ = true;
    script_->lastTimeout_ = timeout;
    return {};
  }

  updclient::Result<size_t> readSome(std::span<uint8_t> buffer) override {
    using updclient::ErrorCode;
    auto &s = *script_;
    if (s.closed_) return updclient::fail(ErrorCode::NotConnected, "mock transport is closed");
    ++s.readCalls_;
    if (buffer.empty()) return size_t{0};

    for (;;) {
      if (s.reads_.empty()) {
        if (s.eofWhenDrained_) return size_t{0};
        return updclient::fail(ErrorCode::Timeout, "mock read timed out (script drained)");
      }
      auto &step = s.reads_.front();
      switch (step.kind) {
      case MockScript::ReadStep::Data: {
        const size_t available = step.data.size() - step.offset;
        const size_t n = std::min({buffer.size(), available, s.maxRead_});
        std::copy_n(step.data.begin() + static_cast<std::ptrdiff_t>(step.offset), n, buffer.begin());
        step.offset += n;
        if (step.offset == step.data.size()) s.reads_.pop_front();
        return n;
      }
      case MockScript::ReadStep::Timeout:
        s.reads_.pop_front();
        return updclient::fail(ErrorCode::Timeout, "mock read timed out");
      case MockScript::ReadStep::Eof:
        s.reads_.pop_front();
        return size_t{0};
      case MockScript::ReadStep::Error: {
        updclient::Error error = std::move(step.error);
        s.reads_.pop_front();
        return updclient::unexpected<updclient::Error>(std::move(error));
      }
      }
    }
  }

  updclient::Result<size_t> writeSome(std::span<const uint8_t> data) override {
    using updclient::ErrorCode;
    auto &s = *script_;
    if (s.closed_) return updclient::fail(ErrorCode::NotConnected, "mock transport is closed");
    ++s.writeCalls_;
    if (data.empty()) return size_t{0};
    if (s.stallWrites_) return size_t{0};

    if (s.written_.size() >= s.failAfter_) return updclient::unexpected<updclient::Error>(s.writeError_);
    size_t n = std::min(data.size(), s.maxWrite_);
    n = std::min(n, s.failAfter_ - s.written_.size());

    const size_t offset = s.written_.size();
    s.written_.insert(s.written_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));

    if (s.expectActive_ && s.mismatch_.empty()) {
      for (size_t i = 0; i < n; ++i) {
        const size_t pos = offset + i;
        if (pos >= s.expected_.size()) {
          s.mismatch_ = "unexpected extra write at offset " + std::to_string(pos);
          break;
        }
        if (s.expected_[pos] != data[i]) {
          s.mismatch_ = "write differs from expectation at offset " + std::to_string(pos);
          break;
        }
      }
      if (!s.mismatch_.empty()) return updclient::fail(ErrorCode::Io, "mock: " + s.mismatch_);
    }
    return n;
  }

private:
  std::shared_ptr<MockScript> script_;
};

inline std::unique_ptr<MockTransport> MockScript::transport() {
  return std::make_unique<MockTransport>(shared_from_this());
}

} // namespace ut
