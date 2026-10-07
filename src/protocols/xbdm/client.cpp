#include <protocols/xbdm/client.hpp>

#include <core/hex.hpp>
#include <net/tcp_transport.hpp>

#include "net/deadline.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <random>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <utility>

namespace updclient::xbdm {

namespace detail {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr size_t kReadChunkBytes = 4096;
constexpr uint64_t kAddressSpace = uint64_t{1} << 32;
constexpr std::string_view kAnswered = "console answered ";
// The greeting, the answer to bye and the status after sendfile data are short
// fixed phrases.
constexpr size_t kMaxShortLineBytes = 512;
// What a body line counts against maxBodyBytes at least: an empty line costs
// about this much memory once it is parsed and kept.
constexpr size_t kMinBodyLineCharge = 64;

std::span<const uint8_t> bytesOf(std::string_view text) {
  return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

// Console text goes into error messages; keep it printable.
std::string printable(std::string_view text) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) {
      out += "\\x";
      out += digits[u >> 4];
      out += digits[u & 0xF];
    } else {
      out += c;
    }
  }
  return out;
}

std::string preview(std::string_view text) {
  constexpr size_t kMaxShown = 80;
  if (text.size() <= kMaxShown) return printable(text);
  return printable(text.substr(0, kMaxShown)) + "...";
}

Error withContext(Error error, std::string_view context) {
  error.message = std::string(context) + ": " + error.message;
  return error;
}

Error refusal(const StatusLine &status, std::string_view context) {
  ErrorCode code = ErrorCode::Io;
  if (status.code == status::kMaxConnections || status.code == status::kClockNotSet ||
      status.code == status::kLineTooLong) {
    code = ErrorCode::LimitExceeded;
  } else if (status.code == status::kInvalidCommand) {
    code = ErrorCode::Unsupported;
  }
  std::string message = std::string(context) + ": " + std::string(kAnswered) + std::to_string(status.code) + "- " +
                        preview(status.text);
  return makeError(code, std::move(message), status.code);
}

// A command line: the name and flags in lower case, numbers as 0x hex, strings
// quoted after the checks of quoteValue.
class Command {
public:
  explicit Command(std::string_view name) : line_(name) {}

  Command &flag(std::string_view name) {
    line_.push_back(' ');
    line_.append(name);
    return *this;
  }

  Command &number(std::string_view key, uint64_t value) {
    line_.push_back(' ');
    line_.append(key);
    line_.push_back('=');
    line_.append(formatNumber(value));
    return *this;
  }

  Command &text(std::string_view key, std::string_view value) {
    if (error_) return *this;
    auto quoted = quoteValue(value);
    if (!quoted) {
      error_ = quoted.error();
      return *this;
    }
    line_.push_back(' ');
    line_.append(key);
    line_.push_back('=');
    line_.append(*quoted);
    return *this;
  }

  // Already validated: hex digits only.
  Command &raw(std::string_view key, std::string_view value) {
    line_.push_back(' ');
    line_.append(key);
    line_.push_back('=');
    line_.append(value);
    return *this;
  }

  Result<std::string> finish(size_t maxBytes) {
    if (error_) return unexpected<Error>(*error_);
    if (line_.size() + 2 > maxBytes) {
      return fail(ErrorCode::LimitExceeded, "command line of " + std::to_string(line_.size() + 2) +
                                                " bytes exceeds " + std::to_string(maxBytes));
    }
    return std::move(line_);
  }

private:
  std::string line_;
  std::optional<Error> error_;
};

// The stop token the connector built by XbdmClient::connect() uses for its next
// connect: the caller's for the first one, the session's own for a reconnect().
struct ConnectSlot {
  std::mutex mutex;
  std::stop_token token;

  void set(std::stop_token next) {
    std::lock_guard<std::mutex> lock(mutex);
    token = std::move(next);
  }
  std::stop_token get() {
    std::lock_guard<std::mutex> lock(mutex);
    return token;
  }
};

struct Session {
  explicit Session(ClientOptions opts, XbdmClient::Connector conn)
      : options(std::move(opts)), connector(std::move(conn)) {}
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  // The last owner, client or transfer, ends the connection.
  ~Session() { closeConnection(); }

  ClientOptions options;
  XbdmClient::Connector connector;

  // Guards the pointer against cancel() on another thread; the owning thread
  // changes it only under the lock and uses it without.
  mutable std::mutex transportMutex;
  net::TransportPtr transport;
  std::atomic<bool> cancelled{false};
  // Counts cancel() calls, so that a reconnect() can tell one that came while it
  // was connecting.
  std::atomic<uint64_t> cancelEpoch{0};
  // Set when the connector came from XbdmClient::connect(): cancel() then also
  // stops a reconnect() inside its TCP connect. Both under transportMutex.
  std::shared_ptr<ConnectSlot> connectSlot;
  std::stop_source connectStop;

  std::vector<uint8_t> buffer;
  size_t bufferPos = 0;
  bool cleanEof = false;

  std::optional<Clock::time_point> deadline;
  milliseconds deadlineLength{0};
  std::string_view deadlineWhat;

  bool transferActive = false;
  std::vector<std::string> pendingCleanup;
  std::optional<StatusLine> lastStatus;
  // How far the last command line got; deliveries counts the commands begun, so a
  // call can tell whether the delivery is its own.
  std::optional<CommandDelivery> delivery;
  uint64_t deliveries = 0;
  bool awaitingAnswer = false;

  bool connected() const noexcept { return transport && transport->isOpen(); }
  size_t buffered() const noexcept { return buffer.size() - bufferPos; }

  void cancel() noexcept {
    std::lock_guard<std::mutex> lock(transportMutex);
    cancelled = true;
    ++cancelEpoch;
    connectStop.request_stop();
    if (transport) transport->close();
  }

  // Gives the connector a fresh stop token for a reconnect(). False when cancel()
  // was called after `epoch` was read.
  bool armConnect(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(transportMutex);
    if (cancelEpoch != epoch) return false;
    connectStop = std::stop_source();
    if (connectSlot) connectSlot->set(connectStop.get_token());
    return true;
  }

  // False, with the new transport closed and the old one kept, when cancel() was
  // called after `epoch` was read.
  bool install(net::TransportPtr next, uint64_t epoch) {
    net::TransportPtr unused;
    bool installed = false;
    {
      std::lock_guard<std::mutex> lock(transportMutex);
      if (cancelEpoch == epoch) {
        unused = std::move(transport);
        transport = std::move(next);
        cancelled = false;
        installed = true;
      } else {
        unused = std::move(next);
      }
    }
    if (unused) unused->close();
    buffer.clear();
    bufferPos = 0;
    deadline.reset();
    awaitingAnswer = false;
    return installed;
  }

  // Closes the connection after a failure that leaves the stream position unknown.
  unexpected<Error> drop(Error error) {
    if (transport) transport->close();
    awaitingAnswer = false;
    buffer.clear();
    bufferPos = 0;
    deadline.reset();
    if (cancelled && error.code != ErrorCode::Cancelled) {
      error.code = ErrorCode::Cancelled;
      error.message = "cancelled (" + error.message + ")";
    }
    spdlog::debug("XBDM connection closed: {}", formatError(error));
    return unexpected<Error>(std::move(error));
  }

  unexpected<Error> dropWith(ErrorCode code, std::string message, std::string_view context) {
    return drop(withContext(makeError(code, std::move(message)), context));
  }

  Result<void> checkUsable(std::string_view context, bool internal) {
    if (!internal && transferActive) {
      return fail(ErrorCode::InvalidArgument,
                  std::string(context) + ": a file transfer owns this connection; finish or abort it first, "
                                         "or open a second client");
    }
    if (!connected()) {
      return fail(cancelled ? ErrorCode::Cancelled : ErrorCode::NotConnected,
                  std::string(context) + ": not connected to the console");
    }
    if (buffered() != 0) {
      return dropWith(ErrorCode::Protocol,
                      "the console sent " + std::to_string(buffered()) + " bytes that belong to no command",
                      context);
    }
    return {};
  }

  // A bound on the whole of what follows, not only on each wait. Zero is none.
  void startDeadline(milliseconds length, std::string_view what) {
    if (length.count() > 0) {
      deadline = net::deadlineAfter(length);
      deadlineLength = length;
      deadlineWhat = what;
    } else {
      deadline.reset();
    }
  }

  void startDeadline() { startDeadline(options.commandTimeout, "the command did not complete"); }

  unexpected<Error> transportFailure(Error error, milliseconds idle, std::string_view context) {
    if (error.code == ErrorCode::Timeout) {
      if (deadline && Clock::now() >= *deadline) {
        error.message = std::string(deadlineWhat) + " within " + std::to_string(deadlineLength.count()) + " ms";
      } else {
        error.message = "the console stopped responding (nothing for " + std::to_string(idle.count()) + " ms)";
      }
    } else if (error.code == ErrorCode::NotConnected || cancelled) {
      error.code = ErrorCode::Cancelled;
      error.message = "cancelled: the connection was closed";
    }
    return drop(withContext(std::move(error), context));
  }

  Result<void> arm(milliseconds idle, std::string_view context) {
    milliseconds timeout = idle;
    if (deadline) {
      const auto now = Clock::now();
      if (now >= *deadline) {
        return transportFailure(makeError(ErrorCode::Timeout, "deadline"), idle, context);
      }
      const auto remaining = std::max(std::chrono::ceil<milliseconds>(*deadline - now), milliseconds(1));
      if (timeout.count() == 0 || remaining < timeout) timeout = remaining;
    }
    if (auto r = transport->setTimeout(timeout); !r) return transportFailure(r.error(), idle, context);
    return {};
  }

  Result<void> sendBytes(std::span<const uint8_t> data, milliseconds idle, std::string_view context) {
    if (auto r = arm(idle, context); !r) return r;
    if (auto r = transport->writeAll(data); !r) return transportFailure(r.error(), idle, context);
    return {};
  }

  void trace(TraceEvent event, std::string_view text, uint64_t bytes = 0) const {
    if (!options.trace) return;
    try {
      options.trace(event, text, bytes);
    } catch (...) {
    }
  }

