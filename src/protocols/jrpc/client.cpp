#include <protocols/jrpc/client.hpp>

#include <net/tcp_transport.hpp>

#include "net/deadline.hpp"
#include "protocols/jrpc/endpoint.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace updclient::jrpc {

namespace detail {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr size_t kReadChunkBytes = 4096;
// The banner is one short fixed phrase.
constexpr size_t kMaxBannerBytes = 512;
// Error::sysError of an `error=` answer is this plus the RemoteFault.
constexpr int kFaultBase = 1000;
constexpr std::string_view kAnsweredError = "console answered error=";

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

RemoteFault classifyFault(std::string_view text) noexcept {
  if (text.starts_with("Could not resolve function address")) return RemoteFault::CouldNotResolve;
  if (text.starts_with("Version mismatch")) return RemoteFault::VersionMismatch;
  if (text.starts_with("The paramaters were not found")) return RemoteFault::ParametersNotFound;
  return RemoteFault::Other;
}

// An `error=` reply (D4). The text goes at the end of the message.
Error remoteError(std::string_view text, std::string_view context) {
  return makeError(ErrorCode::Io,
                   std::string(context) + ": " + std::string(kAnsweredError) + preview(text),
                   kFaultBase + static_cast<int>(classifyFault(text)));
}

// The stop token the connector built by JrpcClient::connect() uses for its next
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
  Session(ClientOptions opts, JrpcClient::Connector conn) : options(std::move(opts)), connector(std::move(conn)) {}
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  ~Session() { closeConnection(); }

  ClientOptions options;
  JrpcClient::Connector connector;

  // Guards the pointer against cancel() on another thread; the owning thread
  // changes it only under the lock and uses it without.
  mutable std::mutex transportMutex;
  net::TransportPtr transport;
  std::atomic<bool> cancelled{false};
  // Counts cancel() calls, so that a reconnect() can tell one that came while it
  // was connecting.
  std::atomic<uint64_t> cancelEpoch{0};
  // Set when the connector came from JrpcClient::connect(): cancel() then also
  // stops a reconnect() inside its TCP connect. Both under transportMutex.
  std::shared_ptr<ConnectSlot> connectSlot;
  std::stop_source connectStop;

  // One call at a time: set while a public call that uses the connection runs.
  std::atomic<bool> busy{false};

  std::vector<uint8_t> buffer;
  size_t bufferPos = 0;
  // The banner was read on this connection, so "Bye" may be written.
  bool greeted = false;

  std::optional<Clock::time_point> deadline;
  milliseconds deadlineLength{0};
  std::string_view deadlineWhat;
  // The timeout last given to the transport, for messages.
  milliseconds armed{0};

