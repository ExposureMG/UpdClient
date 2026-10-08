#pragma once

// A JRPC console for tests: the server side of docs/JRPC_PROTOCOL.md (sections 1 to 4
// and 6), written from that document alone, so that it checks the client rather than
// mirroring it. Nothing from include/protocols/jrpc is used here: the request parser
// is the server's (scan for LF in an 8500-byte buffer, find the fields of the
// `consolefeatures` line, key every argument on the first character of its tag,
// hex-decode strings and blobs, read numbers the way sscanf does), and the replies
// are formatted the way the server's format strings are (unpadded %X, %f with six
// decimals, `v,v;` lists of at most 8 elements).
//
// The console it models is small: functions the test registers at an address or as a
// module export, a module table for ResolveFunction, and the values the system
// opcodes report (CPU key, kernel version, console type, title id, temperatures). The
// effects of the opcodes that change the console (XNotify, SetLeds, constantMemorySet,
// ShutDownConsole) are recorded, not performed.
//
// Everything the document leaves open is a named option, so a test can run the client
// against each reading: do opcodes 11, 12, 14 and 18 answer (D6), does a void call
// answer `S_OK` or hex, is the CPU key padded, what happens past 8 array elements, how
// the CRT reads a number that does not fit. The defaults follow the document's wording
// and, where it is silent, the stricter reading (the one that exposes a client bug).
//
// Like the XBDM mock: every public member is thread-safe, each connection is served
// on its own thread, stop() (or the destructor) closes all of them and joins the
// threads, and every received line is recorded.

#include <core/error.hpp>
#include <net/transport.hpp>

#include "support/test_util.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ut {

// --- Calls as the server decoded them ---

// One argument of a `params` block. `tag` is the first character, the only thing the
// server looks at; which members are filled depends on it (section 3):
//   '1' and '4'  integer, read with sscanf %i into an int (sign-extended here)
//   '8'          integer, read with sscanf %lli
//   '3'          real, read with sscanf as a float (kept as a double here)
//   '2'          data, the decoded bytes of the string without a terminating NUL
//   '7'          data, the decoded bytes
struct JrpcArgument {
  char tag = '1';
  int64_t integer = 0;
  double real = 0.0;
  Bytes data;

  uint32_t u32() const { return static_cast<uint32_t>(integer); }
  std::string text() const { return std::string(data.begin(), data.end()); }
};

// A call of type 0 to 8, after parsing and resolving the entry point.
struct JrpcCall {
  int type = 0;
  bool system = false;
  // The effective address: the address field, or the export a module+ord pair resolved to.
  uint32_t address = 0;
  // Set for a call by module and ordinal.
  std::optional<std::string> module;
  uint32_t ordinal = 0;
  // The `as=` field: the number of array elements the caller expects back.
  int32_t arraySize = 0;
  std::vector<JrpcArgument> args;
  // The acceptance index of the connection it arrived on.
  size_t connection = 0;
};

// What a registered function returns. Like the real call, it hands back everything at
// once and the `type` of the request decides which part is read: r3 for types 0, 1, 4
// and 8, f1 for 3, the string for 2, the arrays for 5 to 7 (an array shorter than `as`
// reads as zeros past its end). A function can also take over the reply.
struct JrpcReturn {
  uint64_t r3 = 0;
  double f1 = 0.0;
  std::string text;
  std::vector<int32_t> ints;
  std::vector<double> floats;
  Bytes bytes;
  // "error=" followed by this text, instead of a value.
  std::optional<std::string> error;
  // These bytes plus CR LF, instead of a value: a reply of the wrong shape.
  std::optional<std::string> rawLine;
  // Send nothing: the function never returns a reply the client could read.
  bool noReply = false;

  static JrpcReturn integer(uint64_t value) {
    JrpcReturn r;
    r.r3 = value;
    return r;
  }
  static JrpcReturn real(double value) {
    JrpcReturn r;
    r.f1 = value;
    return r;
  }
  static JrpcReturn string(std::string value) {
    JrpcReturn r;
    r.text = std::move(value);
    return r;
  }
  static JrpcReturn intArray(std::vector<int32_t> values) {
    JrpcReturn r;
    r.ints = std::move(values);
    return r;
  }
  static JrpcReturn floatArray(std::vector<double> values) {
    JrpcReturn r;
    r.floats = std::move(values);
    return r;
  }
  static JrpcReturn byteArray(Bytes values) {
    JrpcReturn r;
    r.bytes = std::move(values);
    return r;
  }
  static JrpcReturn failure(std::string text) {
    JrpcReturn r;
    r.error = std::move(text);
    return r;
  }
  static JrpcReturn line(std::string text) {
    JrpcReturn r;
    r.rawLine = std::move(text);
    return r;
  }
  static JrpcReturn silent() {
    JrpcReturn r;
    r.noReply = true;
    return r;
  }
};