  // A new command, named by the first word of its line, that has not left yet.
  void beginCommand(std::string_view line) {
    delivery = CommandDelivery{std::string(line.substr(0, line.find(' '))), Delivery::NotSent};
    ++deliveries;
    awaitingAnswer = false;
  }

  // Its own writeSome loop, with the idle timeout and command deadline of
  // sendBytes, so that delivery knows whether part of the line went out.
  Result<void> sendLine(std::string_view line, std::string_view context) {
    std::string wire(line);
    wire += "\r\n";
    trace(TraceEvent::Sent, line);
    const milliseconds idle = options.idleTimeout;
    std::span<const uint8_t> rest = bytesOf(wire);
    while (!rest.empty()) {
      if (auto r = arm(idle, context); !r) return r;
      auto n = transport->writeSome(rest);
      if (!n) return transportFailure(n.error(), idle, context);
      if (*n == 0) return dropWith(ErrorCode::Io, "write made no progress", context);
      rest = rest.subspan(*n);
      if (delivery) delivery->delivery = rest.empty() ? Delivery::Sent : Delivery::PartlySent;
    }
    awaitingAnswer = true;
    return {};
  }

  // Appends what the transport has; 0 is end of stream.
  Result<size_t> fill(milliseconds idle, std::string_view context) {
    if (bufferPos == buffer.size()) {
      buffer.clear();
      bufferPos = 0;
    } else if (bufferPos > 0) {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bufferPos));
      bufferPos = 0;
    }
    if (auto r = arm(idle, context); !r) return unexpected<Error>(r.error());
    uint8_t chunk[kReadChunkBytes];
    auto n = transport->readSome(chunk);
    if (!n) return transportFailure(n.error(), idle, context);
    buffer.insert(buffer.end(), chunk, chunk + *n);
    return *n;
  }

  Result<std::string> readLine(milliseconds idle, std::string_view context) {
    return readLine(idle, context, options.maxLineBytes);
  }

  Result<std::string> readLine(milliseconds idle, std::string_view context, size_t maxBytes) {
    maxBytes = std::min(maxBytes, options.maxLineBytes);
    cleanEof = false;
    // Bytes after bufferPos already searched; fill() keeps them in front.
    size_t scanned = 0;
    for (;;) {
      const auto begin = buffer.begin() + static_cast<std::ptrdiff_t>(bufferPos);
      const auto newline = std::find(begin + static_cast<std::ptrdiff_t>(scanned), buffer.end(), uint8_t{'\n'});
      if (newline != buffer.end()) {
        std::string line(begin, newline);
        bufferPos = static_cast<size_t>(newline - buffer.begin()) + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        trace(TraceEvent::Received, line);
        if (line.size() > maxBytes) {
          return dropWith(ErrorCode::LimitExceeded,
                          "the console sent a line of " + std::to_string(line.size()) + " bytes, limit is " +
                              std::to_string(maxBytes),
                          context);
        }
        return line;
      }
      if (buffered() > maxBytes + 1) {
        return dropWith(ErrorCode::LimitExceeded,
                        "the console sent more than " + std::to_string(maxBytes) + " bytes without ending the line",
                        context);
      }
      const size_t pending = buffered();
      scanned = pending;
      auto n = fill(idle, context);
      if (!n) {
        cleanEof = pending == 0 && n.error().code == ErrorCode::Disconnected;
        return unexpected<Error>(n.error());
      }
      if (*n == 0) {
        cleanEof = pending == 0;
        return dropWith(ErrorCode::Disconnected,
                        pending == 0 ? "the console closed the connection"
                                     : "the console closed the connection in the middle of a line",
                        context);
      }
    }
  }

  Result<StatusLine> readStatus(milliseconds idle, std::string_view context) {
    return readStatus(idle, context, options.maxLineBytes);
  }

  Result<StatusLine> readStatus(milliseconds idle, std::string_view context, size_t maxBytes) {
    auto line = readLine(idle, context, maxBytes);
    if (!line) return unexpected<Error>(line.error());
    auto status = parseStatusLine(*line);
    if (!status) {
      return dropWith(ErrorCode::Protocol, "malformed status line '" + preview(*line) + "'", context);
    }
    lastStatus = *status;
    if (awaitingAnswer && delivery) delivery->delivery = Delivery::Answered;
    awaitingAnswer = false;
    return *status;
  }

  // A refusal is returned as an error and keeps the connection; any other status
  // that is not in `accepted` closes it.
  Result<StatusLine> expect(Result<StatusLine> status, std::initializer_list<int> accepted, std::string_view context) {
    if (!status) return status;
    if (std::find(accepted.begin(), accepted.end(), status->code) != accepted.end()) return status;
    if (status->isRefusal()) {
      deadline.reset();
      return unexpected<Error>(refusal(*status, context));
    }
    const std::string what = status->isSuccess() ? "unexpected answer " : "unknown status ";
    return dropWith(ErrorCode::Protocol,
                    what + "'" + std::to_string(status->code) + "- " + preview(status->text) + "'", context);
  }

  Result<StatusLine> request(const std::string &line, std::initializer_list<int> accepted, std::string_view context,
                             milliseconds idle, bool internal = false) {
    beginCommand(line);
    if (auto r = checkUsable(context, internal); !r) return unexpected<Error>(r.error());
    startDeadline();
    if (auto r = sendLine(line, context); !r) return unexpected<Error>(r.error());
    return expect(readStatus(idle, context), accepted, context);
  }

  Result<StatusLine> request(const std::string &line, std::initializer_list<int> accepted, std::string_view context) {
    return request(line, accepted, context, options.idleTimeout);
  }

  void finishCommand() { deadline.reset(); }

  // Reads the lines of a 202 body up to ".". A failing visitor closes the
  // connection, because the rest of the body is still unread.
  template <class Visit> Result<void> readBody(std::string_view context, Visit &&visit) {
    size_t total = 0;
    for (;;) {
      auto line = readLine(options.idleTimeout, context);
      if (!line) return unexpected<Error>(line.error());
      if (*line == ".") break;
      total += std::max(line->size() + 2, kMinBodyLineCharge);
      if (total > options.maxBodyBytes) {
        return dropWith(ErrorCode::LimitExceeded,
                        "the answer exceeds " + std::to_string(options.maxBodyBytes) + " bytes", context);
      }
      if (auto r = visit(*line); !r) return drop(r.error());
    }
    finishCommand();
    return {};
  }

  Result<std::vector<std::string>> readBodyLines(std::string_view context) {
    std::vector<std::string> lines;
    auto r = readBody(context, [&](const std::string &line) -> Result<void> {
      lines.push_back(line);
      return {};
    });
    if (!r) return unexpected<Error>(r.error());
    return lines;
  }

  Result<size_t> readBinarySome(std::span<uint8_t> out, milliseconds idle, std::string_view context) {
    if (out.empty()) return size_t{0};
    if (buffered() > 0) {
      const size_t n = std::min(out.size(), buffered());
      std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(bufferPos), n, out.begin());
      bufferPos += n;
      trace(TraceEvent::BinaryReceived, {}, n);
      return n;
    }
    if (auto r = arm(idle, context); !r) return unexpected<Error>(r.error());
    auto n = transport->readSome(out);
    if (!n) return transportFailure(n.error(), idle, context);
    if (*n == 0) return dropWith(ErrorCode::Disconnected, "the console closed the connection during binary data", context);
    trace(TraceEvent::BinaryReceived, {}, *n);
    return *n;
  }

  Result<void> readBinaryExact(std::span<uint8_t> out, milliseconds idle, std::string_view context) {
    while (!out.empty()) {
      auto n = readBinarySome(out, idle, context);
      if (!n) return unexpected<Error>(n.error());
      out = out.subspan(*n);
    }
    return {};
  }

  Result<void> greet() {
    startDeadline(options.greetingTimeout, "the greeting did not arrive");
    auto status = readStatus(options.greetingTimeout, "greeting", kMaxShortLineBytes);
    if (!status) return unexpected<Error>(status.error());
    deadline.reset();
    if (status->code == status::kConnected) {
      std::string text = status->text;
      std::transform(text.begin(), text.end(), text.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (text != "connected") spdlog::warn("XBDM greeting is '{}', expected 'connected'", preview(status->text));
      return {};
    }
    Error error = status->isRefusal()
                      ? refusal(*status, "connect")
                      : makeError(ErrorCode::Protocol, "connect: the console greeted with '" +
                                                           std::to_string(status->code) + "- " +
                                                           preview(status->text) + "', expected 201");
    return drop(std::move(error));
  }

  void sayBye() noexcept {
    if (!connected() || transferActive || buffered() != 0) return;
    startDeadline(options.byeTimeout, "the answer to bye did not arrive");
    trace(TraceEvent::Sent, "bye");
    if (!sendBytes(bytesOf("bye\r\n"), options.byeTimeout, "bye")) return;
    (void)readLine(options.byeTimeout, "bye", kMaxShortLineBytes);
  }

  void closeConnection() noexcept {
    try {
      sayBye();
    } catch (...) {
    }
    if (transport) transport->close();
    buffer.clear();
    bufferPos = 0;
  }
};

} // namespace detail