  // How far the last command line got.
  std::optional<CommandDelivery> delivery;

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
    greeted = false;
    return installed;
  }

  // Closes the connection after a failure that leaves the stream position unknown.
  unexpected<Error> drop(Error error) {
    if (transport) transport->close();
    buffer.clear();
    bufferPos = 0;
    deadline.reset();
    greeted = false;
    if (cancelled && error.code != ErrorCode::Cancelled) {
      error.code = ErrorCode::Cancelled;
      error.message = "cancelled (" + error.message + ")";
    }
    spdlog::debug("JRPC connection closed: {}", formatError(error));
    return unexpected<Error>(std::move(error));
  }

  unexpected<Error> dropWith(ErrorCode code, std::string message, std::string_view context) {
    return drop(withContext(makeError(code, std::move(message)), context));
  }

  void trace(TraceEvent event, std::string_view text) const {
    if (!options.trace) return;
    try {
      options.trace(event, text, 0);
    } catch (...) {
    }
  }

  // A public call begins: the delivery describes this command from now on.
  void beginCommand(std::string name) { delivery = CommandDelivery{std::move(name), Delivery::NotSent}; }

  Result<void> checkUsable(std::string_view context) {
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

  unexpected<Error> transportFailure(Error error, std::string_view context) {
    if (error.code == ErrorCode::Timeout) {
      if (deadline && Clock::now() >= *deadline) {
        error.message = std::string(deadlineWhat) + " within " + std::to_string(deadlineLength.count()) + " ms";
      } else {
        error.message = "the console stopped responding (nothing for " + std::to_string(armed.count()) + " ms)";
      }
    } else if (error.code == ErrorCode::NotConnected || cancelled) {
      error.code = ErrorCode::Cancelled;
      error.message = "cancelled: the connection was closed";
    }
    return drop(withContext(std::move(error), context));
  }

  // Sets the transport's timeout to `idle` (zero: none), cut short by the deadline.
  Result<void> arm(milliseconds idle, std::string_view context) {
    milliseconds timeout = idle;
    if (deadline) {
      const auto now = Clock::now();
      if (now >= *deadline) {
        armed = idle;
        return transportFailure(makeError(ErrorCode::Timeout, "deadline"), context);
      }
      const auto remaining = std::max(std::chrono::ceil<milliseconds>(*deadline - now), milliseconds(1));
      if (timeout.count() == 0 || remaining < timeout) timeout = remaining;
    }
    armed = timeout;
    if (auto r = transport->setTimeout(timeout); !r) return transportFailure(r.error(), context);
    return {};
  }

  Result<void> sendBytes(std::span<const uint8_t> data, milliseconds idle, std::string_view context) {
    if (auto r = arm(idle, context); !r) return r;
    if (auto r = transport->writeAll(data); !r) return transportFailure(r.error(), context);
    return {};
  }

  // Its own writeSome loop, with the idle timeout and call deadline, so that delivery
  // knows whether part of the line went out. `track` false leaves the delivery record
  // alone, for the barrier line that follows a silent opcode.
  Result<void> sendLine(std::string_view line, std::string_view context, bool track = true) {
    const std::string wire = formatLine(line);
    trace(TraceEvent::Sent, line);
    std::span<const uint8_t> rest = bytesOf(wire);
    while (!rest.empty()) {
      if (auto r = arm(options.idleTimeout, context); !r) return r;
      auto n = transport->writeSome(rest);
      if (!n) return transportFailure(n.error(), context);
      if (*n == 0) return dropWith(ErrorCode::Io, "write made no progress", context);
      rest = rest.subspan(*n);
      if (track && delivery) delivery->delivery = rest.empty() ? Delivery::Sent : Delivery::PartlySent;
    }
    return {};
  }

  // Appends what the transport has; 0 is end of stream.
  Result<size_t> fill(milliseconds wait, std::string_view context) {
    if (bufferPos == buffer.size()) {
      buffer.clear();
      bufferPos = 0;
    } else if (bufferPos > 0) {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bufferPos));
      bufferPos = 0;
    }
    if (auto r = arm(wait, context); !r) return unexpected<Error>(r.error());
    uint8_t chunk[kReadChunkBytes];
    auto n = transport->readSome(chunk);
    if (!n) return transportFailure(n.error(), context);
    buffer.insert(buffer.end(), chunk, chunk + *n);
    return *n;
  }

  // One line without its terminator (CR LF, or a bare LF). `firstWait` is the longest
  // wait for the first byte of the line, `idle` the longest gap after that; zero is
  // unbounded (the deadline still applies). A line of more than maxBytes, or more
  // bytes than that without an LF, closes the connection.
  Result<std::string> readLine(milliseconds firstWait, milliseconds idle, std::string_view context,
                               size_t maxBytes) {
    // Bytes after bufferPos already searched; fill() keeps them in front.
    size_t scanned = 0;
    for (;;) {
      const auto begin = buffer.begin() + static_cast<std::ptrdiff_t>(bufferPos);
      const auto newline = std::find(begin + static_cast<std::ptrdiff_t>(scanned), buffer.end(), uint8_t{'\n'});
      if (newline != buffer.end()) {
        const std::string_view raw(reinterpret_cast<const char *>(&*begin),
                                   static_cast<size_t>(newline - begin) + 1);
        const std::string_view text = stripTerminator(raw);
        bufferPos = static_cast<size_t>(newline - buffer.begin()) + 1;
        if (text.size() > maxBytes) {
          return dropWith(ErrorCode::LimitExceeded,
                          "the console sent a line of " + std::to_string(text.size()) + " bytes, limit is " +
                              std::to_string(maxBytes),
                          context);
        }
        trace(TraceEvent::Received, text);
        return std::string(text);
      }
      // No LF yet. Even if the next byte were one, the line would be over the limit
      // (one more byte than maxBytes may still be the CR of its terminator).
      if (buffered() > maxBytes + 1) {
        return dropWith(ErrorCode::LimitExceeded,
                        "the console sent more than " + std::to_string(maxBytes) + " bytes without ending the line",
                        context);
      }
      const size_t pending = buffered();
      scanned = pending;
      auto n = fill(pending == 0 ? firstWait : idle, context);
      if (!n) return unexpected<Error>(n.error());
      if (*n == 0) {
        return dropWith(ErrorCode::Disconnected,
                        pending == 0 ? "the console closed the connection"
                                     : "the console closed the connection in the middle of a line",
                        context);
      }
    }
  }

  // Reads the banner of a connection that was just made.
  Result<void> greet() {
    startDeadline(options.bannerTimeout, "the banner did not arrive");
    auto line = readLine(options.bannerTimeout, options.bannerTimeout, "connect",
                         std::min(options.maxReplyBytes, kMaxBannerBytes));
    if (!line) {
      Error error = line.error();
      if (error.code == ErrorCode::Timeout) {
        error.message += " (the console serves 8 connections at a time; a 9th waits for a slot)";
      }
      return unexpected<Error>(std::move(error));
    }
    deadline.reset();
    if (*line == kBanner) {
      greeted = true;
      return {};
    }
    if (isDebugLine(*line)) {
      return dropWith(ErrorCode::Unsupported, "JRPC is not installed: the console greeted with '" + preview(*line) + "'",
                      "connect");
    }
    return dropWith(ErrorCode::Protocol,
                    "the console greeted with '" + preview(*line) + "', expected '" + std::string(kBanner) + "'",
                    "connect");
  }

  // Reads one reply line. A line containing DEBUG closes the connection (not JRPC); an
  // `error=` line is returned like any other, for the caller to treat as the answer or
  // as an error (the connection stays). The delivery becomes Answered once a line was
  // read.
  Result<std::string> readReply(std::string_view context) {
    // A called function may run long: the first byte can take as long as the whole
    // call is allowed to, then the reply has to keep moving.
    auto reply = readLine(milliseconds(0), options.idleTimeout, context, options.maxReplyBytes);
    if (!reply) return unexpected<Error>(reply.error());
    if (delivery) delivery->delivery = Delivery::Answered;
    if (!isErrorLine(*reply) && isDebugLine(*reply)) {
      return dropWith(ErrorCode::Unsupported, "JRPC is not installed: the console answered '" + preview(*reply) + "'",
                      context);
    }
    return reply;
  }

  // Sends one command line and reads the one reply line (see readReply).
  Result<std::string> exchange(const std::string &line, std::string_view context) {
    if (auto r = checkUsable(context); !r) return unexpected<Error>(r.error());
    startDeadline(options.callTimeout, "the call did not complete");
    if (auto r = sendLine(line, context); !r) return unexpected<Error>(r.error());
    auto reply = readReply(context);
    if (!reply) return unexpected<Error>(reply.error());
    deadline.reset();
    return reply;
  }

  // An opcode the console may not answer (XNotify, SetLeds, ConstantMemorySet; D6).
  // Without the barrier the line is sent and that is all. With it, ConsoleType is sent
  // right behind the line and lines are read up to its answer: the console handles the
  // lines of a connection in order, so whatever comes before the console type name is
  // the opcode's own answer, at most one line, `S_OK` or hex or `error=` (an `error=`
  // fails the call once the barrier is read, so the stream stays in step). The name is
  // the sentinel because no answer of the opcode can look like one, which a number
  // (the kernel version, say) could not promise: the opcode may answer `0`.
  Result<void> sendSilent(const std::string &line, std::string_view context) {
    std::string barrier;
    if (options.silentOpBarrier) {
      auto built = buildOpcodeCommand(Opcode::ConsoleType, {}, 0, options.maxCommandBytes);
      if (!built) return unexpected<Error>(withContext(built.error(), context));
      barrier = std::move(*built);
    }
    if (auto r = checkUsable(context); !r) return r;
    startDeadline(options.callTimeout, "the command did not complete");
    if (auto r = sendLine(line, context); !r) return r;
    if (!options.silentOpBarrier) {
      deadline.reset();
      return {};
    }
    if (auto r = sendLine(barrier, context, false); !r) return r;
    std::optional<std::string> answer;
    for (;;) {
      auto reply = readReply(context);
      if (!reply) return unexpected<Error>(reply.error());
      if (!isErrorLine(*reply) && parseConsoleType(*reply)) break;
      if (answer) {
        return dropWith(ErrorCode::Protocol,
                        "two lines ('" + preview(*answer) + "' and '" + preview(*reply) + "') answered one command",
                        context);
      }
      if (!isErrorLine(*reply) && !parseVoidReply(*reply)) {
        return dropWith(ErrorCode::Protocol,
                        "the console answered '" + preview(*reply) + "' where nothing, S_OK, a number or error= was expected",
                        context);
      }
      answer = std::move(*reply);
    }
    deadline.reset();
    if (answer && isErrorLine(*answer)) return unexpected<Error>(remoteError(errorText(*answer), context));
    return {};
  }

  // ShutDownConsole: sends the line, then closes the connection without waiting for
  // anything and without Bye, because the console is going down (D6).
  Result<void> sendAndLeave(const std::string &line, std::string_view context) {
    if (auto r = checkUsable(context); !r) return r;
    startDeadline(options.callTimeout, "the command did not complete");
    if (auto r = sendLine(line, context); !r) return r;
    (void)drop(makeError(ErrorCode::Disconnected, std::string(context) + ": the console is going away"));
    return {};
  }

  // A failed command that was sent but not answered may still have run.
  Error withDeliveryNote(Error error) const {
    if (delivery && (delivery->delivery == Delivery::PartlySent || delivery->delivery == Delivery::Sent)) {
      error.message += "; the command was sent but not answered, so the console may have carried it out";
    }
    return error;
  }

  // "Bye" gets no answer, so this only writes it. Stray bytes still unread do not
  // matter: the server reads lines, whatever state our side of the stream is in.
  void sayBye() noexcept {
    if (!greeted || !connected()) return;
    startDeadline(options.byeTimeout, "the Bye did not go out");
    trace(TraceEvent::Sent, kBye);
    (void)sendBytes(bytesOf("Bye\r\n"), options.byeTimeout, "Bye");
    deadline.reset();
  }

  void closeConnection() noexcept {
    try {
      sayBye();
    } catch (...) {
    }
    if (transport) transport->close();
    buffer.clear();
    bufferPos = 0;
    deadline.reset();
    greeted = false;
  }
};

