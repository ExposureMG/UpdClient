#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>
#include <net/transport_registry.hpp>
#include <protocols/jrpc/protocol.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::jrpc {

namespace detail {
struct Session;
} // namespace detail

// What a trace hook is told, on the thread doing the I/O: every command line sent and
// every line received (the banner included), as text without CR LF. JRPC has no binary
// data, so `bytes` is always 0; the parameter keeps the shape of xbdm::TraceHook so a
// caller can share one function.
enum class TraceEvent { Sent, Received };
using TraceHook = std::function<void(TraceEvent event, std::string_view text, uint64_t bytes)>;

// Every timeout is idle-based unless it says otherwise: the longest wait for the next
// byte (or for room to send one), restarted whenever data moves. Zero disables one,
// which lets a wait go on forever; the defaults never do, except where noted.
struct ClientOptions {
  // For the banner line after the connection is made, as a whole.
  std::chrono::milliseconds bannerTimeout{5000};
  // Inside a reply: the longest gap between two bytes once the reply has begun, and
  // the longest wait for room while a command is being written.
  std::chrono::milliseconds idleTimeout{10000};
  // Upper bound for one whole call, from sending the command to the last byte of the
  // reply. A called function may run long, and nothing arrives until it returns, so
  // this is also how long the client waits for the first byte of a reply. 0 means no
  // bound at all: a call then waits until the console answers, the connection drops
  // or cancel() is called.
  std::chrono::milliseconds callTimeout{60000};
  // For the "Bye" written by close(), as a whole. Bye gets no answer, so this bounds
  // the write only.
  std::chrono::milliseconds byeTimeout{500};

  // The longest command line sent, without its terminator (section 1.3).
  size_t maxCommandBytes = kMaxCommandBytes;
  // The longest reply line accepted, without its terminator. The banner is limited to
  // 512 bytes (or this, if lower).
  size_t maxReplyBytes = 64 * 1024;
  // Raised only if hardware shows that the server sends more than 8 array elements
  // (open question 1).
  size_t maxArrayElements = kMaxArrayElements;

  // D6. Whether the console answers the opcodes XNotify, SetLeds and ConstantMemorySet
  // is not known (hardware checklist item 2). On: after sending one of them the client
  // also sends ConsoleType (opcode 17, read-only) and reads lines until its answer, a
  // name no opcode answer can look like. A line before it is that opcode's answer
  // (`error=` fails the call, `S_OK` or hex is accepted), so a console that answers
  // never leaves a stray line to be mistaken for the next call's reply, and one that
  // stays silent costs one extra round trip. Off: the client sends the opcode and
  // returns at once; use that only once hardware shows the opcodes never answer,
  // because a reply that does arrive is then read as the answer to the next command
  // (a plausible number such as `0` would even pass for one). ShutDownConsole is
  // exempt either way: the client closes the connection after sending it.
  bool silentOpBarrier = true;

  // Protocol trace, for diagnosing a console; empty for none.
  TraceHook trace;
};

// How far a command line got (JrpcClient::lastDelivery). NotSent: no byte left the
// client, so the console cannot have run it. PartlySent: part of the line was written;
// Sent: all of it. In both cases the console may have carried it out. Answered: a
// reply line was read (an `error=` line and a line of the wrong shape included). For
// XNotify, SetLeds and ConstantMemorySet with silentOpBarrier, the line that ends the
// wait counts: the console got through the opcode before it answered the barrier. Without
// the barrier, and for ShutDownConsole, such a command stays Sent: nothing is read.
enum class Delivery { NotSent, PartlySent, Sent, Answered };

struct CommandDelivery {
  // "call" for a generic call, the opcode's name for a system opcode, "raw" for
  // rawCommand(); never the arguments.
  std::string command;
  Delivery delivery = Delivery::NotSent;
};

// A successful call: the decoded value (by the ReturnKind that was asked for) and the
// reply line as the console sent it, without its terminator, for diagnostics.
struct CallResult {
  CallValue value;
  std::string line;
};

// The answer to a line sent as typed (JrpcClient::rawCommand): the reply line without
// its terminator. isError is set for a line starting `error=`, which here is an
// answer, not a failure.
struct RawAnswer {
  std::string line;
  bool isError = false;
};

// The raw values SetLeds sends for each of the four LEDs around the ring. The document
// gives no encoding; these are the ones the JRPC2 client's LEDState enum uses, so they
// are a starting point and not verified for this server (hardware checklist item 7).
// Any other value is reachable with static_cast<LedState>(n) and is sent as it is.
enum class LedState : uint32_t { Off = 0x00, Red = 0x08, Green = 0x80, Orange = 0x88 };

