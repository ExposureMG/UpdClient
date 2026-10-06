#pragma once

#include <spdlog/spdlog.h>

#include <cstddef>
#include <string>

namespace updclient::cli {

// Throttled progress lines on stderr: one per 10 percent, or per 16 MiB when the
// total is unknown.
class Progress {
public:
  explicit Progress(std::string label) : label_(std::move(label)) {}

  void update(size_t done, size_t total) {
    if (total > 0) {
      const size_t step = (done * 10) / total;
      if (step == lastStep_ && done < total) return;
      lastStep_ = step;
      spdlog::info("{}: {}% ({}/{} bytes)", label_, done * 100 / total, done, total);
    } else {
      constexpr size_t kStep = 16u * 1024u * 1024u;
      const size_t step = done / kStep;
      if (step == lastStep_) return;
      lastStep_ = step;
      spdlog::info("{}: {} bytes", label_, done);
    }
  }

private:
  std::string label_;
  size_t lastStep_ = static_cast<size_t>(-1);
};

} // namespace updclient::cli