// Marks the session as in use for the length of a public call.
class BusyGuard {
public:
  explicit BusyGuard(Session &session) : session_(session), owns_(!session.busy.exchange(true)) {}
  BusyGuard(const BusyGuard &) = delete;
  BusyGuard &operator=(const BusyGuard &) = delete;
  ~BusyGuard() {
    if (owns_) session_.busy = false;
  }
  explicit operator bool() const noexcept { return owns_; }

private:
  Session &session_;
  bool owns_;
};

Error busyError(std::string_view what) {
  return makeError(ErrorCode::InvalidArgument,
                   std::string(what) + ": another call is in progress on this connection; wait for it, or open a "
                                       "second client");
}

} // namespace detail

using detail::BusyGuard;
using detail::Session;

std::optional<RemoteFault> remoteFault(const Error &error) noexcept {
  if (error.code != ErrorCode::Io || error.sysError < detail::kFaultBase ||
      error.sysError > detail::kFaultBase + static_cast<int>(RemoteFault::ParametersNotFound)) {
    return std::nullopt;
  }
  if (error.message.find(detail::kAnsweredError) == std::string::npos) return std::nullopt;
  return static_cast<RemoteFault>(error.sysError - detail::kFaultBase);
}

// ---------------------------------------------------------------------------
// JrpcClient

