#include "support/jrpc_mock_server.hpp"

#include "support/loopback_server.hpp"
#include "support/memory_transport.hpp"
#include "support/tcp_server_transport.hpp"

#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace ut {

namespace {

using updclient::ErrorCode;
using updclient::Result;
using updclient::fail;
namespace net = updclient::net;

constexpr size_t kChunk = 64 * 1024;
// The server frame holds 37 arguments (section 2).
constexpr int kMaxArgs = 37;
// The reply formats have eight % slots (section 4.1).
constexpr size_t kFormatSlots = 8;
// A mock guard, not a console fact: the largest array a reply is built for.
constexpr size_t kMaxPrinted = 1 << 16;
// How much of a dropped line is kept in the record.
constexpr size_t kKeptOfOverLong = 256;

constexpr std::string_view kBadParams = "The paramaters were not found";
constexpr std::string_view kBadVersion = "Version mismatch";
constexpr std::string_view kBadCommand = "Unknown command";

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string format(const char *pattern, auto... values) {
  char buffer[1024];
  const int n = std::snprintf(buffer, sizeof(buffer), pattern, values...);
  return std::string(buffer, n > 0 ? std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1) : 0);
}

std::string hex8(uint32_t value) { return format("%08X", static_cast<unsigned>(value)); }

// --- Numbers, read the way sscanf reads them ---

struct Scan {
  bool ok = false;
  bool negative = false;
  uint64_t magnitude = 0;
  // More digits than 64 bits hold.
  bool overflow = false;
};

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// base 0 is %i (0x prefix for hex, a leading 0 for octal), 10 is %d, 16 is %X. Leading
// white space and a sign are skipped, and whatever follows the digits is ignored.
Scan scanInteger(std::string_view text, int base) {
  Scan scan;
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    scan.negative = text[i] == '-';
    ++i;
  }
  int b = base;
  if ((b == 0 || b == 16) && i + 2 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') &&
      digitValue(text[i + 2]) >= 0) {
    i += 2;
    b = 16;
  } else if (b == 0) {
    b = (i < text.size() && text[i] == '0') ? 8 : 10;
  }
  const size_t start = i;
  while (i < text.size()) {
    const int d = digitValue(text[i]);
    if (d < 0 || d >= b) break;
    const auto ub = static_cast<uint64_t>(b);
    const auto ud = static_cast<uint64_t>(d);
    if (scan.magnitude > (std::numeric_limits<uint64_t>::max() - ud) / ub) scan.overflow = true;
    scan.magnitude = scan.magnitude * ub + ud;
    ++i;
  }
  scan.ok = i > start;
  return scan;
}

