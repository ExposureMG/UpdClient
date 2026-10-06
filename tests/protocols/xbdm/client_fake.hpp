#pragma once

#include <updclient/core/error.hpp>
#include <updclient/net/transport.hpp>
#include <updclient/protocols/xbdm/client.hpp>

#include "support/test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A scripted console for the XBDM client tests. It never blocks: what the client
// writes is split into command lines (or collected as binary data while an upload
// is expected), each line goes to the handler, and the handler queues the answer.
// A read with nothing queued is a timeout (the console is silent), or end of
// stream after hangUp(). Independent of the mock server under tests/support.
namespace xt {

using ut::Bytes;

class FakeConsole : public std::enable_shared_from_this<FakeConsole> {
public:
  using Handler = std::function<void(FakeConsole &, const std::string &)>;
  using BinaryDone = std::function<void(FakeConsole &, const Bytes &)>;

  static std::shared_ptr<FakeConsole> create(bool greet = true) {
    auto console = std::shared_ptr<FakeConsole>(new FakeConsole());
    if (greet) console->line("201- connected");
    return console;
  }

  FakeConsole &send(std::string_view text) {
    output_.insert(output_.end(), text.begin(), text.end());
    return *this;
  }
  FakeConsole &send(std::span<const uint8_t> bytes) {
    output_.insert(output_.end(), bytes.begin(), bytes.end());
    return *this;
  }
  FakeConsole &line(std::string_view text) {
    send(text);
    return send(std::string_view("\r\n"));
  }
  // A 202 answer with these body lines and the closing ".".
  FakeConsole &multiline(std::initializer_list<std::string_view> body) {
    line("202- multiline response follows");
    for (auto text : body) line(text);
    return line(".");
  }
  FakeConsole &hangUp() {
    hungUp_ = true;
    return *this;
  }

  // Every command line goes to the handler. Without one, the scripted pairs from
  // on() are used in order.
  FakeConsole &handle(Handler handler) {
    handler_ = std::move(handler);
    return *this;
  }
  // Expects this command next and answers with the reply (sent verbatim; use
  // "\r\n" in it). Mismatches are recorded in problems().
  FakeConsole &on(std::string command, std::string reply) {
    script_.push_back({std::move(command), std::move(reply)});
    return *this;
  }
  // The next n bytes written are upload data; done runs when they are complete.
  void expectBinary(uint64_t n, BinaryDone done, bool keep = true) {
    binaryRemaining_ = n;
    binaryDone_ = std::move(done);
    keepBinary_ = keep;
    binary_.clear();
    if (n == 0 && binaryDone_) finishBinary();
  }

  FakeConsole &setMaxRead(size_t bytes) {
    maxRead_ = bytes;
    return *this;
  }
  // After this many bytes were accepted in total, writes fail with Disconnected and
  // reads report end of stream: the connection dropped.
  FakeConsole &dropAfterWritten(size_t bytes) {
    dropAfter_ = bytes;
    return *this;
  }

  const std::vector<std::string> &commands() const noexcept { return commands_; }
  std::string lastCommand() const { return commands_.empty() ? std::string() : commands_.back(); }
  const Bytes &binary() const noexcept { return binary_; }
  uint64_t binaryReceived() const noexcept { return binaryTotal_; }
  size_t unread() const noexcept { return output_.size(); }
  size_t written() const noexcept { return totalWritten_; }
  bool closed() const noexcept { return closed_; }
  int byes() const noexcept { return byes_; }
  std::chrono::milliseconds lastTimeout() const noexcept { return lastTimeout_; }
  std::string problems() const {
    std::string out = problems_;
    if (!script_.empty()) out += "unused scripted command '" + script_.front().first + "'; ";
    return out;
  }

  std::unique_ptr<updclient::net::ITransport> transport();

private:
  friend class FakeTransport;

  FakeConsole() = default;

  void finishBinary() {
    auto done = std::move(binaryDone_);
    binaryDone_ = nullptr;
    if (done) done(*this, binary_);
  }