// GetTemperature's index (section 4.2). The reply is the raw `%X` value; its unit is not
// documented, so temperature() returns it unconverted. Only these four are sent: the
// console's table has no other entries that anything is known about.
enum class TemperatureSensor : uint32_t { Cpu = 0, Gpu = 1, Edram = 2, Mainboard = 3 };

// The kinds of `error=` text this library tells apart (D4). The server's wording on the
// TCP build is not known; these are the JRPC2 build's texts (section 6 and the plan).
enum class RemoteFault {
  // Any other text.
  Other,
  // "Could not resolve function address..."
  CouldNotResolve,
  // "Version mismatch"
  VersionMismatch,
  // "The paramaters were not found" (the typo is the server's)
  ParametersNotFound,
};

// For an Error this client raised because the console answered `error=`: which kind of
// fault it was. The code is ErrorCode::Io, Error::sysError is 1000 plus the
// enumerator, and the console's text is at the end of Error::message
// ("...: console answered error=Version mismatch"). nullopt for every other error,
// including transport errors whose sysError is an OS error number.
UPDCLIENT_API std::optional<RemoteFault> remoteFault(const Error &error) noexcept;

// Client for JRPC on TCP port 1409. One call at a time and no pipelining: a command is
// sent only after the previous reply was read. Not thread-safe; the exceptions are
// cancel() and isConnected(), which any thread may use, and close(), which from another
// thread while a call is in progress acts as cancel(). A second call made while one is
// in progress (from another thread, or from a trace hook) is refused with
// InvalidArgument and does not disturb the first; for calls in parallel open a second
// client (the console serves 8 connections, a 9th waits for a slot).
//
// Failures. A reply has no status prefix, so after anything unexpected the position in
// the stream is unknown (D3). A timeout, a reply of the wrong shape for the `type`
// sent, a line over maxReplyBytes, a line containing DEBUG, bytes the console sent
// that belong to no command, a dropped connection and cancel() close the connection:
// isConnected() turns false and reconnect() opens a new one when the caller decides to;
// the client never reconnects on its own. A call that timed out may still be running
// on the console, which is why its connection is never reused. The one failure that
// keeps the connection is an `error=` reply (D4): it is a well-formed answer to that
// command. It is an Error with ErrorCode::Io and remoteFault() set.
//
// Replies end in CR LF; a bare LF is tolerated. A CR alone is not a terminator, so such
// a reply never completes and ends in a timeout.
//
// Before a client exists the codes mean: ConnectFailed, no connection could be made
// (refused, unreachable, timed out, resolver failure); InvalidArgument, a bad endpoint
// or a host name that does not exist; Timeout, Protocol or Unsupported (a DEBUG line:
// JRPC is not installed), connected but no usable banner; Cancelled, stopped.
class UPDCLIENT_API JrpcClient {
public:
  using Connector = std::function<Result<net::TransportPtr>()>;

  // jrpc://host[:port] connects over TCP directly, port 1409 by default. Any other
  // scheme, a bare host (tcp) included, goes through the TransportRegistry, so
  // registerBuiltins() must have registered tcp; a missing port becomes 1409 there too.
  // Endpoint::timeout bounds the TCP connect, name lookup included. Then the banner is
  // read.
  static Result<JrpcClient> connect(const net::Endpoint &endpoint, ClientOptions options = {});
  // As above; a stop request ends the connect (jrpc and bare hosts: the TCP connect and
  // its name lookup; other schemes: checked before and after the registry's connect)
  // and the banner at once with Cancelled. The token is used only during the call;
  // afterwards cancel() ends calls in progress, reconnect() included.
  static Result<JrpcClient> connect(const net::Endpoint &endpoint, ClientOptions options, std::stop_token stop);
  // Opens a connection through the connector and reads the banner. reconnect() uses
  // the same connector.
  static Result<JrpcClient> open(Connector connector, ClientOptions options = {});
  // As above; a stop request ends the banner at once with Cancelled. The connector
  // runs to its own end, and the token is checked once it returns.
  static Result<JrpcClient> open(Connector connector, ClientOptions options, std::stop_token stop);
  // Reads the banner from a connection that was just made. Without a connector,
  // reconnect() is Unsupported.
  static Result<JrpcClient> attach(net::TransportPtr transport, ClientOptions options = {},
                                   Connector connector = {});

  JrpcClient(JrpcClient &&) noexcept;
  JrpcClient &operator=(JrpcClient &&) noexcept;
  JrpcClient(const JrpcClient &) = delete;
  JrpcClient &operator=(const JrpcClient &) = delete;
  // As close().
  ~JrpcClient();

