#pragma once

#include <core/error.hpp>
#include <net/datagram.hpp>

#include "support/test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ut {

// Scripted datagram source. Shared by shared_ptr so tests can inspect it after the
// code under test owns the socket. When the script is exhausted receive() behaves
// like a quiet network: it waits a moment and reports "nothing arrived".
class FakeDatagrams : public std::enable_shared_from_this<FakeDatagrams> {
public:
  static std::shared_ptr<FakeDatagrams> create() { return std::shared_ptr<FakeDatagrams>(new FakeDatagrams()); }

  FakeDatagrams &push(Bytes data, std::string sender, uint16_t senderPort = 4000) {
    updclient::net::Datagram datagram;
    datagram.data = std::move(data);
    datagram.senderAddress = std::move(sender);
    datagram.senderPort = senderPort;
    steps_.push_back(Step{std::move(datagram), std::nullopt, false});
    return *this;
  }
  FakeDatagrams &pushNothing() {
    steps_.push_back(Step{std::nullopt, std::nullopt, true});
    return *this;
  }
  FakeDatagrams &pushError(updclient::Error error) {
    steps_.push_back(Step{std::nullopt, std::move(error), false});
    return *this;
  }
  FakeDatagrams &failBind(updclient::Error error) {
    bindError_ = std::move(error);
    return *this;
  }

  int socketsCreated() const noexcept { return socketsCreated_; }
  int bindCalls() const noexcept { return bindCalls_; }
  int receiveCalls() const noexcept { return receiveCalls_; }
  uint16_t boundPort() const noexcept { return boundPort_; }
  bool boundWithReuse() const noexcept { return boundReuse_; }
  bool closed() const noexcept { return closed_; }
  size_t pendingSteps() const noexcept { return steps_.size(); }

  updclient::net::DatagramSocketFactory factory();

private:
  friend class FakeDatagramSocket;

  struct Step {
    std::optional<updclient::net::Datagram> datagram;
    std::optional<updclient::Error> error;
    bool nothing;
  };

  FakeDatagrams() = default;

  std::deque<Step> steps_;
  std::optional<updclient::Error> bindError_;
  int socketsCreated_ = 0;
  int bindCalls_ = 0;
  int receiveCalls_ = 0;
  uint16_t boundPort_ = 0;
  bool boundReuse_ = false;
  bool closed_ = false;
};

class FakeDatagramSocket final : public updclient::net::IDatagramSocket {
public:
  explicit FakeDatagramSocket(std::shared_ptr<FakeDatagrams> state) : state_(std::move(state)) {}

  updclient::Result<void> bind(uint16_t port, bool reuse) override {
    ++state_->bindCalls_;
    state_->boundPort_ = port;
    state_->boundReuse_ = reuse;
    if (state_->bindError_) return updclient::unexpected<updclient::Error>(*state_->bindError_);
    bound_ = true;
    return {};
  }

  updclient::Result<std::optional<updclient::net::Datagram>> receive(std::chrono::milliseconds timeout) override {
    ++state_->receiveCalls_;
    if (!bound_) return updclient::fail(updclient::ErrorCode::NotConnected, "fake socket is not bound");
    if (state_->steps_.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
      return std::optional<updclient::net::Datagram>{};
    }
    auto step = std::move(state_->steps_.front());
    state_->steps_.pop_front();
    if (step.error) return updclient::unexpected<updclient::Error>(std::move(*step.error));
    if (step.nothing || !step.datagram) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
      return std::optional<updclient::net::Datagram>{};
    }
    return std::optional<updclient::net::Datagram>(std::move(*step.datagram));
  }

  void close() noexcept override {
    state_->closed_ = true;
    bound_ = false;
  }

private:
  std::shared_ptr<FakeDatagrams> state_;
  bool bound_ = false;
};

inline updclient::net::DatagramSocketFactory FakeDatagrams::factory() {
  auto self = shared_from_this();
  return [self]() -> std::unique_ptr<updclient::net::IDatagramSocket> {
    ++self->socketsCreated_;
    return std::make_unique<FakeDatagramSocket>(self);
  };
}

// An UpdServer announcement: 'NSvr' followed by the four octets of the console address.
inline Bytes announcement(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return Bytes{'N', 'S', 'v', 'r', a, b, c, d};
}

} // namespace ut