  void received(std::span<const uint8_t> data) {
    input_.insert(input_.end(), data.begin(), data.end());
    for (;;) {
      if (binaryRemaining_ > 0) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(binaryRemaining_, input_.size()));
        if (n == 0) return;
        if (keepBinary_) binary_.insert(binary_.end(), input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(n));
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(n));
        binaryRemaining_ -= n;
        binaryTotal_ += n;
        if (binaryRemaining_ == 0) finishBinary();
        continue;
      }
      auto newline = std::find(input_.begin(), input_.end(), uint8_t{'\n'});
      if (newline == input_.end()) return;
      std::string text(input_.begin(), newline);
      input_.erase(input_.begin(), newline + 1);
      if (!text.empty() && text.back() == '\r') text.pop_back();
      if (!output_.empty()) problems_ += "command '" + text + "' sent before the previous answer was read; ";
      commands_.push_back(text);
      if (text == "bye") {
        ++byes_;
        line("200- bye");
        hangUp();
      } else if (handler_) {
        handler_(*this, text);
      } else if (!script_.empty()) {
        auto [expected, reply] = std::move(script_.front());
        script_.pop_front();
        if (expected != text) problems_ += "expected '" + expected + "', got '" + text + "'; ";
        send(reply);
      } else {
        problems_ += "unscripted command '" + text + "'; ";
      }
    }
  }

  std::deque<uint8_t> output_;
  std::deque<uint8_t> input_;
  bool hungUp_ = false;
  Handler handler_;
  std::deque<std::pair<std::string, std::string>> script_;
  uint64_t binaryRemaining_ = 0;
  uint64_t binaryTotal_ = 0;
  BinaryDone binaryDone_;
  bool keepBinary_ = true;
  Bytes binary_;
  size_t maxRead_ = std::numeric_limits<size_t>::max();
  size_t dropAfter_ = std::numeric_limits<size_t>::max();
  size_t totalWritten_ = 0;
  bool dropped_ = false;
  int byes_ = 0;
  std::vector<std::string> commands_;
  std::string problems_;
  std::atomic<bool> closed_{false};
  std::chrono::milliseconds lastTimeout_{0};
};

class FakeTransport final : public updclient::net::ITransport {
public:
  explicit FakeTransport(std::shared_ptr<FakeConsole> console) : console_(std::move(console)) {}

  bool isOpen() const noexcept override { return !closed_; }
  void close() noexcept override {
    closed_ = true;
    console_->closed_ = true;
  }
  std::string describe() const override { return "fake-xbdm"; }
  updclient::Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    console_->lastTimeout_ = timeout;
    return {};
  }

  updclient::Result<size_t> readSome(std::span<uint8_t> buffer) override {
    using updclient::ErrorCode;
    auto &c = *console_;
    if (closed_) return updclient::fail(ErrorCode::NotConnected, "fake transport is closed");
    if (buffer.empty()) return size_t{0};
    if (c.output_.empty()) {
      if (c.hungUp_ || c.dropped_) return size_t{0};
      return updclient::fail(ErrorCode::Timeout, "fake console is silent");
    }
    const size_t n = std::min({buffer.size(), c.output_.size(), c.maxRead_});
    std::copy_n(c.output_.begin(), n, buffer.begin());
    c.output_.erase(c.output_.begin(), c.output_.begin() + static_cast<std::ptrdiff_t>(n));
    return n;
  }

  updclient::Result<size_t> writeSome(std::span<const uint8_t> data) override {
    using updclient::ErrorCode;
    auto &c = *console_;
    if (closed_) return updclient::fail(ErrorCode::NotConnected, "fake transport is closed");
    if (c.dropped_) return updclient::fail(ErrorCode::Disconnected, "fake connection dropped");
    size_t n = data.size();
    if (c.totalWritten_ + n >= c.dropAfter_) {
      n = c.dropAfter_ - c.totalWritten_;
      c.dropped_ = true;
      c.output_.clear();
    }
    c.totalWritten_ += n;
    c.received(data.first(n));
    if (n == 0) return updclient::fail(ErrorCode::Disconnected, "fake connection dropped");
    return n;
  }

private:
  std::shared_ptr<FakeConsole> console_;
  std::atomic<bool> closed_{false};
};

inline std::unique_ptr<updclient::net::ITransport> FakeConsole::transport() {
  return std::make_unique<FakeTransport>(shared_from_this());
}

// Options with short timeouts; the fake answers at once or never.
inline updclient::xbdm::ClientOptions quickOptions() {
  updclient::xbdm::ClientOptions options;
  options.greetingTimeout = std::chrono::milliseconds(200);
  options.idleTimeout = std::chrono::milliseconds(200);
  options.slowIdleTimeout = std::chrono::milliseconds(300);
  options.commandTimeout = std::chrono::milliseconds(2000);
  options.byeTimeout = std::chrono::milliseconds(50);
  return options;
}

using ConsoleQueue = std::deque<std::shared_ptr<FakeConsole>>;

// Each connect takes the next console of the queue, as reconnect() does.
inline updclient::xbdm::XbdmClient::Connector connectorFor(std::shared_ptr<ConsoleQueue> queue) {
  return [queue]() -> updclient::Result<updclient::net::TransportPtr> {
    if (queue->empty()) return updclient::fail(updclient::ErrorCode::ConnectFailed, "no more fake consoles");
    auto console = queue->front();
    queue->pop_front();
    return console->transport();
  };
}

inline updclient::Result<updclient::xbdm::XbdmClient>
attach(const std::shared_ptr<FakeConsole> &console,
       updclient::xbdm::ClientOptions options = quickOptions(),
       updclient::xbdm::XbdmClient::Connector connector = {}) {
  return updclient::xbdm::XbdmClient::attach(console->transport(), options, std::move(connector));
}

} // namespace xt