JrpcClient::JrpcClient(std::unique_ptr<detail::Session> session) : session_(std::move(session)) {}
JrpcClient::JrpcClient(JrpcClient &&) noexcept = default;
JrpcClient &JrpcClient::operator=(JrpcClient &&other) noexcept {
  if (this != &other) {
    if (session_) session_->closeConnection();
    session_ = std::move(other.session_);
  }
  return *this;
}

JrpcClient::~JrpcClient() {
  if (session_) session_->closeConnection();
}

namespace {

Result<net::TransportPtr> connectCancelled(std::string_view what) {
  return fail(ErrorCode::Cancelled, "connect to " + std::string(what) + " cancelled");
}

// Connects through the connector, unless the token is already stopped, and reads the
// banner; a stop request during the banner cancels the session. The connector itself
// runs to its own end, so a token reaches it only through slot.
Result<std::unique_ptr<Session>> openSession(JrpcClient::Connector connector, ClientOptions options,
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
  auto session = std::make_unique<Session>(std::move(options), std::move(connector));
  session->connectSlot = std::move(slot);
  session->install(std::move(*transport), 0);
  {
    std::stop_callback onStop(stop, [raw = session.get()] { raw->cancel(); });
    if (auto r = session->greet(); !r) return unexpected<Error>(r.error());
  }
  if (session->cancelled) {
    session->closeConnection();
    return fail(ErrorCode::Cancelled, "connect cancelled after the banner");
  }
  spdlog::debug("connected to JRPC at {}", session->transport->describe());
  return session;
}

} // namespace

Result<JrpcClient> JrpcClient::connect(const net::Endpoint &endpoint, ClientOptions options) {
  return connect(endpoint, std::move(options), std::stop_token());
}