int32_t toInt32(const Scan &scan, JrpcIntOverflow mode) {
  if (mode == JrpcIntOverflow::Clamp) {
    if (scan.negative) {
      if (scan.overflow || scan.magnitude > (uint64_t{1} << 31)) return std::numeric_limits<int32_t>::min();
      return static_cast<int32_t>(-static_cast<int64_t>(scan.magnitude));
    }
    if (scan.overflow || scan.magnitude > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(scan.magnitude);
  }
  const uint64_t signedValue = scan.negative ? (uint64_t{0} - scan.magnitude) : scan.magnitude;
  return static_cast<int32_t>(static_cast<uint32_t>(signedValue));
}

int64_t toInt64(const Scan &scan, JrpcIntOverflow mode) {
  if (mode == JrpcIntOverflow::Clamp) {
    if (scan.negative) {
      if (scan.overflow || scan.magnitude > (uint64_t{1} << 63)) return std::numeric_limits<int64_t>::min();
      return static_cast<int64_t>(uint64_t{0} - scan.magnitude);
    }
    if (scan.overflow || scan.magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>(scan.magnitude);
  }
  return static_cast<int64_t>(scan.negative ? (uint64_t{0} - scan.magnitude) : scan.magnitude);
}

// --- Command lines, parsed from sections 1.3, 2 and 3 only ---

struct Parsed {
  enum class Kind { Empty, Bye, Request, Bad };

  Kind kind = Kind::Bad;
  // For Bad: the text after "error=".
  std::string error;
  // Known once the header parsed that far, even when the params then failed.
  std::optional<int> type;
  bool system = false;
  std::optional<std::string> module;
  uint32_t ordinal = 0;
  int32_t arraySize = 0;
  uint32_t address = 0;
  std::string params;
  std::vector<JrpcArgument> args;
};

Parsed bad(std::string_view text, std::optional<int> type = std::nullopt) {
  Parsed p;
  p.kind = Parsed::Kind::Bad;
  p.error = std::string(text);
  p.type = type;
  return p;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

// The argument block of section 2: A\<addr>\A\<argc>\ then argc arguments, each a tag
// character, a separator character, then either a value or a count and a payload,
// every part ended by a backslash. Only the tag decides how an argument is read; the
// separator ("\" or "/") is skipped.
bool parseParams(std::string_view params, const JrpcMockOptions &options, Parsed &out) {
  size_t i = 0;
  auto expect = [&](char c) {
    if (i >= params.size() || params[i] != c) return false;
    ++i;
    return true;
  };
  auto field = [&](std::string_view &value) {
    const size_t end = params.find('\\', i);
    if (end == std::string_view::npos) return false;
    value = params.substr(i, end - i);
    i = end + 1;
    return true;
  };

  std::string_view part;
  if (!expect('A') || !expect('\\') || !field(part)) return false;
  const Scan address = scanInteger(part, 16);
  if (!address.ok) return false;
  out.address = static_cast<uint32_t>(address.negative ? (uint64_t{0} - address.magnitude) : address.magnitude);
  if (!expect('A') || !expect('\\') || !field(part)) return false;
  const Scan argc = scanInteger(part, 10);
  if (!argc.ok || argc.negative || argc.overflow || argc.magnitude > static_cast<uint64_t>(kMaxArgs)) return false;

  for (uint64_t n = 0; n < argc.magnitude; ++n) {
    if (i + 1 >= params.size()) return false;
    JrpcArgument arg;
    arg.tag = params[i];
    const char separator = params[i + 1];
    if (separator != '\\' && separator != '/') return false;
    i += 2;
    switch (arg.tag) {
    case '1':
    case '4': {
      if (!field(part)) return false;
      const Scan scan = scanInteger(part, 0);
      if (!scan.ok) return false;
      arg.integer = toInt32(scan, options.intOverflow);
      break;
    }
    case '8': {
      if (!field(part)) return false;
      const Scan scan = scanInteger(part, 0);
      if (!scan.ok) return false;
      arg.integer = toInt64(scan, options.intOverflow);
      break;
    }
    case '3': {
      if (!field(part)) return false;
      const std::string text(part);
      char *end = nullptr;
      arg.real = std::strtod(text.c_str(), &end);
      if (end == text.c_str()) return false;
      break;
    }
    case '2':
    case '7': {
      std::string_view payload;
      if (!field(part) || !field(payload)) return false;
      const Scan count = scanInteger(part, 10);
      if (!count.ok || count.negative || count.overflow || payload.size() != count.magnitude * 2) return false;
      arg.data.reserve(payload.size() / 2);
      for (size_t k = 0; k < payload.size(); k += 2) {
        const int hi = digitValue(payload[k]);
        const int lo = digitValue(payload[k + 1]);
        if (hi < 0 || lo < 0) return false;
        arg.data.push_back(static_cast<uint8_t>(hi * 16 + lo));
      }
      break;
    }
    default: return false;
    }
    out.args.push_back(std::move(arg));
  }
  return true;
}

Parsed parseLine(std::string_view raw, const JrpcMockOptions &options) {
  const std::string_view line = trim(raw);
  Parsed p;
  if (line.empty()) {
    p.kind = Parsed::Kind::Empty;
    return p;
  }
  if (line == "Bye") {
    p.kind = Parsed::Kind::Bye;
    return p;
  }
  constexpr std::string_view verb = "consolefeatures";
  if (line.substr(0, verb.size()) != verb || (line.size() > verb.size() && line[verb.size()] != ' ')) {
    return bad(kBadCommand);
  }
  const size_t paramsAt = line.find("params=\"");
  if (paramsAt == std::string_view::npos) return bad(kBadParams);

  std::string header(line.substr(verb.size(), paramsAt - verb.size()));
  if (const size_t m = header.find("module=\""); m != std::string::npos) {
    const size_t close = header.find('"', m + 8);
    if (close == std::string::npos) return bad(kBadParams);
    p.module = header.substr(m + 8, close - (m + 8));
    header.erase(m, close + 1 - m);
  }
  int version = 0;
  bool haveType = false;
  size_t at = 0;
  while (at < header.size()) {
    while (at < header.size() && std::isspace(static_cast<unsigned char>(header[at]))) ++at;
    const size_t end = std::min(header.find_first_of(" \t\r", at), header.size());
    const std::string_view token = std::string_view(header).substr(at, end - at);
    at = end;
    if (token.empty()) continue;
    auto number = [&](size_t prefix) { return toInt32(scanInteger(token.substr(prefix), 10), options.intOverflow); };
    if (token.substr(0, 4) == "ver=") version = number(4);
    else if (token.substr(0, 5) == "type=") {
      p.type = number(5);
      haveType = true;
    } else if (token.substr(0, 3) == "as=") p.arraySize = number(3);
    else if (token.substr(0, 4) == "ord=") p.ordinal = static_cast<uint32_t>(number(4));
    else if (token == "system") p.system = true;
  }
  if (!haveType) return bad(kBadParams);
  if (version != 2) return bad(kBadVersion, p.type);
  if (*p.type < 0 || *p.type > 19) return bad(kBadParams, p.type);

  const size_t bodyAt = paramsAt + 8;
  const size_t close = line.rfind('"');
  if (close == std::string_view::npos || close < bodyAt) return bad(kBadParams, p.type);
  p.params = std::string(line.substr(bodyAt, close - bodyAt));
  if (!parseParams(p.params, options, p)) return bad(kBadParams, p.type);
  p.kind = Parsed::Kind::Request;
  return p;
}

// --- Replies, formatted as section 6 says ---

std::string floatText(double value) { return format("%f", value); }

std::string listText(size_t count, const std::function<std::string(size_t)> &element) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) out += ',';
    out += element(i);
  }
  return out + ";";
}