namespace {

using detail::Command;
using detail::Session;
using std::chrono::milliseconds;

constexpr std::string_view kTransferBusy = "a file transfer owns this connection";
// DevTool shows a drive table of 42 entries (section 3.2).
constexpr size_t kMaxDrives = 64;

Result<std::string> buildLine(Command command, const Session &session) {
  return command.finish(session.options.maxCommandBytes);
}

Result<std::string> filePath(const std::string &path) {
  auto canonical = canonicalPath(path);
  if (!canonical) return canonical;
  if (isDriveRoot(*canonical)) {
    return fail(ErrorCode::InvalidArgument, "'" + *canonical + "' is a drive root, not a file or folder");
  }
  return canonical;
}

struct Fields {
  bool ok = true;
  std::optional<uint32_t> u32(const Params &params, std::string_view key) {
    const std::string *text = params.find(key);
    if (!text) return std::nullopt;
    auto value = parseNumber32(*text);
    if (!value) ok = false;
    return value;
  }
  std::optional<uint64_t> pair(const Params &params, std::string_view hiKey, std::string_view loKey) {
    const auto hi = u32(params, hiKey);
    const auto lo = u32(params, loKey);
    if (!hi || !lo) return std::nullopt;
    return joinHalves(*hi, *lo);
  }
};

// sizehi/sizelo, the times and the flags, shared by dirlist and getfileattributes.
bool readAttributes(const Params &params, FileAttributes &out) {
  Fields fields;
  const auto hi = fields.u32(params, "sizehi");
  const auto lo = fields.u32(params, "sizelo");
  out.size = joinHalves(hi.value_or(0), lo.value_or(0));
  out.sizeKnown = hi && lo;
  out.createdFileTime = fields.pair(params, "createhi", "createlo");
  out.changedFileTime = fields.pair(params, "changehi", "changelo");
  out.isDirectory = params.hasFlag("directory");
  out.isReadOnly = params.hasFlag("readonly");
  out.isHidden = params.hasFlag("hidden");
  return fields.ok && !params.malformed;
}

// Adds the first value of each key, and each flag, that `into` does not have yet,
// so an answer spread over many lines keeps only the fields a command reads.
void pick(Params &into, const Params &from, std::initializer_list<std::string_view> keys,
          std::initializer_list<std::string_view> flags = {}) {
  for (std::string_view key : keys) {
    if (into.find(key)) continue;
    if (const std::string *value = from.find(key)) into.values.emplace_back(std::string(key), *value);
  }
  for (std::string_view flag : flags) {
    if (!into.hasFlag(flag) && from.hasFlag(flag)) into.flags.emplace_back(flag);
  }
  into.malformed = into.malformed || from.malformed;
}

void pickAttributes(Params &into, const Params &from) {
  pick(into, from, {"sizehi", "sizelo", "createhi", "createlo", "changehi", "changelo"},
       {"directory", "readonly", "hidden"});
}

bool sameName(std::string_view a, std::string_view b) noexcept {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

ExecState execStateOf(std::string_view word) {
  if (word == "start") return ExecState::Start;
  if (word == "stop") return ExecState::Stop;
  if (word == "pending") return ExecState::Pending;
  if (word == "reboot") return ExecState::Reboot;
  if (word == "pending_title") return ExecState::PendingTitle;
  if (word == "reboot_title") return ExecState::RebootTitle;
  return ExecState::Unknown;
}

// ".<8 hex digits><extension>"
std::string randomSuffix(std::string_view extension) {
  static std::atomic<uint32_t> counter{0};
  std::random_device device;
  const uint32_t token = device() ^ (counter.fetch_add(1) * 0x9E3779B9u);
  static constexpr char digits[] = "0123456789abcdef";
  std::string suffix = ".";
  for (int shift = 28; shift >= 0; shift -= 4) suffix.push_back(digits[(token >> shift) & 0xF]);
  return suffix + std::string(extension);
}

// ".<8 hex digits>.part"
std::string randomPartSuffix() {
  return randomSuffix(".part");
}

std::string temporaryName(std::string_view finalName, std::string_view extension = ".part") {
  const std::string suffix = randomSuffix(extension);
  const size_t room = kMaxFileNameBytes - suffix.size();
  std::string name(finalName.substr(0, room));
  return name + suffix;
}

bool isRefusal(const Error &error) {
  return consoleStatusCode(error).has_value();
}

// For a failed call that changes something on the console: whether its command
// (`command`, the last of the call's commands) can have been carried out, for the
// humans who read the message. Refusals and failures before any command of the
// call began are returned as they are.
Error withDeliveryNote(const Session &s, uint64_t deliveriesBefore, std::string_view command, Error error) {
  if (isRefusal(error) || s.deliveries == deliveriesBefore || !s.delivery) return error;
  if (s.delivery->command != command || s.delivery->delivery == Delivery::NotSent) {
    error.message += "; the command was not sent";
  } else if (s.delivery->delivery != Delivery::Answered) {
    error.message += "; the command was sent but not answered, so the console may have carried it out";
  }
  return error;
}

// Runs a mutating call and notes on a failure how far its command got.
template <class Call> Result<void> mutating(Session &s, std::string_view command, Call &&call) {
  const uint64_t before = s.deliveries;
  Result<void> r = call();
  if (!r) return unexpected<Error>(withDeliveryNote(s, before, command, r.error()));
  return r;
}

// A command answered by 202 and a body, or by another accepted status whose text
// is the only line. Each line goes to visit, parsed, as it arrives.
template <class Visit>
Result<void> eachLine(Session &s, Result<std::string> line, std::string_view context,
                      std::initializer_list<int> accepted, Visit &&visit) {
  if (!line) return unexpected<Error>(line.error());
  auto status = s.request(*line, accepted, context);
  if (!status) return unexpected<Error>(status.error());
  if (status->code != status::kMultiline) {
    visit(parseParams(status->text));
    s.finishCommand();
    return {};
  }
  return s.readBody(context, [&](const std::string &text) -> Result<void> {
    visit(parseParams(text));
    return {};
  });
}

Result<FileAttributes> fileAttributes(Session &s, const std::string &path) {
  const std::string context = "getfileattributes " + path;
  Params all;
  auto r = eachLine(s, buildLine(Command("getfileattributes").text("name", path), s), context,
                    {status::kOk, status::kMultiline}, [&](const Params &params) { pickAttributes(all, params); });
  if (!r) return unexpected<Error>(r.error());
  FileAttributes attributes;
  if (!readAttributes(all, attributes)) return fail(ErrorCode::Protocol, context + ": malformed answer");
  return attributes;
}

// `folder` is canonical.
Result<DirListing> listFolder(Session &s, const std::string &folder) {
  std::string target = folder;
  if (target.back() != '\\') target.push_back('\\');
  auto line = buildLine(Command("dirlist").text("name", target), s);
  if (!line) return unexpected<Error>(line.error());
  const std::string context = "dirlist " + target;
  auto status = s.request(*line, {status::kMultiline}, context);
  if (!status) return unexpected<Error>(status.error());

  DirListing listing;
  auto body = s.readBody(context, [&](const std::string &text) -> Result<void> {
    const Params params = parseParams(text);
    DirEntry entry;
    const std::string *name = params.find("name");
    if (name && (*name == "." || *name == "..")) return {};
    if (!name || !validateName(*name) || !readAttributes(params, entry)) {
      ++listing.skipped;
      spdlog::debug("{}: skipping entry '{}'", context, detail::preview(text));
      return {};
    }
    entry.name = *name;
    listing.entries.push_back(std::move(entry));
    return {};
  });
  if (!body) return unexpected<Error>(body.error());
  return listing;
}

// getfileattributes, completed from the parent folder's listing when the console
// does not know the command (407) or, with needSize, sent no size (section 3.4).
// nullopt when the path does not exist: any other 4xx, or not in the listing.
Result<std::optional<FileAttributes>> lookUp(Session &s, const std::string &path, bool needSize) {
  std::optional<FileAttributes> known;
  if (auto direct = fileAttributes(s, path)) {
    if (!needSize || direct->sizeKnown) return std::optional<FileAttributes>(*direct);
    known = *direct;
  } else if (consoleStatusCode(direct.error()) != status::kInvalidCommand) {
    if (isRefusal(direct.error())) return std::optional<FileAttributes>{};
    return unexpected<Error>(direct.error());
  }
  auto parent = parentOf(path);
  auto name = nameOf(path);
  if (!parent || !name) return known;
  auto listing = listFolder(s, *parent);
  if (!listing) {
    if (isRefusal(listing.error())) return known;
    return unexpected<Error>(listing.error());
  }
  for (const DirEntry &entry : listing->entries) {
    if (sameName(entry.name, *name)) return std::optional<FileAttributes>(static_cast<const FileAttributes &>(entry));
  }
  return known;
}

bool hostPathTaken(const std::filesystem::path &path) {
  std::error_code ec;
  return std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::not_found;
}

// Writes "<final>.part", or "<final>.<random>.part" when that name is taken, and
// moves it over the final name only on commit().
class HostFile {
public:
  HostFile() = default;
  HostFile(const HostFile &) = delete;
  HostFile &operator=(const HostFile &) = delete;
  ~HostFile() {
    if (committed_ || temp_.empty()) return;
    out_.close();
    std::error_code ec;
    std::filesystem::remove(temp_, ec);
  }

  Result<void> open(const std::filesystem::path &finalPath) {
    if (finalPath.empty()) return fail(ErrorCode::InvalidArgument, "local path must not be empty");
    std::error_code ec;
    if (std::filesystem::is_directory(finalPath, ec)) {
      return fail(ErrorCode::InvalidArgument, "'" + pathToUtf8(finalPath) + "' is a directory");
    }
    final_ = finalPath;
    temp_ = finalPath;
    temp_ += ".part";
    for (int attempt = 0; hostPathTaken(temp_); ++attempt) {
      if (attempt == 16) {
        const std::string name = pathToUtf8(temp_);
        temp_.clear();
        return fail(ErrorCode::Io, "no free temporary name next to '" + name + "'");
      }
      temp_ = finalPath;
      temp_ += randomPartSuffix();
    }
    out_.open(temp_, std::ios::binary | std::ios::trunc);
    if (!out_) {
      const std::string name = pathToUtf8(temp_);
      temp_.clear();
      return fail(ErrorCode::Io, "cannot create '" + name + "'");
    }
    return {};
  }

  Result<void> write(std::span<const uint8_t> data) {
    out_.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out_) return fail(ErrorCode::Io, "write to '" + pathToUtf8(temp_) + "' failed");
    return {};
  }

  Result<void> commit() {
    out_.flush();
    out_.close();
    if (out_.fail()) return fail(ErrorCode::Io, "closing '" + pathToUtf8(temp_) + "' failed");
    std::error_code ec;
    std::filesystem::rename(temp_, final_, ec);
    if (ec) {
      return fail(ErrorCode::Io,
                  "cannot move '" + pathToUtf8(temp_) + "' to '" + pathToUtf8(final_) + "': " + ec.message(),
                  ec.value());
    }
    committed_ = true;
    return {};
  }

private:
  std::ofstream out_;
  std::filesystem::path final_;
  std::filesystem::path temp_;
  bool committed_ = false;
};

} // namespace