Result<JrpcClient> JrpcClient::connect(const net::Endpoint &endpoint, ClientOptions options, std::stop_token stop) {
  net::Endpoint target = endpoint;
  std::string scheme = target.scheme;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto slot = std::make_shared<detail::ConnectSlot>();
  Connector connector;
  if (scheme.empty() || scheme == "jrpc") {
    target.scheme = "tcp";
    if (target.port == 0) target.port = kJrpcPort;
    connector = [target, slot] { return net::TcpTransport::connect(target, slot->get()); };
  } else {
    // A registry connector cannot be interrupted; the token is checked around it.
    target = net::TransportRegistry::instance().withDefaultPort(target, kJrpcPort);
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
  return JrpcClient(std::move(*session));
}

Result<JrpcClient> JrpcClient::open(Connector connector, ClientOptions options) {
  return open(std::move(connector), std::move(options), std::stop_token());
}

Result<JrpcClient> JrpcClient::open(Connector connector, ClientOptions options, std::stop_token stop) {
  auto session = openSession(std::move(connector), std::move(options), std::move(stop), nullptr);
  if (!session) return unexpected<Error>(session.error());
  return JrpcClient(std::move(*session));
}

Result<JrpcClient> JrpcClient::attach(net::TransportPtr transport, ClientOptions options, Connector connector) {
  if (!transport) return fail(ErrorCode::ConnectFailed, "no transport");
  auto session = std::make_unique<Session>(std::move(options), std::move(connector));
  session->install(std::move(transport), 0);
  if (auto r = session->greet(); !r) return unexpected<Error>(r.error());
  spdlog::debug("connected to JRPC at {}", session->transport->describe());
  return JrpcClient(std::move(session));
}

bool JrpcClient::isConnected() const noexcept {
  if (!session_) return false;
  std::lock_guard<std::mutex> lock(session_->transportMutex);
  return session_->connected();
}

std::string JrpcClient::describe() const {
  if (!session_ || !session_->transport) return "<not connected>";
  return session_->transport->describe();
}

std::optional<net::Endpoint> JrpcClient::peer() const {
  if (!session_) return std::nullopt;
  std::lock_guard<std::mutex> lock(session_->transportMutex);
  if (!session_->connected()) return std::nullopt;
  const auto *tcp = dynamic_cast<const net::TcpTransport *>(session_->transport.get());
  if (!tcp) return std::nullopt;
  return tcp->peer();
}

const ClientOptions &JrpcClient::options() const noexcept {
  static const ClientOptions defaults;
  return session_ ? session_->options : defaults;
}

void JrpcClient::setOptions(const ClientOptions &options) {
  if (session_) session_->options = options;
}

std::optional<CommandDelivery> JrpcClient::lastDelivery() const {
  return session_ ? session_->delivery : std::nullopt;
}

Result<void> JrpcClient::reconnect() {
  if (!session_) return fail(ErrorCode::NotConnected, "the client was moved from");
  auto &s = *session_;
  BusyGuard guard(s);
  if (!guard) return unexpected<Error>(detail::busyError("reconnect"));
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
  return s.greet();
}

void JrpcClient::close() noexcept {
  if (!session_) return;
  BusyGuard guard(*session_);
  if (!guard) {
    // A call (or a reconnect) is using the connection on another thread: writing "Bye"
    // now could land in the middle of its command, so end it the way cancel() does.
    session_->cancel();
    return;
  }
  session_->closeConnection();
}

void JrpcClient::cancel() noexcept {
  if (session_) session_->cancel();
}

Result<CallResult> JrpcClient::call(const CallSpec &spec) { return callAs(spec, std::nullopt, "call"); }

namespace {

std::string_view kindName(ReturnKind kind) noexcept {
  switch (kind) {
  case ReturnKind::Void: return "Void";
  case ReturnKind::Int: return "Int";
  case ReturnKind::String: return "String";
  case ReturnKind::Float: return "Float";
  case ReturnKind::Byte: return "Byte";
  case ReturnKind::IntArray: return "IntArray";
  case ReturnKind::FloatArray: return "FloatArray";
  case ReturnKind::ByteArray: return "ByteArray";
  case ReturnKind::Int64: return "Int64";
  case ReturnKind::Uint64Array: return "Uint64Array";
  }
  return "?";
}

} // namespace

Result<CallResult> JrpcClient::callAs(const CallSpec &spec, std::optional<ReturnKind> expected, std::string_view helper) {
  if (!session_) return fail(ErrorCode::NotConnected, "the client was moved from");
  auto &s = *session_;
  BusyGuard guard(s);
  if (!guard) return unexpected<Error>(detail::busyError(helper));
  const std::string_view context = helper;
  s.beginCommand("call");

  const CallSpec *use = &spec;
  CallSpec adjusted;
  if (expected && spec.returns != *expected) {
    if (spec.returns != ReturnKind::Void) {
      return fail(ErrorCode::InvalidArgument, std::string(context) + ": the spec asks for a " +
                                                  std::string(kindName(spec.returns)) + " return, this reads a " +
                                                  std::string(kindName(*expected)));
    }
    adjusted = spec;
    adjusted.returns = *expected;
    use = &adjusted;
  }

  auto line = buildCommand(*use, s.options.maxCommandBytes, s.options.maxArrayElements);
  if (!line) return unexpected<Error>(detail::withContext(line.error(), context));

  auto reply = s.exchange(*line, context);
  if (!reply) return unexpected<Error>(s.withDeliveryNote(reply.error()));
  if (isErrorLine(*reply)) return unexpected<Error>(detail::remoteError(errorText(*reply), context));

  auto value = parseReply(use->returns, *reply, use->arraySize);
  if (!value) return s.drop(detail::withContext(value.error(), context));
  return CallResult{std::move(*value), std::move(*reply)};
}

Result<RawAnswer> JrpcClient::rawCommand(const std::string &line) {
  if (!session_) return fail(ErrorCode::NotConnected, "the client was moved from");
  auto &s = *session_;
  BusyGuard guard(s);
  if (!guard) return unexpected<Error>(detail::busyError("raw command"));
  constexpr std::string_view context = "raw command";
  s.beginCommand("raw");

  if (line.empty() || line.find_first_not_of(' ') == std::string::npos) {
    return fail(ErrorCode::InvalidArgument, "raw command: the line is empty");
  }
  for (char c : line) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return fail(ErrorCode::InvalidArgument, "raw command: only printable ASCII is sent");
  }
  if (line.size() > s.options.maxCommandBytes) {
    return fail(ErrorCode::LimitExceeded, "raw command: " + std::to_string(line.size()) + " bytes exceed " +
                                              std::to_string(s.options.maxCommandBytes));
  }

  auto reply = s.exchange(line, context);
  if (!reply) return unexpected<Error>(s.withDeliveryNote(reply.error()));
  RawAnswer answer;
  answer.isError = isErrorLine(*reply);
  answer.line = std::move(*reply);
  return answer;
}