// The reply line (no terminator) to a call, or nullopt for none.
std::optional<std::string> callReply(const JrpcCall &call, const JrpcReturn &r, const JrpcMockOptions &options,
                                     std::optional<size_t> arrayCount) {
  if (r.noReply) return std::nullopt;
  if (r.error) return "error=" + *r.error;
  if (r.rawLine) return *r.rawLine;
  const auto low32 = static_cast<unsigned>(r.r3 & 0xFFFFFFFFu);
  size_t printed = static_cast<size_t>(std::max<int32_t>(call.arraySize, 0));
  if (arrayCount) {
    printed = *arrayCount;
  } else if (options.arrayOverflow == JrpcArrayOverflow::Truncate) {
    printed = std::min(printed, kFormatSlots);
  }
  printed = std::min(printed, kMaxPrinted);
  switch (call.type) {
  case 0:
    if (options.voidAnswer == JrpcVoidAnswer::SOk) return std::string("S_OK");
    return format("%X", low32);
  case 1: return format("%X", low32);
  case 2: return r.text;
  case 3: return floatText(r.f1);
  case 4: return options.byteReplyPadded ? format("%02X", low32 & 0xFFu) : format("%X", low32 & 0xFFu);
  case 5:
    return listText(printed, [&](size_t i) { return format("%d", static_cast<int>(i < r.ints.size() ? r.ints[i] : 0)); });
  case 6: return listText(printed, [&](size_t i) { return floatText(i < r.floats.size() ? r.floats[i] : 0.0); });
  case 7:
    return listText(printed, [&](size_t i) { return format("%X", static_cast<unsigned>(i < r.bytes.size() ? r.bytes[i] : 0)); });
  default: return format("%llX", static_cast<unsigned long long>(r.r3));
  }
}

std::string cpuKeyText(const JrpcConsoleInfo &info, const JrpcMockOptions &options) {
  if (!options.cpuKeyPadded) {
    return format("%llX", static_cast<unsigned long long>(info.cpuKeyHigh)) +
           format("%llX", static_cast<unsigned long long>(info.cpuKeyLow));
  }
  const int width = static_cast<int>(std::min<size_t>(options.cpuKeyDigits, 16));
  return format("%0*llX", width, static_cast<unsigned long long>(info.cpuKeyHigh)) +
         format("%0*llX", width, static_cast<unsigned long long>(info.cpuKeyLow));
}

// --- The virtual console ---

struct ArmedFault {
  JrpcFault fault;
  size_t seen = 0;
  int fired = 0;
};

struct Connection {
  size_t index = 0;
  net::TransportPtr transport;
  std::thread thread;
  std::atomic<bool> finished{false};
  std::string input;
  // Inside the tail of a line that overflowed the receive buffer.
  bool discarding = false;
  size_t discarded = 0;
  std::string discardedPrefix;
  // Guarded by the state mutex.
  bool busy = false;
  bool counted = false;
};

// What a request answers: bytes (maybe none), then what happens to the connection.
struct Reply {
  enum class After { Continue, Close, Shutdown };

  Bytes bytes;
  After after = After::Continue;
};

Reply lineReply(std::string_view text) {
  Reply r;
  append(r.bytes, text);
  append(r.bytes, "\r\n");
  return r;
}

// Deterministic damage for the hostile fault.
void mangle(Bytes &bytes, uint64_t seed, bool &closeEarly) {
  std::mt19937_64 rng(seed);
  const int edits = 1 + static_cast<int>(rng() % 4);
  for (int e = 0; e < edits; ++e) {
    const auto op = rng() % 5;
    const size_t at = bytes.empty() ? 0 : static_cast<size_t>(rng() % bytes.size());
    switch (op) {
    case 0:
      if (!bytes.empty()) bytes[at] = static_cast<uint8_t>(bytes[at] ^ (1u << (rng() % 8)));
      break;
    case 1: bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(rng())); break;
    case 2:
      if (!bytes.empty()) bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at));
      break;
    case 3:
      bytes.resize(at);
      closeEarly = true;
      break;
    default: {
      const Bytes crlf = bytesOf("\r\n");
      bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), crlf.begin(), crlf.end());
      break;
    }
    }
  }
}

} // namespace

// --- Fault factories ---

JrpcFault JrpcFault::dropAfterBytes(size_t bytes) {
  JrpcFault f;
  f.dropAfter = bytes;
  return f;
}
JrpcFault JrpcFault::dropConnection() { return dropAfterBytes(0); }
JrpcFault JrpcFault::stall(size_t afterBytes, std::chrono::milliseconds duration) {
  JrpcFault f;
  f.stallAfter = afterBytes;
  f.stallFor = duration;
  return f;
}
JrpcFault JrpcFault::delay(std::chrono::milliseconds duration) { return stall(0, duration); }
JrpcFault JrpcFault::silence() {
  JrpcFault f = stall(0);
  f.times = -1;
  return f;
}
JrpcFault JrpcFault::reply(std::string_view raw) { return reply(bytesOf(raw)); }
JrpcFault JrpcFault::reply(Bytes raw) {
  JrpcFault f;
  f.replaceWith = std::move(raw);
  return f;
}
JrpcFault JrpcFault::replyLine(std::string_view line) { return reply(std::string(line) + "\r\n"); }
JrpcFault JrpcFault::errorLine(std::string_view text) { return replyLine("error=" + std::string(text)); }
JrpcFault JrpcFault::debugLine() { return replyLine("DEBUG"); }
JrpcFault JrpcFault::oversizedLine(size_t length) { return reply(std::string(length, 'x') + "\r\n"); }
JrpcFault JrpcFault::endlessLine() {
  JrpcFault f = reply(std::string_view("12"));
  f.endless = true;
  return f;
}
JrpcFault JrpcFault::trickleBytes() {
  JrpcFault f;
  f.trickle = true;
  return f;
}
JrpcFault JrpcFault::hostile(uint64_t seed) {
  JrpcFault f;
  f.hostileSeed = seed;
  return f;
}
JrpcFault JrpcFault::extraBytes(Bytes bytes) {
  JrpcFault f;
  f.trailer = std::move(bytes);
  return f;
}
JrpcFault JrpcFault::arrayElements(size_t count) {
  JrpcFault f;
  f.arrayCount = count;
  return f;
}