std::optional<int> consoleStatusCode(const Error &error) noexcept {
  if (error.sysError < 400 || error.sysError > 499) return std::nullopt;
  try {
    const std::string marker = std::string(detail::kAnswered) + std::to_string(error.sysError) + "-";
    if (error.message.find(marker) == std::string::npos) return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
  return error.sysError;
}

// ---------------------------------------------------------------------------
// FileReader

struct FileReader::State {
  std::shared_ptr<Session> session;
  std::string path;
  uint64_t size = 0;
  uint64_t remaining = 0;
  bool open = false;

  void release() {
    if (open) session->transferActive = false;
    open = false;
  }
};

FileReader::FileReader(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileReader::FileReader(FileReader &&) noexcept = default;
FileReader &FileReader::operator=(FileReader &&other) noexcept {
  if (this != &other) {
    close();
    state_ = std::move(other.state_);
  }
  return *this;
}
FileReader::~FileReader() {
  close();
}

uint64_t FileReader::size() const noexcept {
  return state_ ? state_->size : 0;
}

uint64_t FileReader::position() const noexcept {
  return state_ ? state_->size - state_->remaining : 0;
}

bool FileReader::isOpen() const noexcept {
  return state_ && state_->open;
}

Result<size_t> FileReader::read(std::span<uint8_t> buffer) {
  if (!state_) return fail(ErrorCode::NotConnected, "the reader was moved from");
  auto &s = *state_;
  if (s.remaining == 0) {
    s.release();
    return size_t{0};
  }
  if (!s.open) return fail(ErrorCode::NotConnected, "getfile " + s.path + ": the download was closed or failed");
  if (buffer.empty()) return size_t{0};
  const size_t want = static_cast<size_t>(std::min<uint64_t>(buffer.size(), s.remaining));
  auto n = s.session->readBinarySome(buffer.first(want), s.session->options.idleTimeout, "getfile " + s.path);
  if (!n) {
    s.release();
    return unexpected<Error>(n.error());
  }
  s.remaining -= *n;
  if (s.remaining == 0) s.release();
  return *n;
}

void FileReader::close() noexcept {
  if (!state_ || !state_->open) return;
  if (state_->remaining > 0) {
    try {
      (void)state_->session->drop(makeError(ErrorCode::Cancelled, "download of " + state_->path + " closed early"));
    } catch (...) {
      if (state_->session->transport) state_->session->transport->close();
    }
  }
  state_->release();
}

void FileReader::abort() noexcept {
  close();
}

void FileReader::cancel() noexcept {
  if (state_) state_->session->cancel();
}

// ---------------------------------------------------------------------------
// FileWriter

struct FileWriter::State {
  std::shared_ptr<Session> session;
  std::string path;
  std::string tempPath;
  uint64_t size = 0;
  uint64_t written = 0;
  bool open = false;
  bool confirmed = false;
  // Whether a file of the final name existed when openWrite() ran: only then may
  // finish() replace one.
  bool finalExistedAtOpen = false;

  std::string context() const { return "sendfile " + path; }

  unexpected<Error> failed(Error error) {
    if (open) {
      if (session->connected()) (void)session->drop(error);
      session->pendingCleanup.push_back(tempPath);
      session->transferActive = false;
      open = false;
    }
    return unexpected<Error>(std::move(error));
  }

  // On the same connection, after the console answered.
  void removeTemporary() {
    auto line = Command("delete").text("name", tempPath).finish(session->options.maxCommandBytes);
    if (!line) return;
    auto status = session->request(*line, {status::kOk}, "delete " + tempPath);
    if (status) {
      session->finishCommand();
    } else if (!isRefusal(status.error())) {
      session->pendingCleanup.push_back(tempPath);
    }
  }
};

FileWriter::FileWriter(std::unique_ptr<State> state) : state_(std::move(state)) {}
FileWriter::FileWriter(FileWriter &&) noexcept = default;
FileWriter &FileWriter::operator=(FileWriter &&other) noexcept {
  if (this != &other) {
    abort();
    state_ = std::move(other.state_);
  }
  return *this;
}
FileWriter::~FileWriter() {
  abort();
}

uint64_t FileWriter::size() const noexcept {
  return state_ ? state_->size : 0;
}

uint64_t FileWriter::written() const noexcept {
  return state_ ? state_->written : 0;
}

bool FileWriter::isOpen() const noexcept {
  return state_ && state_->open;
}

const std::string &FileWriter::path() const noexcept {
  static const std::string empty;
  return state_ ? state_->path : empty;
}

const std::string &FileWriter::temporaryPath() const noexcept {
  static const std::string empty;
  return state_ ? state_->tempPath : empty;
}

Result<void> FileWriter::write(std::span<const uint8_t> data) {
  if (!state_) return fail(ErrorCode::NotConnected, "the writer was moved from");
  auto &s = *state_;
  if (!s.open) return fail(ErrorCode::NotConnected, s.context() + ": the upload was finished, aborted or failed");
  if (data.size() > s.size - s.written) {
    return fail(ErrorCode::InvalidArgument, s.context() + ": " + std::to_string(data.size()) + " more bytes exceed the " +
                                                std::to_string(s.size) + " announced (" + std::to_string(s.written) +
                                                " written)");
  }
  if (data.empty()) return {};
  if (!s.session->connected()) {
    return s.failed(makeError(s.session->cancelled ? ErrorCode::Cancelled : ErrorCode::NotConnected,
                              s.context() + ": the connection is closed"));
  }
  if (auto r = s.session->sendBytes(data, s.session->options.idleTimeout, s.context()); !r) return s.failed(r.error());
  s.session->trace(TraceEvent::BinarySent, {}, data.size());
  s.written += data.size();
  return {};
}

Result<void> FileWriter::finish() {
  if (!state_) return fail(ErrorCode::NotConnected, "the writer was moved from");
  auto &s = *state_;
  auto &session = *s.session;
  if (!s.open) return fail(ErrorCode::NotConnected, s.context() + ": the upload was finished, aborted or failed");
  if (s.written != s.size) {
    return fail(ErrorCode::InvalidArgument, s.context() + ": " + std::to_string(s.written) + " of " +
                                                std::to_string(s.size) + " bytes written");
  }

  if (!s.confirmed) {
    if (!session.connected()) {
      return s.failed(makeError(session.cancelled ? ErrorCode::Cancelled : ErrorCode::NotConnected,
                                s.context() + ": the connection is closed"));
    }
    session.startDeadline();
    auto status = session.expect(
        session.readStatus(session.options.slowIdleTimeout, s.context(), detail::kMaxShortLineBytes), {status::kOk},
        s.context());
    if (!status) {
      if (!isRefusal(status.error())) return s.failed(status.error());
      session.transferActive = false;
      s.open = false;
      s.removeTemporary();
      return unexpected<Error>(status.error());
    }
  }
  session.transferActive = false;
  s.open = false;

  // The data is on the console under the temporary name. Replace the final name.
  // Once the old file may be gone, the temporary file can hold the only copy of
  // either version and is never deleted.
  std::string_view finalRemoved;
  auto giveUp = [&](Error error) -> Result<void> {
    if (!finalRemoved.empty()) {
      error.message += "; the upload is kept as " + s.tempPath + ", since " + s.path + " " + std::string(finalRemoved);
    } else if (session.connected()) {
      s.removeTemporary();
    } else {
      session.pendingCleanup.push_back(s.tempPath);
    }
    return unexpected<Error>(std::move(error));
  };

  // A file that was not there at openWrite() is someone else's: left alone.
  auto appeared = [&]() -> Result<void> {
    return giveUp(makeError(ErrorCode::InvalidArgument,
                            s.context() + ": " + s.path + " appeared during the upload and was not replaced; " +
                                (session.connected() ? "the upload was removed"
                                                     : "the upload is deleted by the next reconnect()")));
  };

  auto existing = lookUp(session, s.path, false);
  if (!existing) return giveUp(existing.error());
  if (*existing) {
    if ((*existing)->isDirectory) {
      return giveUp(makeError(ErrorCode::InvalidArgument, s.context() + ": a folder of that name exists"));
    }
    if (!s.finalExistedAtOpen) return appeared();
    auto line = Command("delete").text("name", s.path).finish(session.options.maxCommandBytes);
    if (!line) return giveUp(line.error());
    auto removed = session.request(*line, {status::kOk}, "delete " + s.path);
    if (!removed) {
      // Once any of its line went out without an answer, the delete may have happened.
      if (!isRefusal(removed.error()) && session.delivery && session.delivery->delivery != Delivery::NotSent) {
        finalRemoved = "may have been deleted";
      }
      return giveUp(removed.error());
    }
    session.finishCommand();
    finalRemoved = "was deleted";
  }

  auto line = Command("rename").text("name", s.tempPath).text("newname", s.path).finish(session.options.maxCommandBytes);
  if (!line) return giveUp(line.error());
  auto renamed = session.request(*line, {status::kOk}, "rename " + s.tempPath);
  if (!renamed) {
    // Refused while nothing was deleted: the final name may have appeared since
    // the lookup. Looked up once more, so other refusals keep their status.
    if (finalRemoved.empty() && !s.finalExistedAtOpen && isRefusal(renamed.error())) {
      auto now = lookUp(session, s.path, false);
      if (now && *now && !(*now)->isDirectory) return appeared();
    }
    return giveUp(renamed.error());
  }
  session.finishCommand();
  return {};
}

void FileWriter::abort() noexcept {
  if (!state_ || !state_->open) return;
  try {
    (void)state_->failed(makeError(ErrorCode::Cancelled, state_->context() + ": aborted"));
  } catch (...) {
  }
}

void FileWriter::cancel() noexcept {
  if (state_) state_->session->cancel();
}

// ---------------------------------------------------------------------------
// XbdmClient

XbdmClient::XbdmClient(std::shared_ptr<detail::Session> session) : session_(std::move(session)) {}
XbdmClient::XbdmClient(XbdmClient &&) noexcept = default;
XbdmClient &XbdmClient::operator=(XbdmClient &&other) noexcept {
  if (this != &other) {
    if (session_ && !session_->transferActive) session_->closeConnection();
    session_ = std::move(other.session_);
  }
  return *this;
}

XbdmClient::~XbdmClient() {
  // An open transfer keeps the session and finishes on its own.
  if (session_ && !session_->transferActive) session_->closeConnection();
}

namespace {

Result<net::TransportPtr> connectCancelled(std::string_view what) {
  return fail(ErrorCode::Cancelled, "connect to " + std::string(what) + " cancelled");
}

// Connects through the connector, unless the token is already stopped, and reads
// the greeting; a stop request during the greeting cancels the session. The
// connector itself runs to its own end, so a token reaches it only through slot.
Result<std::shared_ptr<Session>> openSession(XbdmClient::Connector connector, ClientOptions options,
                                             std::stop_token stop, std::shared_ptr<detail::ConnectSlot> slot) {
  if (!connector) return fail(ErrorCode::InvalidArgument, "no connector");
  if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "connect cancelled");
  if (slot) slot->set(stop);
  auto transport = connector();
  if (slot) slot->set({});
  if (!transport) return unexpected<Error>(transport.error());
  if (!*transport) return fail(ErrorCode::ConnectFailed, "no transport");
  if (stop.stop_requested()) {
    (*transport)->close();
    return fail(ErrorCode::Cancelled, "connect cancelled");
  }
  auto session = std::make_shared<Session>(std::move(options), std::move(connector));
  session->connectSlot = std::move(slot);
  session->install(std::move(*transport), 0);
  {
    std::stop_callback onStop(stop, [raw = session.get()] { raw->cancel(); });
    if (auto r = session->greet(); !r) return unexpected<Error>(r.error());
  }
  if (session->cancelled) {
    session->closeConnection();
    return fail(ErrorCode::Cancelled, "connect cancelled after the greeting");
  }
  spdlog::debug("connected to XBDM at {}", session->transport->describe());
  return session;
}

} // namespace

Result<XbdmClient> XbdmClient::connect(const net::Endpoint &endpoint, ClientOptions options) {
  return connect(endpoint, std::move(options), std::stop_token());
}

Result<XbdmClient> XbdmClient::connect(const net::Endpoint &endpoint, ClientOptions options, std::stop_token stop) {
  net::Endpoint target = endpoint;
  std::string scheme = target.scheme;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto slot = std::make_shared<detail::ConnectSlot>();
  Connector connector;
  if (scheme.empty() || scheme == "xbdm") {
    target.scheme = "tcp";
    if (target.port == 0) target.port = kXbdmPort;
    connector = [target, slot] { return net::TcpTransport::connect(target, slot->get()); };
  } else {
    // A registry connector cannot be interrupted; the token is checked around it.
    target = net::TransportRegistry::instance().withDefaultPort(target, kXbdmPort);
    connector = [target, slot]() -> Result<net::TransportPtr> {
      const std::stop_token token = slot->get();
      if (token.stop_requested()) return connectCancelled(target.host);
      auto transport = net::TransportRegistry::instance().connect(target);
      if (transport && *transport && token.stop_requested()) {
        (*transport)->close();
        return connectCancelled(target.host);
      }
      return transport;
    };
  }
  auto session = openSession(std::move(connector), std::move(options), std::move(stop), std::move(slot));
  if (!session) return unexpected<Error>(session.error());
  return XbdmClient(std::move(*session));
}

Result<XbdmClient> XbdmClient::open(Connector connector, ClientOptions options) {
  return open(std::move(connector), std::move(options), std::stop_token());
}

Result<XbdmClient> XbdmClient::open(Connector connector, ClientOptions options, std::stop_token stop) {
  auto session = openSession(std::move(connector), std::move(options), std::move(stop), nullptr);
  if (!session) return unexpected<Error>(session.error());
  return XbdmClient(std::move(*session));
}

Result<XbdmClient> XbdmClient::attach(net::TransportPtr transport, ClientOptions options, Connector connector) {
  if (!transport) return fail(ErrorCode::ConnectFailed, "no transport");
  auto session = std::make_shared<Session>(std::move(options), std::move(connector));
  session->install(std::move(transport), 0);
  if (auto r = session->greet(); !r) return unexpected<Error>(r.error());
  spdlog::debug("connected to XBDM at {}", session->transport->describe());
  return XbdmClient(std::move(session));
}

bool XbdmClient::isConnected() const noexcept {
  return session_ && session_->connected();
}

bool XbdmClient::transferActive() const noexcept {
  return session_ && session_->transferActive;
}

std::string XbdmClient::describe() const {
  if (!session_ || !session_->transport) return "<not connected>";
  return session_->transport->describe();
}

const ClientOptions &XbdmClient::options() const noexcept {
  static const ClientOptions defaults;
  return session_ ? session_->options : defaults;
}

void XbdmClient::setOptions(const ClientOptions &options) {
  if (session_) session_->options = options;
}

std::optional<StatusLine> XbdmClient::lastStatus() const {
  return session_ ? session_->lastStatus : std::nullopt;
}

std::optional<CommandDelivery> XbdmClient::lastDelivery() const {
  return session_ ? session_->delivery : std::nullopt;
}

std::vector<std::string> XbdmClient::pendingCleanup() const {
  return session_ ? session_->pendingCleanup : std::vector<std::string>{};
}

Result<void> XbdmClient::reconnect() {
  if (!session_) return fail(ErrorCode::NotConnected, "the client was moved from");
  auto &s = *session_;
  if (s.transferActive) return fail(ErrorCode::InvalidArgument, std::string("reconnect: ") + std::string(kTransferBusy));
  if (!s.connector) return fail(ErrorCode::Unsupported, "reconnect: the client was attached without a connector");
  const uint64_t epoch = s.cancelEpoch;
  s.closeConnection();
  if (!s.armConnect(epoch)) return fail(ErrorCode::Cancelled, "reconnect: cancelled before the connection was made");
  auto transport = s.connector();
  if (!transport) {
    Error error = transport.error();
    if (s.cancelEpoch != epoch && error.code != ErrorCode::Cancelled) {
      error.code = ErrorCode::Cancelled;
      error.message = "reconnect: cancelled (" + error.message + ")";
    }
    return unexpected<Error>(std::move(error));
  }
  if (!*transport) return fail(ErrorCode::ConnectFailed, "reconnect: the connector returned no transport");
  if (!s.install(std::move(*transport), epoch)) {
    return fail(ErrorCode::Cancelled, "reconnect: cancelled while the connection was being made");
  }
  if (auto r = s.greet(); !r) return r;

  while (!s.pendingCleanup.empty()) {
    const std::string temp = s.pendingCleanup.back();
    auto line = Command("delete").text("name", temp).finish(s.options.maxCommandBytes);
    if (line) {
      auto status = s.request(*line, {status::kOk}, "delete " + temp);
      if (!status && !isRefusal(status.error())) return unexpected<Error>(status.error());
      s.finishCommand();
      if (status) spdlog::debug("deleted temporary upload {}", temp);
    }
    s.pendingCleanup.pop_back();
  }
  return {};
}

void XbdmClient::close() noexcept {
  if (session_) session_->closeConnection();
}

void XbdmClient::cancel() noexcept {
  if (session_) session_->cancel();
}

namespace {

Result<Session *> sessionOf(const std::shared_ptr<Session> &session) {
  if (!session) return fail(ErrorCode::NotConnected, "the client was moved from");
  return session.get();
}

// A command answered by one 200 line; returns its text.
Result<std::string> singleLine(Session &s, Result<std::string> line, std::string_view context) {
  if (!line) return unexpected<Error>(line.error());
  auto status = s.request(*line, {status::kOk}, context);
  if (!status) return unexpected<Error>(status.error());
  s.finishCommand();
  return status->text;
}

Result<XbeInfo> xbeInfo(Session &s, Result<std::string> line, std::string_view context) {
  Params all;
  auto r = eachLine(s, std::move(line), context, {status::kMultiline},
                    [&](const Params &params) { pick(all, params, {"name", "timestamp", "checksum"}); });
  if (!r) return unexpected<Error>(r.error());
  const std::string *name = all.find("name");
  if (!name) return fail(ErrorCode::Protocol, std::string(context) + ": the answer has no name");
  XbeInfo info;
  info.name = *name;
  if (const std::string *v = all.find("timestamp")) info.timestamp = parseNumber32(*v);
  if (const std::string *v = all.find("checksum")) info.checksum = parseNumber32(*v);
  return info;
}

Result<PowerResult> power(Session &s, Result<std::string> line, std::string_view context) {
  if (!line) return unexpected<Error>(line.error());
  s.beginCommand(*line);
  if (auto r = s.checkUsable(context, false); !r) return unexpected<Error>(r.error());
  s.startDeadline();
  if (auto r = s.sendLine(*line, context); !r) return unexpected<Error>(r.error());
  auto status = s.readStatus(s.options.slowIdleTimeout, context);
  if (!status) {
    if (s.cleanEof && status.error().code == ErrorCode::Disconnected) return PowerResult::ConnectionClosed;
    return unexpected<Error>(status.error());
  }
  if (status->isRefusal()) {
    s.finishCommand();
    return unexpected<Error>(detail::refusal(*status, context));
  }
  if (!status->isSuccess()) {
    return s.dropWith(ErrorCode::Protocol, "unknown status '" + std::to_string(status->code) + "- " +
                                               detail::preview(status->text) + "'",
                      context);
  }
  (void)s.drop(makeError(ErrorCode::Disconnected, std::string(context) + ": the console is going away"));
  return PowerResult::Acknowledged;
}

Result<void> checkRange(uint32_t address, uint64_t length, uint64_t limit, std::string_view what) {
  if (length == 0) return fail(ErrorCode::InvalidArgument, std::string(what) + ": length must be at least 1");
  if (length > limit) {
    return fail(ErrorCode::LimitExceeded, std::string(what) + ": length " + std::to_string(length) +
                                              " exceeds the limit of " + std::to_string(limit));
  }
  if (uint64_t{address} + length > detail::kAddressSpace) {
    return fail(ErrorCode::InvalidArgument, std::string(what) + ": the range passes the end of the 32-bit address space");
  }
  return {};
}

int hexDigit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// One getmem request; appends its bytes to `into`.
Result<void> readMemoryText(Session &session, uint32_t address, uint32_t length, MemoryRead &into) {
  const std::string context = "getmem " + formatNumber(address);
  auto line = buildLine(Command("getmem").number("addr", address).number("length", length), session);
  if (!line) return unexpected<Error>(line.error());
  auto status = session.request(*line, {status::kMultiline}, context);
  if (!status) return unexpected<Error>(status.error());

  const size_t start = into.data.size();
  auto body = session.readBody(context, [&](const std::string &text) -> Result<void> {
    if (text.size() % 2 != 0) return fail(ErrorCode::Protocol, context + ": odd number of hex digits");
    if (into.data.size() - start + text.size() / 2 > length) {
      return fail(ErrorCode::Protocol, context + ": more than the " + std::to_string(length) + " bytes asked for");
    }
    for (size_t i = 0; i < text.size(); i += 2) {
      if (text[i] == '?' || text[i + 1] == '?') {
        into.data.push_back(0);
        into.readable.push_back(false);
        continue;
      }
      const int hi = hexDigit(text[i]);
      const int lo = hexDigit(text[i + 1]);
      if (hi < 0 || lo < 0) return fail(ErrorCode::Protocol, context + ": '" + detail::preview(text) + "' is not hex");
      into.data.push_back(static_cast<uint8_t>((hi << 4) | lo));
      into.readable.push_back(true);
    }
    return {};
  });
  if (!body) return unexpected<Error>(body.error());
  if (into.data.size() - start != length) {
    return fail(ErrorCode::Protocol, context + ": " + std::to_string(into.data.size() - start) + " of " +
                                         std::to_string(length) + " bytes received");
  }
  return {};
}

// A new name that differs only in case: the target "exists" because FATX ignores
// case, so no existence check. Whether the console takes such a rename at once is
// not known (section 3.10); after a 410 or 400, the answers for an existing name,
// it goes through an intermediate name in the same folder.
Result<void> renameCase(Session &s, const std::string &source, const std::string &target, std::string line) {
  const std::string context = "rename " + source;
  auto direct = s.request(line, {status::kOk}, context);
  if (direct) {
    s.finishCommand();
    return {};
  }
  const auto code = consoleStatusCode(direct.error());
  if (code != status::kAlreadyExists && code != status::kUndefined) return unexpected<Error>(direct.error());

  auto parent = parentOf(target);
  auto name = nameOf(target);
  if (!parent || !name) return unexpected<Error>(direct.error());
  auto intermediate = joinPath(*parent, temporaryName(*name, ".ren"));
  if (!intermediate) return unexpected<Error>(intermediate.error());
  auto away = buildLine(Command("rename").text("name", source).text("newname", *intermediate), s);
  auto into = buildLine(Command("rename").text("name", *intermediate).text("newname", target), s);
  auto back = buildLine(Command("rename").text("name", *intermediate).text("newname", source), s);
  if (!away || !into || !back) return unexpected<Error>(direct.error());

  spdlog::debug("{}: {}; renaming through {}", context, formatError(direct.error()), *intermediate);
  auto first = s.request(*away, {status::kOk}, context);
  if (!first) return unexpected<Error>(first.error());
  s.finishCommand();
  auto second = s.request(*into, {status::kOk}, "rename " + *intermediate);
  if (second) {
    s.finishCommand();
    return {};
  }
  Error error = second.error();
  // Without an answer to a step, the file may be under either name.
  bool certain = isRefusal(error);
  if (s.connected()) {
    auto restored = s.request(*back, {status::kOk}, "rename " + *intermediate);
    if (restored) {
      s.finishCommand();
      return unexpected<Error>(std::move(error));
    }
    certain = certain && isRefusal(restored.error());
  }
  error.message += (certain ? "; the file is now named " : "; the file may now be named ") + *intermediate;
  return unexpected<Error>(std::move(error));
}

} // namespace

Result<std::string> XbdmClient::debugName() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return singleLine(**s, buildLine(Command("dbgname"), **s), "dbgname");
}

