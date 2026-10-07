#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace updclient::cli {

// While it lives, Ctrl-C (SIGINT) runs `cancel` once, on a thread of its own,
// instead of killing the process; a second Ctrl-C kills it as usual. The signal
// handler only sets a flag. Not reentrant: one scope at a time.
class InterruptScope {
public:
  explicit InterruptScope(std::function<void()> cancel);
  InterruptScope(const InterruptScope &) = delete;
  InterruptScope &operator=(const InterruptScope &) = delete;
  ~InterruptScope();

  bool interrupted() const noexcept;

private:
  std::function<void()> cancel_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread watcher_;
  void (*previous_)(int) = nullptr;
};

} // namespace updclient::cli