JrpcFault &JrpcFault::onType(int value) {
  type = value;
  greeting = false;
  return *this;
}
JrpcFault &JrpcFault::onAddress(uint32_t value) {
  address = value;
  greeting = false;
  return *this;
}
JrpcFault &JrpcFault::whenContains(std::string_view text) {
  contains = std::string(text);
  greeting = false;
  return *this;
}
JrpcFault &JrpcFault::onGreeting() {
  greeting = true;
  type.reset();
  address.reset();
  contains.clear();
  return *this;
}
JrpcFault &JrpcFault::onConnection(size_t index) {
  connection = index;
  return *this;
}
JrpcFault &JrpcFault::after(size_t matches) {
  skip = matches;
  return *this;
}
JrpcFault &JrpcFault::repeat(int count) {
  times = count;
  return *this;
}
JrpcFault &JrpcFault::always() {
  times = -1;
  return *this;
}
JrpcFault &JrpcFault::withoutCommand() {
  skipCommand = true;
  return *this;
}

// --- Server state ---

struct JrpcMockServer::State {
  mutable std::mutex mutex;
  std::condition_variable slotFree;
  JrpcMockOptions options;
  JrpcConsoleInfo info;
  std::map<uint32_t, std::shared_ptr<JrpcFunction>> functions;
  std::map<uint32_t, size_t> callCounts;
  // (module in lower case, ordinal) to address.
  std::map<std::pair<std::string, uint32_t>, uint32_t> exports;
  uint32_t nextAddress = 0x90000000u;
  std::vector<ArmedFault> faults;
  std::vector<JrpcCommandRecord> commands;
  std::vector<JrpcCall> calls;
  std::vector<JrpcNotification> notifications;
  std::vector<JrpcLedWrite> leds;
  std::vector<JrpcMemoryTask> tasks;
  std::vector<std::string> events;
  std::vector<std::shared_ptr<Connection>> connections;
  // Arrival order of the connections waiting for a slot.
  std::deque<size_t> queue;
  size_t arrived = 0;
  size_t accepted = 0;
  size_t active = 0;
  bool stopping = false;

  std::atomic<bool> stopFlag{false};
  SocketHandle listener;
  std::thread acceptThread;
  uint16_t tcpPort = 0;

  std::optional<JrpcFault> takeFault(std::optional<int> type, std::optional<uint32_t> address, std::string_view line,
                                     size_t connection, bool greeting) {
    for (size_t i = 0; i < faults.size(); ++i) {
      auto &armed = faults[i];
      const auto &f = armed.fault;
      if (f.greeting != greeting) continue;
      if (!greeting) {
        if (f.type && f.type != type) continue;
        if (f.address && f.address != address) continue;
        if (!f.contains.empty() && line.find(f.contains) == std::string_view::npos) continue;
      }
      if (f.connection && *f.connection != connection) continue;
      if (armed.seen < f.skip) {
        ++armed.seen;
        continue;
      }
      JrpcFault copy = f;
      ++armed.fired;
      if (f.times >= 0 && armed.fired >= f.times) faults.erase(faults.begin() + static_cast<std::ptrdiff_t>(i));
      return copy;
    }
    return std::nullopt;
  }

  void closeAllConnections() {
    for (auto &c : connections) {
      if (!c->finished) c->transport->close();
    }
  }
};

namespace {

using State = JrpcMockServer::State;

// Sends one reply and applies the fault armed for it. Every byte of the reply counts
// toward the fault's offsets.
class Sender {
public:
  Sender(State &state, Connection &connection, std::optional<JrpcFault> fault)
      : state_(state), c_(connection), fault_(std::move(fault)) {}

  const std::optional<JrpcFault> &fault() const { return fault_; }

  bool put(std::span<const uint8_t> data) {
    while (true) {
      if (fault_ && fault_->dropAfter && sent_ >= *fault_->dropAfter) return drop();
      if (fault_ && fault_->stallAfter && !stalled_ && sent_ >= *fault_->stallAfter) {
        stalled_ = true;
        if (!stall(fault_->stallFor)) return false;
      }
      if (data.empty()) return true;
      uint64_t n = data.size();
      if (fault_ && fault_->dropAfter) n = std::min<uint64_t>(n, *fault_->dropAfter - sent_);
      if (fault_ && fault_->stallAfter && !stalled_) n = std::min<uint64_t>(n, *fault_->stallAfter - sent_);
      if (!raw(data.first(static_cast<size_t>(n)))) return false;
      sent_ += n;
      data = data.subspan(static_cast<size_t>(n));
    }
  }

  // Sends a whole reply; false when the connection is over.
  bool send(const Reply &reply) {
    bool closeEarly = false;
    if (fault_ && fault_->replaceWith) {
      if (!put(*fault_->replaceWith)) return false;
      return finish();
    }
    if (fault_ && fault_->hostileSeed) {
      Bytes all = reply.bytes;
      mangle(all, *fault_->hostileSeed, closeEarly);
      if (!put(all)) return false;
      if (closeEarly) return drop();
      return finish();
    }
    if (!put(reply.bytes)) return false;
    return finish();
  }

  bool finish() {
    if (fault_ && !fault_->trailer.empty() && !put(fault_->trailer)) return false;
    if (fault_ && fault_->stallAfter && !stalled_) {
      stalled_ = true;
      if (!stall(fault_->stallFor)) return false;
    }
    if (fault_ && fault_->dropAfter) return drop();
    if (fault_ && fault_->endless) {
      const Bytes junk(4096, 'x');
      while (!state_.stopFlag) {
        if (!raw(junk)) return false;
      }
      return false;
    }
    return true;
  }