Result<std::string> XbdmClient::consoleType() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return singleLine(**s, buildLine(Command("consoletype"), **s), "consoletype");
}

Result<std::string> XbdmClient::consoleId() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto text = singleLine(**s, buildLine(Command("getconsoleid"), **s), "getconsoleid");
  if (!text) return text;
  const Params params = parseParams(*text);
  if (const std::string *id = params.find("consoleid")) return *id;
  return text;
}

Result<XbeInfo> XbdmClient::runningTitle() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return xbeInfo(**s, buildLine(Command("xbeinfo").flag("running"), **s), "xbeinfo running");
}

Result<XbeInfo> XbdmClient::executableInfo(const std::string &path) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  return xbeInfo(**s, buildLine(Command("xbeinfo").text("name", *canonical), **s), "xbeinfo " + *canonical);
}

Result<ExecStatus> XbdmClient::execState() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto text = singleLine(**s, buildLine(Command("getexecstate"), **s), "getexecstate");
  if (!text) return unexpected<Error>(text.error());
  std::string word = *text;
  while (!word.empty() && word.back() == ' ') word.pop_back();
  std::transform(word.begin(), word.end(), word.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ExecStatus{execStateOf(word), *text};
}

Result<TitleAddress> XbdmClient::titleAddress() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto text = singleLine(**s, buildLine(Command("altaddr"), **s), "altaddr");
  if (!text) return unexpected<Error>(text.error());
  const Params params = parseParams(*text);
  const std::string *addr = params.find("addr");
  const auto raw = addr ? parseNumber32(*addr) : std::nullopt;
  if (!raw) return fail(ErrorCode::Protocol, "altaddr: no address in '" + detail::preview(*text) + "'");
  TitleAddress address;
  address.raw = *raw;
  address.text = std::to_string(*raw >> 24) + "." + std::to_string((*raw >> 16) & 0xFF) + "." +
                 std::to_string((*raw >> 8) & 0xFF) + "." + std::to_string(*raw & 0xFF);
  return address;
}