// ---------------------------------------------------------------------------
// Typed calls

namespace {

// The value of a call, as the type its kind decodes to.
template <class T> Result<T> valueOf(Result<CallResult> result, std::string_view helper) {
  if (!result) return unexpected<Error>(result.error());
  if (auto *value = std::get_if<T>(&result->value)) return std::move(*value);
  return fail(ErrorCode::Unknown, std::string(helper) + ": internal error, the decoded value has another type");
}

} // namespace

Result<void> JrpcClient::callVoid(const CallSpec &spec) {
  auto result = callAs(spec, ReturnKind::Void, "callVoid");
  if (!result) return unexpected<Error>(result.error());
  return {};
}

Result<int32_t> JrpcClient::callInt32(const CallSpec &spec) {
  auto value = valueOf<uint64_t>(callAs(spec, ReturnKind::Int, "callInt32"), "callInt32");
  if (!value) return unexpected<Error>(value.error());
  return static_cast<int32_t>(static_cast<uint32_t>(*value));
}

Result<uint32_t> JrpcClient::callUInt32(const CallSpec &spec) {
  auto value = valueOf<uint64_t>(callAs(spec, ReturnKind::Int, "callUInt32"), "callUInt32");
  if (!value) return unexpected<Error>(value.error());
  return static_cast<uint32_t>(*value);
}

Result<uint8_t> JrpcClient::callByte(const CallSpec &spec) {
  auto value = valueOf<uint64_t>(callAs(spec, ReturnKind::Byte, "callByte"), "callByte");
  if (!value) return unexpected<Error>(value.error());
  return static_cast<uint8_t>(*value);
}

Result<int64_t> JrpcClient::callInt64(const CallSpec &spec) {
  auto value = valueOf<uint64_t>(callAs(spec, ReturnKind::Int64, "callInt64"), "callInt64");
  if (!value) return unexpected<Error>(value.error());
  return static_cast<int64_t>(*value);
}

Result<uint64_t> JrpcClient::callUInt64(const CallSpec &spec) {
  return valueOf<uint64_t>(callAs(spec, ReturnKind::Int64, "callUInt64"), "callUInt64");
}

Result<double> JrpcClient::callFloat(const CallSpec &spec) {
  return valueOf<double>(callAs(spec, ReturnKind::Float, "callFloat"), "callFloat");
}

Result<std::string> JrpcClient::callString(const CallSpec &spec) {
  return valueOf<std::string>(callAs(spec, ReturnKind::String, "callString"), "callString");
}

Result<std::vector<uint8_t>> JrpcClient::callBytes(const CallSpec &spec) {
  return valueOf<std::vector<uint8_t>>(callAs(spec, ReturnKind::ByteArray, "callBytes"), "callBytes");
}

Result<std::vector<int32_t>> JrpcClient::callInts(const CallSpec &spec) {
  return valueOf<std::vector<int32_t>>(callAs(spec, ReturnKind::IntArray, "callInts"), "callInts");
}

Result<std::vector<double>> JrpcClient::callFloats(const CallSpec &spec) {
  return valueOf<std::vector<double>>(callAs(spec, ReturnKind::FloatArray, "callFloats"), "callFloats");
}

// ---------------------------------------------------------------------------
// System opcodes