  bool drop() {
    c_.transport->close();
    return false;
  }

  // Sends nothing for `duration` (zero: until the client closes), still reading so
  // that a close is noticed at once. Bytes the client sends meanwhile are kept.
  bool stall(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    std::array<uint8_t, 4096> scratch{};
    while (true) {
      std::chrono::milliseconds wait{0};
      if (duration.count() > 0) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        wait = left;
      }
      (void)c_.transport->setTimeout(wait);
      auto n = c_.transport->readSome(scratch);
      if (!n) {
        if (n.error().code == ErrorCode::Timeout) break;
        return false;
      }
      if (*n == 0) return false;
      c_.input.append(reinterpret_cast<const char *>(scratch.data()), *n);
    }
    (void)c_.transport->setTimeout(std::chrono::milliseconds(0));
    return true;
  }

private:
  bool raw(std::span<const uint8_t> data) {
    if (fault_ && fault_->trickle) {
      for (size_t i = 0; i < data.size(); ++i) {
        if (!c_.transport->writeAll(data.subspan(i, 1))) return false;
      }
      return true;
    }
    return c_.transport->writeAll(data).has_value();
  }

  State &state_;
  Connection &c_;
  std::optional<JrpcFault> fault_;
  uint64_t sent_ = 0;
  bool stalled_ = false;
};

// --- The connection loop ---

struct LineRead {
  // Without the terminator.
  std::string text;
  bool pipelined = false;
  bool overLong = false;
  size_t length = 0;
};

// Section 1.3: the server appends each recv to a buffer of `bufferBytes` and scans it
// for LF. A line that does not fit before its LF is dropped, up to and including that
// LF, and the next line starts clean (the document does not say where the server
// resumes; resynchronising at the LF is the reading that keeps the connection usable).
std::optional<LineRead> readLine(Connection &c, size_t bufferBytes) {
  std::array<uint8_t, kChunk> buffer{};
  while (true) {
    const size_t lf = c.input.find('\n');
    if (lf != std::string::npos) {
      LineRead out;
      std::string piece = c.input.substr(0, lf);
      c.input.erase(0, lf + 1);
      out.pipelined = !c.input.empty();
      if (c.discarding) {
        out.overLong = true;
        out.length = c.discarded + lf + 1;
        out.text = std::move(c.discardedPrefix);
        c.discarding = false;
        c.discarded = 0;
        c.discardedPrefix.clear();
        return out;
      }
      out.length = lf + 1;
      if (out.length > bufferBytes) {
        out.overLong = true;
        out.text = piece.substr(0, kKeptOfOverLong);
        return out;
      }
      if (!piece.empty() && piece.back() == '\r') piece.pop_back();
      out.text = std::move(piece);
      return out;
    }
    if (c.discarding || c.input.size() >= bufferBytes) {
      if (!c.discarding) {
        c.discarding = true;
        c.discardedPrefix = c.input.substr(0, kKeptOfOverLong);
      }
      c.discarded += c.input.size();
      c.input.clear();
    }
    auto n = c.transport->readSome(buffer);
    if (!n || *n == 0) return std::nullopt;
    c.input.append(reinterpret_cast<const char *>(buffer.data()), *n);
  }
}

// One request, after parsing: what the connection loop works out under the mock lock
// and execute() then answers without it.
struct Request {
  Parsed parsed;
  JrpcCall call;
  std::shared_ptr<JrpcFunction> function;
  JrpcMockOptions options;
  JrpcConsoleInfo info;
  // False when a fault says the command must not take effect.
  bool run = true;
  std::optional<size_t> arrayCount;
};

std::string recordName(const Parsed &p) {
  switch (p.kind) {
  case Parsed::Kind::Empty: return "empty";
  case Parsed::Kind::Bye: return "bye";
  case Parsed::Kind::Bad: return "invalid";
  case Parsed::Kind::Request: break;
  }
  switch (*p.type) {
  case 9: return "resolve";
  case 10: return "cpukey";
  case 11: return "shutdown";
  case 12: return "notify";
  case 13: return "kernel";
  case 14: return "leds";
  case 15: return "temperature";
  case 16: return "titleid";
  case 17: return "consoletype";
  case 18: return "constmem";
  case 19: return "opcode19";
  default: return "call";
  }
}

// What a failure of the kind `policy` answers.
Reply failureReply(JrpcOnFailure policy, std::string_view text) {
  switch (policy) {
  case JrpcOnFailure::ErrorLine: return lineReply("error=" + std::string(text));
  case JrpcOnFailure::Silent: return Reply{};
  case JrpcOnFailure::Close: break;
  }
  Reply r;
  r.after = Reply::After::Close;
  return r;
}

Reply silentOpcodeReply(const JrpcMockOptions &options) {
  switch (options.silentOpcodes) {
  case JrpcSilentOpcodeAnswer::SOk: return lineReply("S_OK");
  case JrpcSilentOpcodeAnswer::Zero: return lineReply("0");
  case JrpcSilentOpcodeAnswer::Silent: break;
  }
  return Reply{};
}

bool hasArgs(const Parsed &p, std::initializer_list<char> tags) {
  if (p.args.size() < tags.size()) return false;
  size_t i = 0;
  for (char tag : tags) {
    if (p.args[i++].tag != tag) return false;
  }
  return true;
}