// Runs on the connection's thread without any mock lock held, so it may block (a call
// that takes long) or call back into the mock.
using JrpcFunction = std::function<JrpcReturn(const JrpcCall &)>;

// --- Options ---

// What the server does with a request it cannot use.
enum class JrpcOnFailure {
  // An `error=` line, the connection stays.
  ErrorLine,
  // Nothing; the connection stays and the client is left waiting.
  Silent,
  // The connection is closed.
  Close,
};

// What the opcodes that are documented without a reply (11, 12, 14, 18) do. Whether
// they answer is unknown (hardware checklist item 2, plan D6).
enum class JrpcSilentOpcodeAnswer {
  Silent,
  // `S_OK` and CR LF.
  SOk,
  // `0` and CR LF, the %X of a zero r3.
  Zero,
};

// What a type-0 call answers: the %X of r3, or `S_OK`.
enum class JrpcVoidAnswer { Hex, SOk };

// What the reply of an array call does when `as` is more than 8. The format has eight
// % slots (open question 1).
enum class JrpcArrayOverflow {
  // Eight elements, then `;`.
  Truncate,
  // The format loops: `as` elements, then `;`.
  Loop,
};

// How sscanf reads a number that does not fit its type (the console's CRT is unknown).
enum class JrpcIntOverflow {
  // Saturates at the limits of the type (what a strtol-based scan does).
  Clamp,
  // Keeps the low bits.
  Wrap,
};

struct JrpcMockOptions {
  // Simultaneous connections served; the next one is held, with no banner, until a
  // slot frees (section 1.1: a table of 8 sockets, a 9th waits).
  size_t connectionLimit = 8;
  // The per-connection receive buffer (section 1.3). A line of more bytes than this
  // including its LF is dropped.
  size_t bufferBytes = 8500;

  // Sent on accept, followed by CR LF. `sendBanner` false sends nothing at all.
  bool sendBanner = true;
  std::string banner = "JRPC2 connected";

  JrpcSilentOpcodeAnswer silentOpcodes = JrpcSilentOpcodeAnswer::Silent;
  JrpcVoidAnswer voidAnswer = JrpcVoidAnswer::Hex;
  JrpcArrayOverflow arrayOverflow = JrpcArrayOverflow::Truncate;
  JrpcIntOverflow intOverflow = JrpcIntOverflow::Clamp;

  // GetCPUKey prints two values with %X, high half then low half. Padded, each half is
  // zero-filled to `cpuKeyDigits` digits (8 gives the document's 16-digit example, 16
  // gives 32 digits in all); unpadded it is the bare %X, shorter whenever the half has
  // leading zeros.
  bool cpuKeyPadded = true;
  size_t cpuKeyDigits = 8;

  // A byte return (type 4) as %02X instead of %X (section 4.1 shows both).
  bool byteReplyPadded = false;

  // A line that does not parse, a call whose entry point is not registered or cannot
  // be resolved, and a line longer than the receive buffer.
  JrpcOnFailure onMalformed = JrpcOnFailure::ErrorLine;
  JrpcOnFailure onUnknownTarget = JrpcOnFailure::ErrorLine;
  JrpcOnFailure onOverLong = JrpcOnFailure::Silent;

  // ShutDownConsole takes the whole console down: every connection closes, not only
  // the one that sent it.
  bool shutdownDropsAllConnections = true;

  // Capacity of each direction of an in-memory connection.
  size_t pipeCapacity = 64 * 1024;
};

// What the system opcodes report.
struct JrpcConsoleInfo {
  // GetCPUKey: the two halves, high first (section 4.2: `%X%X`).
  uint64_t cpuKeyHigh = 0xA1B2C3D4;
  uint64_t cpuKeyLow = 0xE5F60718;
  // GetKernelVersion, printed with %d.
  int32_t kernelVersion = 17559;
  // ConsoleType: sent as is. The server sends one of Xenon, Zephyr, Falcon, Jasper,
  // Trinity, Corona or Unknown.
  std::string consoleType = "Jasper";
  // GetCurrentTitleId.
  uint32_t titleId = 0xFFFE07D1;
  // GetTemperature indexes this: 0 CPU, 1 GPU, 2 EDRAM, 3 mainboard. Printed with %X.
  std::array<uint32_t, 4> temperatures = {0x32, 0x2D, 0x2A, 0x28};
};