  // Thread-safe.
  bool isConnected() const noexcept;
  std::string describe() const;
  // The numeric address of the console while connected over TcpTransport (connect(), or
  // a connector that returns one); nullopt when not connected or over another
  // transport. See TcpTransport::peer().
  std::optional<net::Endpoint> peer() const;
  const ClientOptions &options() const noexcept;
  // Not while a call is in progress on another thread.
  void setOptions(const ClientOptions &options);

  // The last command this client tried; nullopt before the first. `delivery` describes
  // that command: NotSent means it was refused before anything went out (a bad
  // argument, a line over the limit) or the client was not connected, so repeating it
  // is safe; PartlySent and Sent mean the console may have run it, whether or not the
  // call then failed. D7: every call is treated as one that changes the console.
  // reconnect() does not change it, so it can be read after the reconnect to decide
  // whether to repeat the call.
  std::optional<CommandDelivery> lastDelivery() const;

  // Opens a new connection through the connector, after closing the current one as
  // close() does, and reads the banner. Refused while a call is in progress.
  Result<void> reconnect();
  // Writes "Bye" when the connection is idle and usable (it gets no answer), then
  // closes it. Calling it again, or on a connection that is already closed, sends
  // nothing.
  void close() noexcept;
  // Thread-safe. Closes the connection at once without "Bye"; the call in progress
  // fails with Cancelled, a reconnect() that is still connecting included. Does not
  // wait for it. A client made by connect() also ends a reconnect()'s TCP connect at
  // once; with a connector given to open() or attach(), cancel() takes effect when the
  // connector returns. Until reconnect(), every call fails with Cancelled.
  void cancel() noexcept;

  // One generic call (type 0 to 8, section 4.1). The spec is checked before anything
  // is sent (see buildCommand); the reply is parsed by the ReturnKind and must have the
  // shape of that kind, else the connection is closed (D3). A reply of `error=...`
  // fails with Io and keeps the connection (D4); a line containing DEBUG fails with
  // Unsupported ("JRPC is not installed") and closes it.
  Result<CallResult> call(const CallSpec &spec);

  // Typed calls: call() with the reply decoded. The helper's name picks the return
  // kind: spec.returns must be Void (the default, "unset") or that helper's own kind,
  // else InvalidArgument and nothing is sent; arraySize must suit the kind (0 for the
  // scalar helpers, 1 to maxArrayElements for callBytes, callInts and callFloats), as
  // buildCommand checks. Everything else is as for call(): the same errors, and the
  // connection closes on a reply of the wrong shape.
  //   callVoid      type 0: the reply is read and checked, its value dropped
  //   callInt32     type 1, the 32-bit register as a signed number
  //   callUInt32    type 1, the same bits as an unsigned number
  //   callByte      type 4, the low 8 bits
  //   callInt64     type 8, the 64-bit register as a signed number
  //   callUInt64    type 8, unsigned
  //   callFloat     type 3: the server prints `%f` with six decimals, so the value has
  //                 that precision and a double holds it; a float function included
  //   callString    type 2: the whole reply line
  //   callBytes     type 7, callInts type 5, callFloats type 6 (doubles, as callFloat)
  Result<void> callVoid(const CallSpec &spec);
  Result<int32_t> callInt32(const CallSpec &spec);
  Result<uint32_t> callUInt32(const CallSpec &spec);
  Result<uint8_t> callByte(const CallSpec &spec);
  Result<int64_t> callInt64(const CallSpec &spec);
  Result<uint64_t> callUInt64(const CallSpec &spec);
  Result<double> callFloat(const CallSpec &spec);
  Result<std::string> callString(const CallSpec &spec);
  Result<std::vector<uint8_t>> callBytes(const CallSpec &spec);
  Result<std::vector<int32_t>> callInts(const CallSpec &spec);
  Result<std::vector<double>> callFloats(const CallSpec &spec);