std::string unresolvedText(const Parsed &p) {
  return "Could not resolve function address, params = " + p.params + ", " + std::to_string(*p.type);
}

Reply execute(State &st, Request &req) {
  const Parsed &p = req.parsed;
  const JrpcMockOptions &opt = req.options;
  const int type = *p.type;

  if (type <= 8) {
    if (!req.function) return failureReply(opt.onUnknownTarget, unresolvedText(p));
    if (!req.run) return Reply{};
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      ++st.callCounts[req.call.address];
    }
    const JrpcReturn r = (*req.function)(req.call);
    const auto line = callReply(req.call, r, opt, req.arrayCount);
    return line ? lineReply(*line) : Reply{};
  }

  auto badArgs = [&] { return failureReply(opt.onMalformed, kBadParams); };
  switch (type) {
  case 9: {
    if (!hasArgs(p, {'2', '1'})) return badArgs();
    const std::string module = lower(p.args[0].text());
    std::optional<uint32_t> address;
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      const auto it = st.exports.find({module, p.args[1].u32()});
      if (it != st.exports.end()) address = it->second;
    }
    if (!address) return failureReply(opt.onUnknownTarget, unresolvedText(p));
    return lineReply(format("%X", static_cast<unsigned>(*address)));
  }
  case 10: return lineReply(cpuKeyText(req.info, opt));
  case 11: {
    if (req.run) {
      std::lock_guard<std::mutex> lock(st.mutex);
      st.events.push_back("shutdown");
    }
    Reply r = req.run ? silentOpcodeReply(opt) : Reply{};
    if (req.run) r.after = Reply::After::Shutdown;
    return r;
  }
  case 12: {
    if (!hasArgs(p, {'2', '1'})) return badArgs();
    if (!req.run) return Reply{};
    JrpcNotification n{p.args[0].text(), p.args[1].u32()};
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      st.events.push_back(format("notify %u ", static_cast<unsigned>(n.type)) + n.text);
      st.notifications.push_back(std::move(n));
    }
    return silentOpcodeReply(opt);
  }
  case 13: return lineReply(format("%d", static_cast<int>(req.info.kernelVersion)));
  case 14: {
    if (!hasArgs(p, {'1', '1', '1', '1'})) return badArgs();
    if (!req.run) return Reply{};
    const JrpcLedWrite w{static_cast<int32_t>(p.args[0].integer), static_cast<int32_t>(p.args[1].integer),
                         static_cast<int32_t>(p.args[2].integer), static_cast<int32_t>(p.args[3].integer)};
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      st.events.push_back(format("leds %d %d %d %d", w.topLeft, w.topRight, w.bottomLeft, w.bottomRight));
      st.leds.push_back(w);
    }
    return silentOpcodeReply(opt);
  }
  case 15: {
    if (!hasArgs(p, {'1'})) return badArgs();
    const int64_t which = p.args[0].integer;
    const uint32_t value = which >= 0 && which < 4 ? req.info.temperatures[static_cast<size_t>(which)] : 0;
    return lineReply(format("%X", static_cast<unsigned>(value)));
  }
  case 16: return lineReply(format("%X", static_cast<unsigned>(req.info.titleId)));
  case 17: return lineReply(req.info.consoleType);
  case 18: {
    if (!hasArgs(p, {'1', '1', '1', '1', '1'})) return badArgs();
    if (!req.run) return Reply{};
    const JrpcMemoryTask t{p.address,        p.args[0].u32(), p.args[1].u32(),
                           p.args[2].u32(), p.args[3].u32(), p.args[4].u32()};
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      st.events.push_back("constmem " + hex8(t.address) + " " + hex8(t.value) + " " + hex8(t.useIf) + " " +
                          hex8(t.ifValue) + " " + hex8(t.useTitle) + " " + hex8(t.titleId));
      st.tasks.push_back(t);
    }
    return silentOpcodeReply(opt);
  }
  default:
    // Opcode 19 builds a byte string and has no documented reply or effect.
    return Reply{};
  }
}