namespace {

// Takes the busy flag and starts the delivery record for `opcode`, then runs the body.
template <class T, class Body>
Result<T> runOpcode(const std::unique_ptr<Session> &session, Opcode opcode, Body &&body) {
  if (!session) return fail(ErrorCode::NotConnected, "the client was moved from");
  Session &s = *session;
  BusyGuard guard(s);
  if (!guard) return unexpected<Error>(detail::busyError(opcodeName(opcode)));
  s.beginCommand(std::string(opcodeName(opcode)));
  return body(s, opcodeName(opcode));
}

// An opcode that answers one line: sends it and returns the line. `error=` is a remote
// fault and keeps the connection (D4).
Result<std::string> askOpcode(Session &s, Opcode opcode, std::span<const Arg> args = {}) {
  const std::string_view context = opcodeName(opcode);
  auto line = buildOpcodeCommand(opcode, args, 0, s.options.maxCommandBytes);
  if (!line) return unexpected<Error>(detail::withContext(line.error(), context));
  auto reply = s.exchange(*line, context);
  if (!reply) return unexpected<Error>(s.withDeliveryNote(reply.error()));
  if (isErrorLine(*reply)) return unexpected<Error>(detail::remoteError(errorText(*reply), context));
  return reply;
}

// A reply of the wrong shape for its opcode closes the connection (D3).
template <class T, class Parse>
Result<T> readValue(Session &s, std::string_view context, const std::string &reply, Parse &&parse) {
  auto value = parse(std::string_view(reply));
  if (!value) return s.drop(detail::withContext(value.error(), context));
  return std::move(*value);
}

// An opcode that may not answer: the barrier of Session::sendSilent decides what a
// reply means. A failure that left the command sent but unanswered says so.
Result<void> sendSilentOpcode(Session &s, Opcode opcode, std::span<const Arg> args, uint32_t address) {
  const std::string_view context = opcodeName(opcode);
  auto line = buildOpcodeCommand(opcode, args, address, s.options.maxCommandBytes);
  if (!line) return unexpected<Error>(detail::withContext(line.error(), context));
  if (auto r = s.sendSilent(*line, context); !r) return unexpected<Error>(s.withDeliveryNote(r.error()));
  return {};
}

bool isHexText(std::string_view text) noexcept {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

Result<uint32_t> JrpcClient::resolveFunction(const std::string &module, uint32_t ordinal) {
  return runOpcode<uint32_t>(session_, Opcode::ResolveFunction, [&](Session &s, std::string_view context) -> Result<uint32_t> {
    if (module.empty()) return fail(ErrorCode::InvalidArgument, std::string(context) + ": the module name is empty");
    const Arg args[] = {Arg::string(module), Arg::u32(ordinal)};
    auto reply = askOpcode(s, Opcode::ResolveFunction, args);
    if (!reply) return unexpected<Error>(reply.error());
    return readValue<uint32_t>(s, context, *reply, parseIntReply);
  });
}

Result<CpuKey> JrpcClient::cpuKey() {
  return runOpcode<CpuKey>(session_, Opcode::GetCpuKey, [&](Session &s, std::string_view context) -> Result<CpuKey> {
    auto reply = askOpcode(s, Opcode::GetCpuKey);
    if (!reply) return unexpected<Error>(reply.error());
    auto key = parseCpuKey(*reply);
    if (key) return std::move(*key);
    // Hex digits of the wrong count: the halves lost their leading zeros (D8). The line
    // is a well-formed reply and the stream is in step, so only the call fails.
    if (isHexText(*reply) && reply->size() <= 32) return unexpected<Error>(detail::withContext(key.error(), context));
    return s.drop(detail::withContext(key.error(), context));
  });
}

Result<void> JrpcClient::shutdown() {
  return runOpcode<void>(session_, Opcode::ShutDownConsole, [&](Session &s, std::string_view context) -> Result<void> {
    auto line = buildOpcodeCommand(Opcode::ShutDownConsole, {}, 0, s.options.maxCommandBytes);
    if (!line) return unexpected<Error>(detail::withContext(line.error(), context));
    if (auto r = s.sendAndLeave(*line, context); !r) return unexpected<Error>(s.withDeliveryNote(r.error()));
    return {};
  });
}

Result<void> JrpcClient::notify(const std::string &text, uint32_t type) {
  return runOpcode<void>(session_, Opcode::XNotify, [&](Session &s, std::string_view context) -> Result<void> {
    if (text.empty()) return fail(ErrorCode::InvalidArgument, std::string(context) + ": the text is empty");
    const Arg args[] = {Arg::string(text), Arg::u32(type)};
    return sendSilentOpcode(s, Opcode::XNotify, args, 0);
  });
}

Result<uint32_t> JrpcClient::kernelVersion() {
  return runOpcode<uint32_t>(session_, Opcode::GetKernelVersion, [&](Session &s, std::string_view context) -> Result<uint32_t> {
    auto reply = askOpcode(s, Opcode::GetKernelVersion);
    if (!reply) return unexpected<Error>(reply.error());
    auto version = readValue<int32_t>(s, context, *reply, parseDecimalReply);
    if (!version) return unexpected<Error>(version.error());
    if (*version < 0) {
      return s.drop(makeError(ErrorCode::Protocol, std::string(context) + ": a negative kernel version '" + *reply + "'"));
    }
    return static_cast<uint32_t>(*version);
  });
}

Result<void> JrpcClient::setLeds(LedState topLeft, LedState topRight, LedState bottomLeft, LedState bottomRight) {
  return runOpcode<void>(session_, Opcode::SetLeds, [&](Session &s, std::string_view) -> Result<void> {
    const Arg args[] = {Arg::u32(static_cast<uint32_t>(topLeft)), Arg::u32(static_cast<uint32_t>(topRight)),
                        Arg::u32(static_cast<uint32_t>(bottomLeft)), Arg::u32(static_cast<uint32_t>(bottomRight))};
    return sendSilentOpcode(s, Opcode::SetLeds, args, 0);
  });
}

Result<uint32_t> JrpcClient::temperature(TemperatureSensor sensor) {
  return runOpcode<uint32_t>(session_, Opcode::GetTemperature, [&](Session &s, std::string_view context) -> Result<uint32_t> {
    if (static_cast<uint32_t>(sensor) > static_cast<uint32_t>(TemperatureSensor::Mainboard)) {
      return fail(ErrorCode::InvalidArgument, std::string(context) + ": sensor " +
                                                  std::to_string(static_cast<uint32_t>(sensor)) +
                                                  " is not one of the four (0 to 3)");
    }
    const Arg args[] = {Arg::u32(static_cast<uint32_t>(sensor))};
    auto reply = askOpcode(s, Opcode::GetTemperature, args);
    if (!reply) return unexpected<Error>(reply.error());
    return readValue<uint32_t>(s, context, *reply, parseIntReply);
  });
}

Result<uint32_t> JrpcClient::currentTitleId() {
  return runOpcode<uint32_t>(session_, Opcode::GetCurrentTitleId, [&](Session &s, std::string_view context) -> Result<uint32_t> {
    auto reply = askOpcode(s, Opcode::GetCurrentTitleId);
    if (!reply) return unexpected<Error>(reply.error());
    return readValue<uint32_t>(s, context, *reply, parseIntReply);
  });
}

Result<ConsoleType> JrpcClient::consoleType() {
  return runOpcode<ConsoleType>(session_, Opcode::ConsoleType, [&](Session &s, std::string_view context) -> Result<ConsoleType> {
    auto reply = askOpcode(s, Opcode::ConsoleType);
    if (!reply) return unexpected<Error>(reply.error());
    return readValue<ConsoleType>(s, context, *reply, parseConsoleType);
  });
}

Result<void> JrpcClient::constantMemorySet(uint32_t address, uint32_t value, std::optional<uint32_t> onlyIfValue,
                                           std::optional<uint32_t> inTitle) {
  return runOpcode<void>(session_, Opcode::ConstantMemorySet, [&](Session &s, std::string_view context) -> Result<void> {
    if (address == 0) return fail(ErrorCode::InvalidArgument, std::string(context) + ": the address is 0");
    // Section 4.2: the value, then a flag and the value for each guard.
    const Arg args[] = {Arg::u32(value), Arg::u32(onlyIfValue ? 1 : 0), Arg::u32(onlyIfValue.value_or(0)),
                        Arg::u32(inTitle ? 1 : 0), Arg::u32(inTitle.value_or(0))};
    return sendSilentOpcode(s, Opcode::ConstantMemorySet, args, address);
  });
}

Result<IdentifyResult> identify(const net::Endpoint &endpoint, ClientOptions options) {
  return identify(endpoint, std::move(options), std::stop_token());
}

Result<IdentifyResult> identify(const net::Endpoint &endpoint, ClientOptions options, std::stop_token stop) {
  auto client = JrpcClient::connect(endpoint, std::move(options), std::move(stop));
  if (!client) return unexpected<Error>(client.error());
  IdentifyResult result;
  result.endpoint = withDefaultJrpcPort(endpoint);
  // connect() accepted the banner, and it accepts nothing but this.
  result.banner = std::string(kBanner);
  client->close();
  return result;
}

void registerJrpcScheme(net::TransportRegistry &registry) {
  net::SchemeTraits traits;
  traits.defaultPort = kJrpcPort;
  registry.registerScheme(
      "jrpc",
      [](const net::Endpoint &endpoint) {
        net::Endpoint target = endpoint;
        target.scheme = "tcp";
        if (target.port == 0) target.port = kJrpcPort;
        return net::TcpTransport::connect(target);
      },
      traits);
}

} // namespace updclient::jrpc