Result<ConsoleInfo> XbdmClient::consoleInfo() {
  ConsoleInfo info;
  auto keep = [](auto result, auto &field) -> Result<void> {
    if (result) {
      field = std::move(*result);
      return {};
    }
    if (isRefusal(result.error())) return {};
    return unexpected<Error>(result.error());
  };
  if (auto r = keep(debugName(), info.debugName); !r) return unexpected<Error>(r.error());
  if (auto r = keep(consoleType(), info.consoleType); !r) return unexpected<Error>(r.error());
  if (auto r = keep(consoleId(), info.consoleId); !r) return unexpected<Error>(r.error());
  if (auto r = keep(runningTitle(), info.runningTitle); !r) return unexpected<Error>(r.error());
  if (auto r = keep(execState(), info.execState); !r) return unexpected<Error>(r.error());
  if (auto r = keep(titleAddress(), info.titleAddress); !r) return unexpected<Error>(r.error());
  return info;
}

Result<std::vector<std::string>> XbdmClient::drives() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  std::vector<std::string> names;
  size_t ignored = 0;
  auto r = eachLine(**s, buildLine(Command("drivelist"), **s), "drivelist", {status::kMultiline},
                    [&](const Params &params) {
                      const std::string *name = params.find("drivename");
                      if (!name || !validateDriveName(*name)) return;
                      if (std::any_of(names.begin(), names.end(),
                                      [&](const std::string &known) { return sameName(known, *name); })) {
                        return;
                      }
                      if (names.size() < kMaxDrives) names.push_back(*name);
                      else ++ignored;
                    });
  if (!r) return unexpected<Error>(r.error());
  if (ignored > 0) spdlog::warn("drivelist: ignoring {} drives after the first {}", ignored, kMaxDrives);
  return names;
}

Result<DriveSpace> XbdmClient::driveSpace(const std::string &drive) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  std::string_view name = drive;
  if (name.size() >= 2 && name.substr(name.size() - 2) == ":\\") name.remove_suffix(2);
  else if (!name.empty() && name.back() == ':') name.remove_suffix(1);
  if (auto r = validateDriveName(name); !r) return unexpected<Error>(r.error());
  const std::string root = std::string(name) + ":\\";

  Params all;
  auto r = eachLine(**s, buildLine(Command("drivefreespace").text("name", root), **s), "drivefreespace " + root,
                    {status::kOk, status::kMultiline}, [&](const Params &params) {
                      pick(all, params,
                           {"freetocallerhi", "freetocallerlo", "totalbyteshi", "totalbyteslo", "totalfreebyteshi",
                            "totalfreebyteslo"});
                    });
  if (!r) return unexpected<Error>(r.error());
  Fields fields;
  const auto free = fields.pair(all, "freetocallerhi", "freetocallerlo");
  const auto total = fields.pair(all, "totalbyteshi", "totalbyteslo");
  const auto totalFree = fields.pair(all, "totalfreebyteshi", "totalfreebyteslo");
  if (!fields.ok || !free || !total) {
    return fail(ErrorCode::Protocol, "drivefreespace " + root + ": the answer lacks freetocaller or totalbytes");
  }
  return DriveSpace{*free, *total, totalFree.value_or(*free)};
}

Result<DirListing> XbdmClient::list(const std::string &directory) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = canonicalPath(directory);
  if (!canonical) return unexpected<Error>(canonical.error());
  return listFolder(**s, *canonical);
}

Result<FileAttributes> XbdmClient::attributes(const std::string &path) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = canonicalPath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  return fileAttributes(**s, *canonical);
}