// --- Records ---

// One received line, in arrival order.
struct JrpcCommandRecord {
  // Acceptance index of the connection (order of arrival, from 0).
  size_t connection = 0;
  // Without the line terminator; the first 256 bytes only when overLong.
  std::string line;
  // call (type 0 to 8), resolve, cpukey, shutdown, notify, kernel, leds, temperature,
  // titleid, consoletype, constmem, opcode19, bye, empty, invalid, overlong.
  std::string name;
  // The `type=` value; empty for a line that did not parse that far.
  std::optional<int> type;
  // More bytes were already waiting on the connection when this line was taken.
  bool pipelined = false;
  bool overLong = false;
  // Bytes of the line including the LF (for an over-long line, all that were dropped).
  size_t length = 0;
};

struct JrpcNotification {
  std::string text;
  uint32_t type = 0;
};

struct JrpcLedWrite {
  int32_t topLeft = 0;
  int32_t topRight = 0;
  int32_t bottomLeft = 0;
  int32_t bottomRight = 0;
};

// Opcode 18: the five ints after the address.
struct JrpcMemoryTask {
  uint32_t address = 0;
  uint32_t value = 0;
  uint32_t useIf = 0;
  uint32_t ifValue = 0;
  uint32_t useTitle = 0;
  uint32_t titleId = 0;
};

// --- Faults ---

// A misbehaviour, armed with JrpcMockServer::inject(). The target fields choose which
// replies it applies to; the effect fields can be combined. The command still takes
// effect on the console unless skipCommand is set.
struct JrpcFault {
  // Only requests with this `type=`; empty matches every request.
  std::optional<int> type;
  // Only calls to this effective address (by address, or a module+ord that resolves to it).
  std::optional<uint32_t> address;
  // Only requests whose line contains this text; empty matches all.
  std::string contains;
  // Applies to the banner instead of a command reply.
  bool greeting = false;
  // Only on the connection with this acceptance index.
  std::optional<size_t> connection;
  // Matching replies to let through before the first firing.
  size_t skip = 0;
  // Firings before the fault disarms itself; negative fires forever.
  int times = 1;

  // Close the connection after this many bytes of the reply (0 closes at once).
  std::optional<size_t> dropAfter;
  // After this many bytes of the reply send nothing for stallFor (zero: until the
  // client closes or the server stops), then carry on. The connection keeps being
  // read meanwhile, so a close is noticed at once.
  std::optional<size_t> stallAfter;
  std::chrono::milliseconds stallFor{0};
  // Send these bytes instead of the reply.
  std::optional<Bytes> replaceWith;
  // Do not carry the command out (the function is not called, the opcode has no
  // effect), so a replaced reply that refuses it is the truth.
  bool skipCommand = false;
  // Bytes sent after the reply.
  Bytes trailer;
  // After the reply, send bytes that never form a line until the connection closes.
  bool endless = false;
  // Write the reply one byte per write.
  bool trickle = false;
  // Deterministically mangle the reply: flipped, inserted and dropped bytes, cut
  // lines, early closes. The same seed gives the same damage.
  std::optional<uint64_t> hostileSeed;
  // An array reply prints this many elements, whatever `as` and the 8-element limit say.
  std::optional<size_t> arrayCount;

  static JrpcFault dropAfterBytes(size_t bytes);
  // Close at once, without any reply.
  static JrpcFault dropConnection();
  static JrpcFault stall(size_t afterBytes, std::chrono::milliseconds duration = std::chrono::milliseconds(0));
  // The reply comes `duration` late.
  static JrpcFault delay(std::chrono::milliseconds duration);
  // Never answer, on every matching request: the call hangs.
  static JrpcFault silence();
  static JrpcFault reply(std::string_view raw);
  static JrpcFault reply(Bytes raw);
  // A reply line: replyLine("12") sends "12\r\n".
  static JrpcFault replyLine(std::string_view line);
  // "error=<text>" and CR LF.
  static JrpcFault errorLine(std::string_view text);
  // A line containing DEBUG, which the client reads as "JRPC is not installed".
  static JrpcFault debugLine();
  // A line of `length` characters (default: one past the client's 64 KiB cap).
  static JrpcFault oversizedLine(size_t length = 64 * 1024 + 1);
  static JrpcFault endlessLine();
  static JrpcFault trickleBytes();
  static JrpcFault hostile(uint64_t seed);
  static JrpcFault extraBytes(Bytes bytes);
  static JrpcFault arrayElements(size_t count);