  // System opcodes (section 4.2). Each is one command, named for lastDelivery() by
  // opcodeName(). An `error=` answer is an Io error with remoteFault() set and keeps the
  // connection (D4); the other failures are as for call(). None of them takes a
  // by-ordinal target or a thread context.
  //
  // Opcode 9. The export's address as the console prints it (`0` is passed on as it is;
  // buildCommand refuses to call it). An empty module name is InvalidArgument.
  Result<uint32_t> resolveFunction(const std::string &module, uint32_t ordinal);
  // Opcode 10 (D8). The reply is two unpadded `%X` halves run together, so it can only
  // be split when it has all its digits: 16 digits give an 8-byte key, 32 a 16-byte one.
  // Any other length is Protocol with the raw text in the message, and when it is made
  // of hex digits alone (a half lost its leading zeros) the connection stays: that is
  // the ambiguity the document leaves, not a sign that the stream is out of step.
  Result<CpuKey> cpuKey();
  // Opcode 11. Sends the command and closes the connection without waiting for
  // anything and without "Bye", because the console goes down (D6): isConnected() is
  // false afterwards and reconnect() is the caller's decision once the console is back.
  // If the line could not be written, the failure is reported as for any call.
  Result<void> shutdown();
  // Opcode 12, with silentOpBarrier (D6). An empty text is InvalidArgument. `type` is
  // the icon the console shows; the document gives no list of values.
  Result<void> notify(const std::string &text, uint32_t type);
  // Opcode 13: `%d`; a negative number is Protocol.
  Result<uint32_t> kernelVersion();
  // Opcode 14, with silentOpBarrier (D6). The four LEDs, in that order.
  Result<void> setLeds(LedState topLeft, LedState topRight, LedState bottomLeft, LedState bottomRight);
  // Opcode 15: the raw `%X` value. A sensor that is not one of the four is
  // InvalidArgument.
  Result<uint32_t> temperature(TemperatureSensor sensor);
  // Opcode 16.
  Result<uint32_t> currentTitleId();
  // Opcode 17: one of the seven names; any other text is Protocol and closes the
  // connection. consoleTypeName() gives the text back.
  Result<ConsoleType> consoleType();
  // Opcode 18, with silentOpBarrier (D6): asks the console to keep writing `value` at
  // `address` (a task nothing here can cancel). With onlyIfValue the write happens only
  // while the word at `address` holds that value; with inTitle only while that title
  // is running. An address of 0 is InvalidArgument.
  Result<void> constantMemorySet(uint32_t address, uint32_t value, std::optional<uint32_t> onlyIfValue = std::nullopt,
                                 std::optional<uint32_t> inTitle = std::nullopt);

  // For diagnostics and hardware tests: sends one line as it is (printable ASCII, not
  // empty, CR LF added, at most maxCommandBytes) and returns the reply line. Exactly one
  // reply line is expected, so a command the server does not answer (opcodes 11, 12,
  // 14, 18, maybe) ends in a timeout and the closed connection. Here an `error=` line
  // is an answer, not an error. Counts as a command that changes the console.
  Result<RawAnswer> rawCommand(const std::string &line);

private:
  explicit JrpcClient(std::unique_ptr<detail::Session> session);
  // call() and the typed helpers: with `expected` set, the spec's return kind is
  // checked against it and replaced by it.
  Result<CallResult> callAs(const CallSpec &spec, std::optional<ReturnKind> expected, std::string_view helper);
  std::unique_ptr<detail::Session> session_;
};

// What identify() learned about a console.
struct IdentifyResult {
  // The endpoint that was probed, with the port filled in (1409 when the caller gave
  // none); the scheme is the caller's.
  net::Endpoint endpoint;
  // The greeting line, without its terminator. The client accepts only "JRPC2 connected",
  // so on success this is always kBanner; it is returned so that a caller can show what
  // the console said.
  std::string banner;
};

// Is JRPC installed on the console at this address? Connects, checks the banner, says
// "Bye" and closes; no command is sent, so nothing on the console changes. This is the
// way in by address alone, as JRPC does not announce itself and no discovery provider
// is registered by default. The errors are those of JrpcClient::connect: ConnectFailed
// (nothing listens), Timeout (no banner; a 9th connection waits for a slot without
// one), Protocol (another service greeted), Unsupported (a DEBUG line: JRPC is not
// installed), Cancelled. options.bannerTimeout bounds the banner.
UPDCLIENT_API Result<IdentifyResult> identify(const net::Endpoint &endpoint, ClientOptions options = {});
// As above; a stop request ends the connect or the banner at once with Cancelled.
UPDCLIENT_API Result<IdentifyResult> identify(const net::Endpoint &endpoint, ClientOptions options,
                                              std::stop_token stop);

// Registers the "jrpc" scheme: TCP with port 1409 when the endpoint has none. Not part of
// registerBuiltins(). JrpcClient::connect() handles jrpc:// itself and does not need it;
// it is for code that connects through the TransportRegistry, and it makes
// withDefaultPort() give 1409 for jrpc:// whatever protocol port the caller passes.
UPDCLIENT_API void registerJrpcScheme(net::TransportRegistry &registry = net::TransportRegistry::instance());

} // namespace updclient::jrpc
