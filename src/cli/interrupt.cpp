#include "cli/interrupt.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>

namespace updclient::cli {

namespace {

std::atomic<bool> gInterrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the signal handler needs a lock-free flag");

void onInterrupt(int) {
  gInterrupted.store(true);
  std::signal(SIGINT, SIG_DFL);
}

} // namespace

InterruptScope::InterruptScope(std::function<void()> cancel) : cancel_(std::move(cancel)) {
  gInterrupted.store(false);
  previous_ = std::signal(SIGINT, onInterrupt);
  if (previous_ == SIG_ERR) previous_ = SIG_DFL;
  // Polled, because a signal handler may not touch a condition variable.
  watcher_ = std::thread([this] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      if (gInterrupted.load()) {
        lock.unlock();
        spdlog::warn("Interrupted; cancelling (press Ctrl-C again to quit at once)");
        if (cancel_) cancel_();
        return;
      }
      wake_.wait_for(lock, std::chrono::milliseconds(50));
    }
  });
}

InterruptScope::~InterruptScope() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (watcher_.joinable()) watcher_.join();
  std::signal(SIGINT, previous_);
}

bool InterruptScope::interrupted() const noexcept {
  return gInterrupted.load();
}

} // namespace updclient::cli