  JrpcFault &onType(int value);
  JrpcFault &onAddress(uint32_t value);
  JrpcFault &whenContains(std::string_view text);
  JrpcFault &onGreeting();
  JrpcFault &onConnection(size_t index);
  JrpcFault &after(size_t matches);
  JrpcFault &repeat(int count);
  JrpcFault &always();
  JrpcFault &withoutCommand();
};

class JrpcMockServer {
public:
  struct State;

  // Starts with an empty function table and module table, and the default console of
  // JrpcConsoleInfo.
  explicit JrpcMockServer(JrpcMockOptions options = {});
  ~JrpcMockServer();
  JrpcMockServer(const JrpcMockServer &) = delete;
  JrpcMockServer &operator=(const JrpcMockServer &) = delete;

  JrpcMockOptions options() const;
  void setOptions(const JrpcMockOptions &options);

  // --- Connections ---

  // A new in-memory connection: the client end of a MemoryPipe whose server end is
  // served on its own thread. The client end has no timeout until the client sets one.
  updclient::Result<updclient::net::TransportPtr> connect();
  // connect() as a factory, for clients that take a connector.
  std::function<updclient::Result<updclient::net::TransportPtr>()> connector();
  // Serves a server-side transport the caller made.
  void serve(updclient::net::TransportPtr transport);
  // Starts a TCP listener on 127.0.0.1 and an ephemeral port, and returns the port.
  // Fails with ConnectFailed (and a reason) when the environment refuses sockets.
  updclient::Result<uint16_t> listenTcp();
  uint16_t tcpPort() const;
  // Closes every open connection, as a network drop would; the listeners stay.
  void dropAllConnections();
  // Closes everything and joins every thread. Idempotent.
  void stop();

  // Connections that got a slot, are being served now, and are held for lack of a slot.
  size_t connectionsAccepted() const;
  size_t activeConnections() const;
  size_t waitingConnections() const;
  // Poll until the condition holds or limit passes. For tests only, never used by the
  // protocol path.
  bool waitForActiveConnections(size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) const;
  bool waitForWaitingConnections(size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) const;
  bool waitForCommands(size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) const;
  bool waitUntilIdle(std::chrono::milliseconds limit = std::chrono::seconds(5)) const;

  // --- Fault injection ---

  void inject(JrpcFault fault);
  void clearFaults();
  // Armed faults that have not fired their last time yet.
  size_t pendingFaults() const;

  // --- Records ---

  std::vector<JrpcCommandRecord> commands() const;
  std::vector<std::string> commandLines() const;
  void clearCommands();
  // Every generic call (type 0 to 8) that parsed, whether or not it found its function.
  std::vector<JrpcCall> calls() const;
  std::vector<JrpcNotification> notifications() const;
  std::vector<JrpcLedWrite> ledWrites() const;
  std::vector<JrpcMemoryTask> memoryTasks() const;
  // Effects in order: "shutdown", "notify <type> <text>", "leds <a> <b> <c> <d>",
  // "constmem <address> <value> <useIf> <ifValue> <useTitle> <titleId>" (hex, 8 digits).
  std::vector<std::string> events() const;
  void clearEvents();

  // --- Console ---

  JrpcConsoleInfo info() const;
  void setInfo(const JrpcConsoleInfo &info);

  // Registers the function at an address; replaces what was there.
  void registerFunction(uint32_t address, JrpcFunction function);
  // Registers it as export `ordinal` of `module` (names compare without case) at a
  // fresh address, which is returned; ResolveFunction answers that address and a call
  // by address reaches the function as well. Registering the same export again
  // replaces the function and keeps its address.
  uint32_t registerFunction(std::string_view module, uint32_t ordinal, JrpcFunction function);
  // Makes export `ordinal` of `module` resolve to `address` (a function registered
  // there, or none: a call then reaches an unknown target).
  void registerExport(std::string_view module, uint32_t ordinal, uint32_t address);
  void unregisterFunction(uint32_t address);
  void clearFunctions();
  // How often the function at the address ran.
  size_t callCount(uint32_t address) const;

private:
  std::shared_ptr<State> state_;
};

} // namespace ut
