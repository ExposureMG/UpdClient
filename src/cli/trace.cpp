#include "cli/trace.hpp"

#include "cli/version.hpp"

#include <format>

namespace updclient::cli {

Outcome<std::unique_ptr<TraceFile>> TraceFile::open(const std::string &path, const std::string &target) {
  std::unique_ptr<TraceFile> trace(new TraceFile());
  trace->out_.open(pathFromUtf8(path), std::ios::binary | std::ios::app);
  if (!trace->out_) return usageError("cannot open the trace file '" + path + "' for writing");
  trace->start_ = std::chrono::steady_clock::now();
  const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  trace->out_ << std::format("# updclient {} XBDM trace of {}, started {:%FT%TZ}\n", kVersion, target, now)
              << "# '>' command sent, '<' line received; binary data is shown by its size only\n";
  trace->out_.flush();
  return trace;
}

TraceFile::~TraceFile() {
  flushBinary();
  out_.flush();
}

xbdm::TraceHook TraceFile::hook() {
  return [this](xbdm::TraceEvent event, std::string_view text, uint64_t bytes) { record(event, text, bytes); };
}

void TraceFile::record(xbdm::TraceEvent event, std::string_view text, uint64_t bytes) {
  switch (event) {
  case xbdm::TraceEvent::Sent:
    flushBinary();
    line('>', text);
    break;
  case xbdm::TraceEvent::Received:
    flushBinary();
    line('<', text);
    break;
  case xbdm::TraceEvent::BinarySent:
  case xbdm::TraceEvent::BinaryReceived: {
    const char direction = event == xbdm::TraceEvent::BinarySent ? '>' : '<';
    if (binaryDirection_ != direction) flushBinary();
    binaryDirection_ = direction;
    binaryBytes_ += bytes;
    break;
  }
  }
}

void TraceFile::flushBinary() {
  if (binaryDirection_ == 0) return;
  line(binaryDirection_, std::format("[{} bytes of binary data]", binaryBytes_));
  binaryDirection_ = 0;
  binaryBytes_ = 0;
}

void TraceFile::line(char direction, std::string_view text) {
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_);
  std::string printable;
  printable.reserve(text.size());
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) printable += std::format("\\x{:02x}", u);
    else printable += c;
  }
  out_ << std::format("{:10.3f} {} {}\n", elapsed.count(), direction, printable);
  out_.flush();
}

} // namespace updclient::cli
