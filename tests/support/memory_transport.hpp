#pragma once

#include <core/error.hpp>
#include <net/transport.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace ut {

class MemoryTransport;

// Two connected in-memory transports: what one end writes the other reads. Unlike
// MockTransport they block. A read waits for data, a write waits for room (each
// direction holds at most `capacity` bytes), both bounded by the end's timeout
// (zero waits forever). They follow the whole ITransport contract, close() from
// another thread included, so a protocol client can talk to a mock server running
// on another thread with no socket at all.
//
// Closing one end is like a TCP close: the other end still reads what was already
// written, then end of stream, and its writes fail with Disconnected.
struct MemoryPipe {
  std::unique_ptr<MemoryTransport> client;
  std::unique_ptr<MemoryTransport> server;

  static MemoryPipe create(size_t capacity = 64 * 1024,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
};

class MemoryTransport final : public updclient::net::ITransport {
public:
  struct Link {
    explicit Link(size_t bytes) : capacity(std::max<size_t>(bytes, 1)) {}

    std::mutex mutex;
    std::condition_variable changed;
    // queue[i] carries the bytes written by end i.
    std::deque<uint8_t> queue[2];
    bool closed[2] = {false, false};
    size_t capacity;
    size_t blocked = 0;
  };

  MemoryTransport(std::shared_ptr<Link> link, int side, std::chrono::milliseconds timeout)
      : link_(std::move(link)), side_(side), timeout_(timeout) {}

  bool isOpen() const noexcept override {
    std::lock_guard<std::mutex> lock(link_->mutex);
    return !link_->closed[side_];
  }

  void close() noexcept override {
    {
      std::lock_guard<std::mutex> lock(link_->mutex);
      link_->closed[side_] = true;
    }
    link_->changed.notify_all();
  }

  std::string describe() const override { return side_ == 0 ? "memory://client" : "memory://server"; }

  updclient::Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    using updclient::ErrorCode;
    if (!isOpen()) return updclient::fail(ErrorCode::NotConnected, "memory transport is closed");
    if (timeout.count() < 0) return updclient::fail(ErrorCode::InvalidArgument, "negative timeout");
    timeout_ = timeout;
    return {};
  }

  updclient::Result<size_t> readSome(std::span<uint8_t> buffer) override {
    using updclient::ErrorCode;
    std::unique_lock<std::mutex> lock(link_->mutex);
    if (link_->closed[side_]) return updclient::fail(ErrorCode::NotConnected, "memory transport is closed");
    if (buffer.empty()) return size_t{0};

    auto &in = link_->queue[1 - side_];
    const bool ready = waitLocked(lock, [&] { return link_->closed[side_] || !in.empty() || link_->closed[1 - side_]; });
    if (link_->closed[side_]) return updclient::fail(ErrorCode::Cancelled, "memory read cancelled");
    if (!in.empty()) {
      const size_t n = std::min(buffer.size(), in.size());
      std::copy_n(in.begin(), n, buffer.begin());
      in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(n));
      lock.unlock();
      link_->changed.notify_all();
      return n;
    }
    if (!ready) return updclient::fail(ErrorCode::Timeout, "memory read timed out");
    return size_t{0};
  }

  updclient::Result<size_t> writeSome(std::span<const uint8_t> data) override {
    using updclient::ErrorCode;
    std::unique_lock<std::mutex> lock(link_->mutex);
    if (link_->closed[side_]) return updclient::fail(ErrorCode::NotConnected, "memory transport is closed");
    if (data.empty()) return size_t{0};

    auto &out = link_->queue[side_];
    const bool ready = waitLocked(lock, [&] {
      return link_->closed[side_] || link_->closed[1 - side_] || out.size() < link_->capacity;
    });
    if (link_->closed[side_]) return updclient::fail(ErrorCode::Cancelled, "memory write cancelled");
    if (link_->closed[1 - side_]) return updclient::fail(ErrorCode::Disconnected, "memory peer closed");
    if (!ready) return updclient::fail(ErrorCode::Timeout, "memory write timed out");
    const size_t n = std::min(data.size(), link_->capacity - out.size());
    out.insert(out.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
    lock.unlock();
    link_->changed.notify_all();
    return n;
  }

  // Calls, on either end, currently waiting for data, room or close(). Lets a test
  // close only once the other thread is really blocked.
  size_t blockedCalls() const {
    std::lock_guard<std::mutex> lock(link_->mutex);
    return link_->blocked;
  }

  // Polls blockedCalls() until it reaches count; false if that takes longer than limit.
  bool waitUntilBlocked(size_t count = 1, std::chrono::milliseconds limit = std::chrono::seconds(5)) const {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (blockedCalls() < count) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

private:
  template <class Pred> bool waitLocked(std::unique_lock<std::mutex> &lock, Pred pred) {
    if (pred()) return true;
    ++link_->blocked;
    bool ready = true;
    if (timeout_.count() == 0) {
      link_->changed.wait(lock, pred);
    } else {
      ready = link_->changed.wait_for(lock, timeout_, pred);
    }
    --link_->blocked;
    return ready;
  }

  std::shared_ptr<Link> link_;
  int side_;
  std::chrono::milliseconds timeout_;
};

inline MemoryPipe MemoryPipe::create(size_t capacity, std::chrono::milliseconds timeout) {
  auto link = std::make_shared<MemoryTransport::Link>(capacity);
  MemoryPipe pipe;
  pipe.client = std::make_unique<MemoryTransport>(link, 0, timeout);
  pipe.server = std::make_unique<MemoryTransport>(link, 1, timeout);
  return pipe;
}

} // namespace ut
