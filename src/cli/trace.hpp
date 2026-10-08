#pragma once

#include "cli/context.hpp"

#include <protocols/jrpc/client.hpp>
#include <protocols/xbdm/client.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace updclient::cli {

// --trace FILE: appends one line per XBDM or JRPC command sent ("> ") and per text
// line received ("< "), with the milliseconds since the session started. Nothing is
// redacted. Binary data (file contents, memory blocks, frame buffers) is never
// written; a run of it appears as one line giving its size.
class TraceFile {
public:
  // protocol names the session in the file header ("XBDM", "JRPC").
  static Outcome<std::unique_ptr<TraceFile>> open(const std::string &path, const std::string &target,
                                                  std::string_view protocol = "XBDM");
  TraceFile(const TraceFile &) = delete;
  TraceFile &operator=(const TraceFile &) = delete;
  ~TraceFile();

  xbdm::TraceHook hook();
  jrpc::TraceHook jrpcHook();

private:
  TraceFile() = default;
  void record(xbdm::TraceEvent event, std::string_view text, uint64_t bytes);
  void recordJrpc(jrpc::TraceEvent event, std::string_view text);
  void flushBinary();
  void line(char direction, std::string_view text);

  std::ofstream out_;
  std::chrono::steady_clock::time_point start_;
  char binaryDirection_ = 0;
  uint64_t binaryBytes_ = 0;
};

} // namespace updclient::cli
