#pragma once

#include <core/error.hpp>
#include <net/transport.hpp>
#include <protocols/jrpc/client.hpp>

#include "support/test_util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A scripted console for the JRPC client tests. It never blocks: what the client
// writes is split into command lines at each LF, every line goes to the handler (or
// the next scripted pair), and the answer is queued. A read with nothing queued is a
// timeout (the console is silent), or end of stream after hangUp(). Independent of
// the mock server under tests/support.
namespace jt {

using ut::Bytes;

inline constexpr std::string_view kBannerLine = "JRPC2 connected\r\n";

class FakeConsole : public std::enable_shared_from_this<FakeConsole> {
public:
  using Handler = std::function<void(FakeConsole &, const std::string &)>;

  // With a banner, "JRPC2 connected" and CR LF are queued at once, as the server sends
  // them on accept.
  static std::shared_ptr<FakeConsole> create(bool banner = true) {
    auto console = std::shared_ptr<FakeConsole>(new FakeConsole());
    if (banner) console->send(kBannerLine);
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
  // text and CR LF
  FakeConsole &line(std::string_view text) {
    send(text);
    return send(std::string_view("\r\n"));
  }
  FakeConsole &hangUp() {
    hungUp_ = true;
    return *this;
  }

  // Every command line goes to the handler. Without one, the scripted pairs from on()
  // are used in order.
  FakeConsole &handle(Handler handler) {
    handler_ = std::move(handler);
    return *this;
  }
  // Expects this command next and answers with the reply (sent verbatim; use "\r\n" in
  // it, or an empty reply for a command the console does not answer). Mismatches are
  // recorded in problems().
  FakeConsole &on(std::string command, std::string reply) {
    script_.push_back({std::move(command), std::move(reply)});
    return *this;
  }

  // No read returns more than this many bytes.
  FakeConsole &setMaxRead(size_t bytes) {
    maxRead_ = bytes;
    return *this;
  }
  // The next reads return at most these many bytes each, in order (a read that finds
  // nothing does not use one up); after that, setMaxRead() applies. planReads({k})
  // splits the next data after its first k bytes.
  FakeConsole &planReads(std::initializer_list<size_t> sizes) {
    plan_.assign(sizes.begin(), sizes.end());
    return *this;
  }
  // After this many bytes were accepted in total, writes fail with Disconnected and
  // reads report end of stream: the connection dropped.
  FakeConsole &dropAfterWritten(size_t bytes) {
    dropAfter_ = bytes;
    return *this;
  }

  // The command lines without CR LF, in order; "Bye" is not one of them.
  const std::vector<std::string> &commands() const noexcept { return commands_; }
  std::string lastCommand() const { return commands_.empty() ? std::string() : commands_.back(); }
  // Every byte the client wrote, in order.
  const std::string &wire() const noexcept { return wire_; }
  size_t unread() const noexcept { return output_.size(); }
  size_t written() const noexcept { return wire_.size(); }
  bool closed() const noexcept { return closed_; }
  // "Bye" lines received.
  int byes() const noexcept { return byes_; }
  std::chrono::milliseconds lastTimeout() const noexcept { return timeouts_.empty() ? std::chrono::milliseconds(0) : timeouts_.back(); }
  // Every timeout the client set, in order.
  const std::vector<std::chrono::milliseconds> &timeouts() const noexcept { return timeouts_; }
  std::string problems() const {
    std::string out = problems_;
    if (!script_.empty()) out += "unused scripted command '" + script_.front().first + "'; ";
    return out;
  }

  std::unique_ptr<updclient::net::ITransport> transport();

private:
  friend class FakeTransport;

  FakeConsole() = default;

  void received(std::span<const uint8_t> data) {
    wire_.append(data.begin(), data.end());
    input_.insert(input_.end(), data.begin(), data.end());
    for (;;) {
      auto newline = std::find(input_.begin(), input_.end(), uint8_t{'\n'});
      if (newline == input_.end()) return;
      std::string text(input_.begin(), newline);
      input_.erase(input_.begin(), newline + 1);
      if (!text.empty() && text.back() == '\r') text.pop_back();
      if (text == "Bye") {
        ++byes_;
        hangUp();
        continue;
      }
      if (!output_.empty()) problems_ += "command '" + text + "' sent before the previous answer was read; ";
      commands_.push_back(text);
      if (handler_) {
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
  size_t maxRead_ = std::numeric_limits<size_t>::max();
  std::deque<size_t> plan_;
  size_t dropAfter_ = std::numeric_limits<size_t>::max();
  bool dropped_ = false;
  int byes_ = 0;
  std::vector<std::string> commands_;
  std::string wire_;
  std::string problems_;
  std::atomic<bool> closed_{false};
  std::vector<std::chrono::milliseconds> timeouts_;
};

class FakeTransport final : public updclient::net::ITransport {
public:
  explicit FakeTransport(std::shared_ptr<FakeConsole> console) : console_(std::move(console)) {}

  bool isOpen() const noexcept override { return !closed_; }
  void close() noexcept override {
    closed_ = true;
    console_->closed_ = true;
  }
  std::string describe() const override { return "fake-jrpc"; }
  updclient::Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    console_->timeouts_.push_back(timeout);
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
    size_t limit = c.maxRead_;
    if (!c.plan_.empty()) {
      limit = std::min(limit, c.plan_.front());
      c.plan_.pop_front();
    }
    const size_t n = std::min({buffer.size(), c.output_.size(), limit});
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
    if (c.wire_.size() + n >= c.dropAfter_) {
      n = c.dropAfter_ - c.wire_.size();
      c.dropped_ = true;
      c.output_.clear();
    }
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
inline updclient::jrpc::ClientOptions quickOptions() {
  updclient::jrpc::ClientOptions options;
  options.bannerTimeout = std::chrono::milliseconds(200);
  options.idleTimeout = std::chrono::milliseconds(200);
  options.callTimeout = std::chrono::milliseconds(2000);
  options.byeTimeout = std::chrono::milliseconds(50);
  return options;
}

using ConsoleQueue = std::deque<std::shared_ptr<FakeConsole>>;

// Each connect takes the next console of the queue, as reconnect() does.
inline updclient::jrpc::JrpcClient::Connector connectorFor(std::shared_ptr<ConsoleQueue> queue) {
  return [queue]() -> updclient::Result<updclient::net::TransportPtr> {
    if (queue->empty()) return updclient::fail(updclient::ErrorCode::ConnectFailed, "no more fake consoles");
    auto console = queue->front();
    queue->pop_front();
    return console->transport();
  };
}

inline updclient::Result<updclient::jrpc::JrpcClient>
attach(const std::shared_ptr<FakeConsole> &console, updclient::jrpc::ClientOptions options = quickOptions(),
       updclient::jrpc::JrpcClient::Connector connector = {}) {
  return updclient::jrpc::JrpcClient::attach(console->transport(), std::move(options), std::move(connector));
}

// A client on the console; a failure to connect aborts the test.
inline updclient::jrpc::JrpcClient connected(const std::shared_ptr<FakeConsole> &console,
                                             updclient::jrpc::ClientOptions options = quickOptions(),
                                             updclient::jrpc::JrpcClient::Connector connector = {}) {
  auto client = attach(console, std::move(options), std::move(connector));
  if (!client) throw std::runtime_error("attach failed: " + updclient::formatError(client.error()));
  return std::move(*client);
}

// A call by address with the given return kind and arguments.
inline updclient::jrpc::CallSpec callAt(uint32_t address, updclient::jrpc::ReturnKind returns = updclient::jrpc::ReturnKind::Void,
                                        std::vector<updclient::jrpc::Arg> args = {}, size_t arraySize = 0) {
  updclient::jrpc::CallSpec spec;
  spec.target = address;
  spec.returns = returns;
  spec.arraySize = arraySize;
  spec.args = std::move(args);
  return spec;
}

// The document's example 7.2: int g(int a = 5, int b = 0x10) at 0x82010000.
inline constexpr std::string_view kIntCommand = R"JR(consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\")JR";

inline updclient::jrpc::CallSpec intCall() {
  using updclient::jrpc::Arg;
  return callAt(0x82010000, updclient::jrpc::ReturnKind::Int, {Arg::i32(5), Arg::i32(16)});
}

} // namespace jt