void serveConnection(State &st, Connection &c) {
  auto end = [&] {
    c.transport->close();
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      if (c.counted) --st.active;
      c.busy = false;
    }
    st.slotFree.notify_all();
  };

  {
    // Section 1.1: the server keeps a table of sockets and a connection beyond it
    // waits, unserved and without a banner, for a slot.
    std::unique_lock<std::mutex> lock(st.mutex);
    st.slotFree.wait(lock, [&] {
      return st.stopping || (!st.queue.empty() && st.queue.front() == c.index && st.active < st.options.connectionLimit);
    });
    if (st.stopping) {
      lock.unlock();
      end();
      return;
    }
    st.queue.pop_front();
    ++st.active;
    ++st.accepted;
    c.counted = true;
  }
  st.slotFree.notify_all();

  bool sendBanner = false;
  std::string banner;
  std::optional<JrpcFault> greetingFault;
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    sendBanner = st.options.sendBanner;
    banner = st.options.banner;
    if (sendBanner) greetingFault = st.takeFault(std::nullopt, std::nullopt, "", c.index, true);
  }
  if (sendBanner) {
    Sender sender(st, c, std::move(greetingFault));
    if (!sender.send(lineReply(banner))) {
      end();
      return;
    }
  }

  while (true) {
    size_t bufferBytes = 0;
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      bufferBytes = st.options.bufferBytes;
    }
    auto received = readLine(c, bufferBytes);
    if (!received) break;
    // The options in force when the line arrived, not when the connection last looked.
    JrpcMockOptions options;
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      options = st.options;
    }

    Request req;
    req.options = options;
    if (!received->overLong) req.parsed = parseLine(received->text, options);

    JrpcCommandRecord record;
    record.connection = c.index;
    record.line = received->text;
    record.pipelined = received->pipelined;
    record.overLong = received->overLong;
    record.length = received->length;
    record.name = received->overLong ? "overlong" : recordName(req.parsed);
    record.type = received->overLong ? std::nullopt : req.parsed.type;

    std::optional<JrpcFault> fault;
    bool isRequest = !received->overLong && req.parsed.kind == Parsed::Kind::Request;
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      req.info = st.info;
      std::optional<uint32_t> faultAddress;
      if (isRequest && *req.parsed.type <= 8) {
        const Parsed &p = req.parsed;
        JrpcCall &call = req.call;
        call.type = *p.type;
        call.system = p.system;
        call.module = p.module;
        call.ordinal = p.ordinal;
        call.arraySize = p.arraySize;
        call.args = p.args;
        call.connection = c.index;
        bool resolved = true;
        if (p.module) {
          const auto it = st.exports.find({lower(*p.module), p.ordinal});
          if (it != st.exports.end()) call.address = it->second;
          else resolved = false;
        } else {
          call.address = p.address;
        }
        if (resolved) {
          const auto fn = st.functions.find(call.address);
          if (fn != st.functions.end()) req.function = fn->second;
        }
        faultAddress = call.address;
        st.calls.push_back(call);
      }
      st.commands.push_back(record);
      if (received->overLong || req.parsed.kind == Parsed::Kind::Request || req.parsed.kind == Parsed::Kind::Bad) {
        fault = st.takeFault(record.type, faultAddress, received->text, c.index, false);
      }
      c.busy = true;
    }

    if (!received->overLong && req.parsed.kind == Parsed::Kind::Bye) break;
    if (!received->overLong && req.parsed.kind == Parsed::Kind::Empty) {
      std::lock_guard<std::mutex> lock(st.mutex);
      c.busy = false;
      continue;
    }

    Reply reply;
    if (fault) {
      req.run = !fault->skipCommand;
      req.arrayCount = fault->arrayCount;
    }
    if (received->overLong) {
      reply = failureReply(options.onOverLong, "line too long");
    } else if (req.parsed.kind == Parsed::Kind::Bad) {
      reply = failureReply(options.onMalformed, req.parsed.error);
    } else {
      reply = execute(st, req);
    }

    // A shutdown takes down the connections open when it was asked for.
    std::vector<std::shared_ptr<Connection>> going;
    if (reply.after == Reply::After::Shutdown && options.shutdownDropsAllConnections) {
      std::lock_guard<std::mutex> lock(st.mutex);
      for (const auto &other : st.connections) {
        if (!other->finished) going.push_back(other);
      }
    }

    if (!reply.bytes.empty() || fault) {
      Sender sender(st, c, std::move(fault));
      if (!sender.send(reply)) break;
    }
    if (reply.after == Reply::After::Close) break;
    if (reply.after == Reply::After::Shutdown) {
      for (const auto &other : going) other->transport->close();
      break;
    }
    std::lock_guard<std::mutex> lock(st.mutex);
    c.busy = false;
  }
  end();
}

void startConnection(const std::shared_ptr<State> &st, net::TransportPtr transport) {
  auto c = std::make_shared<Connection>();
  c->transport = std::move(transport);
  std::lock_guard<std::mutex> lock(st->mutex);
  for (auto it = st->connections.begin(); it != st->connections.end();) {
    if ((*it)->finished) {
      if ((*it)->thread.joinable()) (*it)->thread.join();
      it = st->connections.erase(it);
    } else {
      ++it;
    }
  }
  if (st->stopping) {
    c->transport->close();
    return;
  }
  c->index = st->arrived++;
  st->queue.push_back(c->index);
  st->connections.push_back(c);
  c->thread = std::thread([st, c] {
    serveConnection(*st, *c);
    c->finished = true;
  });
}

template <class Pred> bool pollUntil(Pred pred, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

} // namespace

// --- JrpcMockServer ---

JrpcMockServer::JrpcMockServer(JrpcMockOptions options) : state_(std::make_shared<State>()) {
  state_->options = std::move(options);
}

JrpcMockServer::~JrpcMockServer() { stop(); }

JrpcMockOptions JrpcMockServer::options() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->options;
}

void JrpcMockServer::setOptions(const JrpcMockOptions &options) {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->options = options;
  }
  // A higher connection limit frees the connections held for a slot.
  state_->slotFree.notify_all();
}

Result<net::TransportPtr> JrpcMockServer::connect() {
  size_t capacity = 0;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping) return fail(ErrorCode::ConnectFailed, "mock server stopped");
    capacity = state_->options.pipeCapacity;
  }
  auto pipe = MemoryPipe::create(capacity);
  startConnection(state_, std::move(pipe.server));
  return net::TransportPtr(std::move(pipe.client));
}

std::function<Result<net::TransportPtr>()> JrpcMockServer::connector() {
  return [this] { return connect(); };
}

void JrpcMockServer::serve(net::TransportPtr transport) { startConnection(state_, std::move(transport)); }