Result<void> XbdmClient::makeDirectory(const std::string &path) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  return mutating(**s, "mkdir", [&]() -> Result<void> {
    auto r = singleLine(**s, buildLine(Command("mkdir").text("name", *canonical), **s), "mkdir " + *canonical);
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<void> XbdmClient::removeFile(const std::string &path) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  return mutating(**s, "delete", [&]() -> Result<void> {
    auto r = singleLine(**s, buildLine(Command("delete").text("name", *canonical), **s), "delete " + *canonical);
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<void> XbdmClient::removeDirectory(const std::string &path) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  return mutating(**s, "delete", [&]() -> Result<void> {
    auto r = singleLine(**s, buildLine(Command("delete").text("name", *canonical).flag("dir"), **s),
                        "delete " + *canonical);
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<void> XbdmClient::rename(const std::string &from, const std::string &to) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto source = filePath(from);
  if (!source) return unexpected<Error>(source.error());
  auto target = filePath(to);
  if (!target) return unexpected<Error>(target.error());
  if (*driveOf(*source) != *driveOf(*target)) {
    return fail(ErrorCode::InvalidArgument, "rename: '" + *source + "' and '" + *target + "' are on different drives");
  }
  auto line = buildLine(Command("rename").text("name", *source).text("newname", *target), **s);
  if (!line) return unexpected<Error>(line.error());
  return mutating(**s, "rename", [&]() -> Result<void> {
    if (*source != *target && sameName(*source, *target)) return renameCase(**s, *source, *target, std::move(*line));

    auto existing = attributes(*target);
    if (existing) return fail(ErrorCode::InvalidArgument, "rename: '" + *target + "' already exists");
    if (!isRefusal(existing.error())) return unexpected<Error>(existing.error());

    auto r = singleLine(**s, std::move(line), "rename " + *source);
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<FileReader> XbdmClient::openRead(const std::string &path, std::optional<uint64_t> expectedSize) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  const std::string context = "getfile " + *canonical;
  if (expectedSize && *expectedSize > 0xFFFFFFFFull) {
    return fail(ErrorCode::Unsupported, context + ": getfile cannot describe a file of 4 GiB or more");
  }
  auto line = buildLine(Command("getfile").text("name", *canonical), session);
  if (!line) return unexpected<Error>(line.error());
  auto status = session.request(*line, {status::kBinary}, context);
  if (!status) return unexpected<Error>(status.error());

  uint8_t prefix[4];
  if (auto r = session.readBinaryExact(prefix, session.options.idleTimeout, context); !r) {
    return unexpected<Error>(r.error());
  }
  const uint64_t length = uint64_t{prefix[0]} | (uint64_t{prefix[1]} << 8) | (uint64_t{prefix[2]} << 16) |
                          (uint64_t{prefix[3]} << 24);
  const uint64_t limit = std::min(session.options.maxDownloadBytes, expectedSize.value_or(UINT64_MAX));
  if (length > limit) {
    return session.dropWith(ErrorCode::LimitExceeded,
                            "the console announced " + std::to_string(length) + " bytes, at most " +
                                std::to_string(limit) + " expected",
                            context);
  }
  session.finishCommand();

  auto state = std::make_unique<FileReader::State>();
  state->session = session_;
  state->path = *canonical;
  state->size = length;
  state->remaining = length;
  state->open = length > 0;
  session.transferActive = state->open;
  return FileReader(std::move(state));
}

Result<FileWriter> XbdmClient::openWrite(const std::string &path, uint64_t size) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  const std::string context = "sendfile " + *canonical;
  if (size > session.options.maxUploadBytes) {
    return fail(ErrorCode::LimitExceeded, context + ": " + std::to_string(size) + " bytes exceed the limit of " +
                                              std::to_string(session.options.maxUploadBytes));
  }
  auto name = nameOf(*canonical);
  auto parent = parentOf(*canonical);
  if (!name || !parent) return fail(ErrorCode::InvalidArgument, context + ": no folder and name");
  auto temp = joinPath(*parent, temporaryName(*name));
  if (!temp) return unexpected<Error>(temp.error());

  auto line = buildLine(Command("sendfile").text("name", *temp).number("length", size), session);
  if (!line) return unexpected<Error>(line.error());
  // What is at the final name now decides whether finish() may replace it.
  auto existing = lookUp(session, *canonical, false);
  if (!existing) return unexpected<Error>(existing.error());
  if (*existing && (*existing)->isDirectory) {
    return fail(ErrorCode::InvalidArgument, context + ": a folder of that name exists");
  }
  if (auto r = session.checkUsable(context, false); !r) return unexpected<Error>(r.error());
  // Whether a console answers a zero-length sendfile with 204 or 200 is not known.
  auto status = size == 0 ? session.request(*line, {status::kSendBinary, status::kOk}, context)
                          : session.request(*line, {status::kSendBinary}, context);
  if (!status) {
    // The command may have reached the console, which may have created the file
    // before its answer was lost; a refusal means it did not.
    if (!isRefusal(status.error())) session.pendingCleanup.push_back(*temp);
    return unexpected<Error>(status.error());
  }
  session.finishCommand();

  auto state = std::make_unique<FileWriter::State>();
  state->session = session_;
  state->path = *canonical;
  state->tempPath = *temp;
  state->size = size;
  state->open = true;
  state->confirmed = status->code == status::kOk;
  state->finalExistedAtOpen = existing->has_value();
  session.transferActive = true;
  return FileWriter(std::move(state));
}

Result<void> XbdmClient::downloadToFile(const std::string &path, const std::filesystem::path &hostPath,
                                        Progress progress) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(path);
  if (!canonical) return unexpected<Error>(canonical.error());
  HostFile out;
  if (auto r = out.open(hostPath); !r) return r;
  auto known = lookUp(**s, *canonical, true);
  if (!known) return unexpected<Error>(known.error());
  std::optional<uint64_t> expectedSize;
  if (*known && (*known)->sizeKnown) expectedSize = (*known)->size;
  auto reader = openRead(*canonical, expectedSize);
  if (!reader) return unexpected<Error>(reader.error());
  std::vector<uint8_t> chunk(static_cast<size_t>(std::min<uint64_t>(kTransferChunkBytes, std::max<uint64_t>(reader->size(), 1))));
  if (progress) progress(0, reader->size());
  for (;;) {
    auto n = reader->read(chunk);
    if (!n) return unexpected<Error>(n.error());
    if (*n == 0) break;
    if (auto r = out.write(std::span<const uint8_t>(chunk.data(), *n)); !r) {
      reader->abort();
      return r;
    }
    if (progress) progress(reader->position(), reader->size());
  }
  return out.commit();
}

Result<void> XbdmClient::uploadFromFile(const std::filesystem::path &hostPath, const std::string &path,
                                        Progress progress) {
  if (hostPath.empty()) return fail(ErrorCode::InvalidArgument, "local path must not be empty");
  const std::string localName = pathToUtf8(hostPath);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(hostPath, ec)) {
    return fail(ErrorCode::InvalidArgument, "'" + localName + "' is not a readable regular file");
  }
  const uint64_t size = std::filesystem::file_size(hostPath, ec);
  if (ec) return fail(ErrorCode::Io, "cannot determine the size of '" + localName + "': " + ec.message(), ec.value());
  std::ifstream in(hostPath, std::ios::binary);
  if (!in) return fail(ErrorCode::Io, "cannot open local file '" + localName + "'");

  auto writer = openWrite(path, size);
  if (!writer) return unexpected<Error>(writer.error());
  std::vector<uint8_t> chunk(static_cast<size_t>(std::min<uint64_t>(kTransferChunkBytes, std::max<uint64_t>(size, 1))));
  if (progress) progress(0, size);
  while (writer->written() < size) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), size - writer->written()));
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(want));
    if (static_cast<size_t>(in.gcount()) != want) {
      writer->abort();
      return fail(ErrorCode::Io, "read from '" + localName + "' failed or the file shrank");
    }
    if (auto r = writer->write(std::span<const uint8_t>(chunk.data(), want)); !r) return r;
    if (progress) progress(writer->written(), size);
  }
  return writer->finish();
}

Result<PowerResult> XbdmClient::launch(const std::string &executablePath) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto canonical = filePath(executablePath);
  if (!canonical) return unexpected<Error>(canonical.error());
  auto directory = parentOf(*canonical);
  if (!directory) return unexpected<Error>(directory.error());
  return power(**s, buildLine(Command("magicboot").text("title", *canonical).text("directory", *directory), **s),
               "magicboot " + *canonical);
}

Result<PowerResult> XbdmClient::reboot(RebootMode mode) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  Command command("magicboot");
  if (mode == RebootMode::Cold) command.flag("cold");
  return power(**s, buildLine(std::move(command), **s), mode == RebootMode::Cold ? "magicboot cold" : "magicboot");
}

Result<PowerResult> XbdmClient::shutdown() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return power(**s, buildLine(Command("shutdown"), **s), "shutdown");
}

Result<void> XbdmClient::ejectTray() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return mutating(**s, "dvdeject", [&]() -> Result<void> {
    auto r = singleLine(**s, buildLine(Command("dvdeject"), **s), "dvdeject");
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<Screenshot> XbdmClient::screenshot() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  constexpr std::string_view context = "screenshot";
  auto line = buildLine(Command("screenshot"), session);
  if (!line) return unexpected<Error>(line.error());
  auto status = session.request(*line, {status::kBinary}, context, session.options.slowIdleTimeout);
  if (!status) return unexpected<Error>(status.error());
  auto geometry = session.readLine(session.options.slowIdleTimeout, context);
  if (!geometry) return unexpected<Error>(geometry.error());

  const Params params = parseParams(*geometry);
  Fields fields;
  const auto pitch = fields.u32(params, "pitch");
  const auto width = fields.u32(params, "width");
  const auto height = fields.u32(params, "height");
  const auto format = fields.u32(params, "format");
  const auto offsetX = fields.u32(params, "offsetx");
  const auto offsetY = fields.u32(params, "offsety");
  const auto bytes = fields.u32(params, "framebuffersize");
  if (!fields.ok || !pitch || !width || !height || !format || !bytes) {
    return session.dropWith(ErrorCode::Protocol, "malformed geometry line '" + detail::preview(*geometry) + "'",
                            context);
  }
  if (*bytes > session.options.maxScreenshotBytes) {
    return session.dropWith(ErrorCode::LimitExceeded,
                            "framebuffersize " + std::to_string(*bytes) + " exceeds the limit of " +
                                std::to_string(session.options.maxScreenshotBytes),
                            context);
  }
  // The frame buffer is tiled in 32x32 blocks, so its height is rounded up.
  const uint64_t tiledBytes = uint64_t{*pitch} * ((uint64_t{*height} + 31) & ~uint64_t{31});
  if (*bytes > tiledBytes) {
    return session.dropWith(ErrorCode::Protocol,
                            "framebuffersize " + std::to_string(*bytes) + " is larger than pitch * height (" +
                                std::to_string(tiledBytes) + ")",
                            context);
  }

  Screenshot shot;
  shot.pitch = *pitch;
  shot.width = *width;
  shot.height = *height;
  shot.format = *format;
  shot.offsetX = offsetX.value_or(0);
  shot.offsetY = offsetY.value_or(0);
  // Grown as the bytes arrive, never allocated from the announced size alone.
  while (shot.data.size() < *bytes) {
    const size_t offset = shot.data.size();
    const size_t piece = std::min<size_t>(kTransferChunkBytes, *bytes - offset);
    shot.data.resize(offset + piece);
    if (auto r = session.readBinaryExact(std::span<uint8_t>(shot.data.data() + offset, piece),
                                         session.options.slowIdleTimeout, context);
        !r) {
      return unexpected<Error>(r.error());
    }
  }
  session.finishCommand();
  return shot;
}

Result<void> XbdmClient::setSystemTime(FileTimePoint time) {
  const auto fileTime = timePointToFileTime(time);
  if (!fileTime) return fail(ErrorCode::InvalidArgument, "setsystime: the time is before 1601");
  return setSystemTimeRaw(*fileTime);
}

Result<void> XbdmClient::setSystemTimeRaw(uint64_t fileTime) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  return mutating(**s, "setsystime", [&]() -> Result<void> {
    auto r = singleLine(**s,
                        buildLine(Command("setsystime")
                                      .number("clockhi", fileTime >> 32)
                                      .number("clocklo", fileTime & 0xFFFFFFFFull),
                                  **s),
                        "setsystime");
    if (!r) return unexpected<Error>(r.error());
    return {};
  });
}

Result<MemoryRead> XbdmClient::getMemory(uint32_t address, uint32_t length) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  if (auto r = checkRange(address, length, session.options.maxMemoryReadBytes, "getmem " + formatNumber(address)); !r) {
    return unexpected<Error>(r.error());
  }
  MemoryRead result;
  result.address = address;
  result.data.reserve(length);
  result.readable.reserve(length);
  for (uint32_t offset = 0; offset < length; offset += kGetMemChunkBytes) {
    const uint32_t piece = std::min(kGetMemChunkBytes, length - offset);
    if (auto r = readMemoryText(session, address + offset, piece, result); !r) return unexpected<Error>(r.error());
  }
  return result;
}

Result<MemoryRead> XbdmClient::getMemoryEx(uint32_t address, uint32_t length) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  const std::string context = "getmemex " + formatNumber(address);
  if (auto r = checkRange(address, length, session.options.maxMemoryReadBytes, context); !r) {
    return unexpected<Error>(r.error());
  }
  auto line = buildLine(Command("getmemex").number("addr", address).number("length", length), session);
  if (!line) return unexpected<Error>(line.error());
  auto status = session.request(*line, {status::kBinary}, context);
  if (!status) return unexpected<Error>(status.error());

  MemoryRead result;
  result.address = address;
  result.data.assign(length, 0);
  result.readable.assign(length, false);
  size_t received = 0;
  while (received < length) {
    uint8_t header[2];
    if (auto r = session.readBinaryExact(header, session.options.idleTimeout, context); !r) {
      return unexpected<Error>(r.error());
    }
    const unsigned word = unsigned{header[0]} | (unsigned{header[1]} << 8);
    const size_t count = word & 0x7FFFu;
    const bool last = (word & 0x8000u) != 0;
    if (count > length - received) {
      return session.dropWith(ErrorCode::Protocol,
                              "a block of " + std::to_string(count) + " bytes where " +
                                  std::to_string(length - received) + " remain",
                              context);
    }
    if (count == 0 && !last) return session.dropWith(ErrorCode::Protocol, "an empty block that is not the last", context);
    if (auto r = session.readBinaryExact(std::span<uint8_t>(result.data.data() + received, count),
                                         session.options.idleTimeout, context);
        !r) {
      return unexpected<Error>(r.error());
    }
    std::fill_n(result.readable.begin() + static_cast<std::ptrdiff_t>(received), count, true);
    received += count;
    if (last) break;
  }
  session.finishCommand();
  return result;
}

Result<void> XbdmClient::setMemory(uint32_t address, std::span<const uint8_t> data) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  if (auto r = checkRange(address, data.size(), UINT32_MAX, "setmem " + formatNumber(address)); !r) return r;
  for (size_t offset = 0; offset < data.size(); offset += kSetMemChunkBytes) {
    const auto piece = data.subspan(offset, std::min(kSetMemChunkBytes, data.size() - offset));
    const uint32_t at = address + static_cast<uint32_t>(offset);
    auto r = mutating(session, "setmem", [&]() -> Result<void> {
      auto line = singleLine(session, buildLine(Command("setmem").number("addr", at).raw("data", formatHex(piece)), session),
                             "setmem " + formatNumber(at));
      if (!line) return unexpected<Error>(line.error());
      return {};
    });
    if (!r) {
      Error error = r.error();
      if (offset > 0) error.message += "; the first " + std::to_string(offset) + " bytes were written";
      return unexpected<Error>(std::move(error));
    }
  }
  return {};
}

Result<std::vector<MemoryRegion>> XbdmClient::memoryRegions() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  std::vector<MemoryRegion> regions;
  auto r = eachLine(**s, buildLine(Command("walkmem"), **s), "walkmem", {status::kMultiline}, [&](const Params &params) {
    Fields fields;
    const auto base = fields.u32(params, "base");
    const auto size = fields.u32(params, "size");
    const auto protect = fields.u32(params, "protect");
    const auto phys = fields.u32(params, "phys");
    if (!fields.ok || !base || !size) return;
    regions.push_back(MemoryRegion{*base, *size, protect.value_or(0), phys.value_or(0)});
  });
  if (!r) return unexpected<Error>(r.error());
  return regions;
}

Result<std::vector<Module>> XbdmClient::modules() {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  std::vector<Module> list;
  auto r = eachLine(**s, buildLine(Command("modules"), **s), "modules", {status::kMultiline}, [&](const Params &params) {
    Fields fields;
    const std::string *name = params.find("name");
    const auto base = fields.u32(params, "base");
    const auto size = fields.u32(params, "size");
    Module module;
    module.checksum = fields.u32(params, "check");
    module.timestamp = fields.u32(params, "timestamp");
    module.originalSize = fields.u32(params, "osize");
    if (!fields.ok || !name || !base || !size) return;
    module.name = *name;
    module.base = *base;
    module.size = *size;
    list.push_back(std::move(module));
  });
  if (!r) return unexpected<Error>(r.error());
  return list;
}

Result<std::vector<ModuleSection>> XbdmClient::moduleSections(const std::string &module) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  if (module.empty()) return fail(ErrorCode::InvalidArgument, "modsections: empty module name");
  std::vector<ModuleSection> sections;
  auto r = eachLine(**s, buildLine(Command("modsections").text("name", module), **s), "modsections " + module,
                    {status::kMultiline}, [&](const Params &params) {
                      Fields fields;
                      const std::string *name = params.find("name");
                      const auto base = fields.u32(params, "base");
                      const auto size = fields.u32(params, "size");
                      ModuleSection section;
                      section.index = fields.u32(params, "index");
                      section.flags = fields.u32(params, "flags");
                      if (!fields.ok || !name || !base || !size) return;
                      section.name = *name;
                      section.base = *base;
                      section.size = *size;
                      sections.push_back(std::move(section));
                    });
  if (!r) return unexpected<Error>(r.error());
  return sections;
}

Result<RawAnswer> XbdmClient::rawCommand(const std::string &line) {
  auto s = sessionOf(session_);
  if (!s) return unexpected<Error>(s.error());
  auto &session = **s;
  constexpr std::string_view context = "raw command";
  if (line.empty() || line.find_first_not_of(' ') == std::string::npos) {
    return fail(ErrorCode::InvalidArgument, "raw command: the line is empty");
  }
  for (char c : line) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return fail(ErrorCode::InvalidArgument, "raw command: only printable ASCII is sent");
  }
  if (line.size() + 2 > session.options.maxCommandBytes) {
    return fail(ErrorCode::LimitExceeded, "raw command: " + std::to_string(line.size() + 2) + " bytes exceed " +
                                              std::to_string(session.options.maxCommandBytes));
  }
  const uint64_t before = session.deliveries;
  auto answer = [&]() -> Result<RawAnswer> {
    session.beginCommand(line);
    if (auto r = session.checkUsable(context, false); !r) return unexpected<Error>(r.error());
    session.startDeadline();
    if (auto r = session.sendLine(line, context); !r) return unexpected<Error>(r.error());
    auto status = session.readStatus(session.options.slowIdleTimeout, context);
    if (!status) return unexpected<Error>(status.error());
    RawAnswer answer{*status, {}};
    if (status->code == status::kMultiline) {
      auto body = session.readBodyLines(context);
      if (!body) return unexpected<Error>(body.error());
      answer.body = std::move(*body);
      return answer;
    }
    if (status->isRefusal() || status->code == status::kOk || status->code == status::kConnected) {
      session.finishCommand();
      return answer;
    }
    return session.dropWith(ErrorCode::Unsupported,
                            "answer '" + std::to_string(status->code) + "- " + detail::preview(status->text) +
                                "' carries data a raw command cannot read; the connection was closed",
                            context);
  }();
  if (!answer && session.delivery) {
    return unexpected<Error>(withDeliveryNote(session, before, session.delivery->command, answer.error()));
  }
  return answer;
}

void registerXbdmScheme(net::TransportRegistry &registry) {
  net::SchemeTraits traits;
  traits.defaultPort = kXbdmPort;
  registry.registerScheme(
      "xbdm",
      [](const net::Endpoint &endpoint) {
        net::Endpoint target = endpoint;
        target.scheme = "tcp";
        if (target.port == 0) target.port = kXbdmPort;
        return net::TcpTransport::connect(target);
      },
      traits);
}

} // namespace updclient::xbdm