Result<uint16_t> JrpcMockServer::listenTcp() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->tcpPort != 0) return state_->tcpPort;
  if (state_->stopping) return fail(ErrorCode::ConnectFailed, "mock server stopped");
  if (!netStartup()) return fail(ErrorCode::ConnectFailed, "socket runtime unavailable");
  SocketHandle listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listener.valid()) return fail(ErrorCode::ConnectFailed, "socket() failed", nativeError());
  sockaddr_in address = loopbackAddress(0);
  if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    return fail(ErrorCode::ConnectFailed, "bind() failed", nativeError());
  }
  if (::listen(listener.get(), 16) != 0) return fail(ErrorCode::ConnectFailed, "listen() failed", nativeError());
  sockaddr_in bound{};
#if defined(_WIN32)
  int length = sizeof(bound);
#else
  socklen_t length = sizeof(bound);
#endif
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
    return fail(ErrorCode::ConnectFailed, "getsockname() failed", nativeError());
  }
  state_->tcpPort = ntohs(bound.sin_port);
  state_->listener = std::move(listener);
  std::weak_ptr<State> weak = state_;
  state_->acceptThread = std::thread([weak, raw = state_.get()] {
    while (!raw->stopFlag) {
      if (!waitReadableMs(raw->listener.get(), 20)) continue;
      NativeSocket accepted = ::accept(raw->listener.get(), nullptr, nullptr);
      if (accepted == kNoSocket) continue;
      const int on = 1;
      (void)::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
      auto st = weak.lock();
      if (!st) return;
      startConnection(st, std::make_unique<TcpServerTransport>(SocketHandle(accepted)));
    }
  });
  return state_->tcpPort;
}

uint16_t JrpcMockServer::tcpPort() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->tcpPort;
}

void JrpcMockServer::dropAllConnections() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->closeAllConnections();
}

void JrpcMockServer::stop() {
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true;
    state_->stopFlag = true;
  }
  state_->slotFree.notify_all();
  if (state_->acceptThread.joinable()) state_->acceptThread.join();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closeAllConnections();
    connections = state_->connections;
  }
  for (auto &c : connections) {
    if (c->thread.joinable()) c->thread.join();
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->connections.clear();
  state_->listener.reset();
}

size_t JrpcMockServer::connectionsAccepted() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->accepted;
}

size_t JrpcMockServer::activeConnections() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->active;
}

size_t JrpcMockServer::waitingConnections() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->queue.size();
}

bool JrpcMockServer::waitForActiveConnections(size_t count, std::chrono::milliseconds limit) const {
  return pollUntil([&] { return activeConnections() == count; }, limit);
}

bool JrpcMockServer::waitForWaitingConnections(size_t count, std::chrono::milliseconds limit) const {
  return pollUntil([&] { return waitingConnections() == count; }, limit);
}

bool JrpcMockServer::waitForCommands(size_t count, std::chrono::milliseconds limit) const {
  return pollUntil(
      [&] {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->commands.size() >= count;
      },
      limit);
}

bool JrpcMockServer::waitUntilIdle(std::chrono::milliseconds limit) const {
  return pollUntil(
      [&] {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (const auto &c : state_->connections) {
          if (!c->finished && c->busy) return false;
        }
        return true;
      },
      limit);
}

void JrpcMockServer::inject(JrpcFault fault) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->faults.push_back(ArmedFault{std::move(fault), 0, 0});
}

void JrpcMockServer::clearFaults() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->faults.clear();
}

size_t JrpcMockServer::pendingFaults() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->faults.size();
}

std::vector<JrpcCommandRecord> JrpcMockServer::commands() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->commands;
}

std::vector<std::string> JrpcMockServer::commandLines() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<std::string> lines;
  lines.reserve(state_->commands.size());
  for (const auto &c : state_->commands) lines.push_back(c.line);
  return lines;
}

void JrpcMockServer::clearCommands() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->commands.clear();
  state_->calls.clear();
}

std::vector<JrpcCall> JrpcMockServer::calls() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->calls;
}

std::vector<JrpcNotification> JrpcMockServer::notifications() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->notifications;
}

std::vector<JrpcLedWrite> JrpcMockServer::ledWrites() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->leds;
}

std::vector<JrpcMemoryTask> JrpcMockServer::memoryTasks() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->tasks;
}

std::vector<std::string> JrpcMockServer::events() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->events;
}

void JrpcMockServer::clearEvents() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->events.clear();
  state_->notifications.clear();
  state_->leds.clear();
  state_->tasks.clear();
}

JrpcConsoleInfo JrpcMockServer::info() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->info;
}

void JrpcMockServer::setInfo(const JrpcConsoleInfo &info) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->info = info;
}

void JrpcMockServer::registerFunction(uint32_t address, JrpcFunction function) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->functions[address] = std::make_shared<JrpcFunction>(std::move(function));
}

uint32_t JrpcMockServer::registerFunction(std::string_view module, uint32_t ordinal, JrpcFunction function) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto key = std::make_pair(lower(module), ordinal);
  auto it = state_->exports.find(key);
  if (it == state_->exports.end()) {
    it = state_->exports.emplace(key, state_->nextAddress).first;
    state_->nextAddress += 0x10;
  }
  state_->functions[it->second] = std::make_shared<JrpcFunction>(std::move(function));
  return it->second;
}

void JrpcMockServer::registerExport(std::string_view module, uint32_t ordinal, uint32_t address) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->exports[std::make_pair(lower(module), ordinal)] = address;
}

void JrpcMockServer::unregisterFunction(uint32_t address) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->functions.erase(address);
}

void JrpcMockServer::clearFunctions() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->functions.clear();
  state_->exports.clear();
}

size_t JrpcMockServer::callCount(uint32_t address) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto it = state_->callCounts.find(address);
  return it == state_->callCounts.end() ? 0 : it->second;
}

} // namespace ut
