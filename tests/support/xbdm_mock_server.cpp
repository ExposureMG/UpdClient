#include "support/xbdm_mock_server.hpp"

#include "support/loopback_server.hpp"
#include "support/memory_transport.hpp"

#include <net/udp_socket.hpp>

#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace ut {

namespace {

using updclient::ErrorCode;
using updclient::Result;
using updclient::fail;
namespace net = updclient::net;

constexpr uint16_t kNamePort = 730;
constexpr size_t kChunk = 64 * 1024;

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool sameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

std::string format(const char *pattern, auto... values) {
  char buffer[512];
  const int n = std::snprintf(buffer, sizeof(buffer), pattern, values...);
  return std::string(buffer, n > 0 ? std::min<size_t>(static_cast<size_t>(n), sizeof(buffer) - 1) : 0);
}

std::string hex32(uint64_t value) {
  return format("0x%08x", static_cast<unsigned>(value & 0xFFFFFFFFu));
}

uint32_t hiHalf(uint64_t value) { return static_cast<uint32_t>(value >> 32); }
uint32_t loHalf(uint64_t value) { return static_cast<uint32_t>(value); }

// --- Command lines, parsed from section 1.3 only ---

struct Args {
  std::string name;
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<std::string> flags;

  const std::string *get(std::string_view key) const {
    for (const auto &[k, v] : values) {
      if (k == key) return &v;
    }
    return nullptr;
  }
  bool flag(std::string_view name) const {
    return std::find(flags.begin(), flags.end(), name) != flags.end();
  }
};

Args parseCommand(std::string_view line) {
  Args args;
  size_t i = 0;
  auto skipSpaces = [&] {
    while (i < line.size() && line[i] == ' ') ++i;
  };
  skipSpaces();
  const size_t nameStart = i;
  while (i < line.size() && line[i] != ' ') ++i;
  args.name = lower(line.substr(nameStart, i - nameStart));
  while (true) {
    skipSpaces();
    if (i >= line.size()) break;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '=') ++i;
    std::string key = lower(line.substr(start, i - start));
    if (i < line.size() && line[i] == '=') {
      ++i;
      std::string value;
      if (i < line.size() && line[i] == '"') {
        const size_t close = line.find('"', i + 1);
        const size_t end = close == std::string_view::npos ? line.size() : close;
        value = std::string(line.substr(i + 1, end - i - 1));
        i = close == std::string_view::npos ? line.size() : close + 1;
      } else {
        const size_t vs = i;
        while (i < line.size() && line[i] != ' ') ++i;
        value = std::string(line.substr(vs, i - vs));
      }
      args.values.emplace_back(std::move(key), std::move(value));
    } else {
      args.flags.push_back(std::move(key));
    }
  }
  return args;
}

std::optional<uint64_t> parseNumber(std::string_view text) {
  if (text.empty()) return std::nullopt;
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'q' || text[1] == 'Q')) {
    base = 16;
    text.remove_prefix(2);
  }
  uint64_t value = 0;
  for (char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    if (digit < 0) return std::nullopt;
    if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(digit)) / static_cast<uint64_t>(base)) {
      return std::nullopt;
    }
    value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
  }
  return value;
}

std::optional<uint32_t> number32(const Args &args, std::string_view key) {
  const auto *text = args.get(key);
  if (!text) return std::nullopt;
  const auto value = parseNumber(*text);
  if (!value || *value > 0xFFFFFFFFull) return std::nullopt;
  return static_cast<uint32_t>(*value);
}

// --- Console paths (section 4.1) ---

struct ConsolePath {
  std::string drive;
  std::vector<std::string> parts;
};

// "DRIVE:", "DRIVE:\" and "DRIVE:\a\b" with at most one trailing backslash.
std::optional<ConsolePath> splitPath(std::string_view path) {
  const size_t colon = path.find(':');
  if (colon == std::string_view::npos || colon == 0) return std::nullopt;
  ConsolePath out;
  out.drive = std::string(path.substr(0, colon));
  std::string_view rest = path.substr(colon + 1);
  if (rest.empty()) return out;
  if (rest.front() != '\\') return std::nullopt;
  rest.remove_prefix(1);
  if (!rest.empty() && rest.back() == '\\') rest.remove_suffix(1);
  while (!rest.empty()) {
    const size_t slash = rest.find('\\');
    const auto part = rest.substr(0, slash);
    if (part.empty()) return std::nullopt;
    out.parts.emplace_back(part);
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
    if (rest.empty()) return std::nullopt;
  }
  return out;
}

bool validName(std::string_view name, size_t maxLength) {
  if (name.empty() || name == "." || name == "..") return false;
  if (maxLength != 0 && name.size() > maxLength) return false;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return false;
    if (std::string_view("\"*/:<>?\\|").find(c) != std::string_view::npos) return false;
  }
  return true;
}

// --- The virtual console ---

struct Node {
  std::string name;
  bool directory = false;
  std::shared_ptr<Bytes> data = std::make_shared<Bytes>();
  bool stored = true;
  bool isVirtual = false;
  uint32_t seed = 0;
  uint64_t size = 0;
  Fnv1a digest;
  XbdmEntryOptions options;
  std::vector<std::unique_ptr<Node>> children;

  uint64_t length() const { return stored ? data->size() : size; }
  Node *find(std::string_view child) const {
    for (const auto &c : children) {
      if (sameName(c->name, child)) return c.get();
    }
    return nullptr;
  }
  void remove(const Node *child) {
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [&](const std::unique_ptr<Node> &c) { return c.get() == child; }),
                   children.end());
  }
};

struct DriveState {
  XbdmDrive drive;
  Node root;
};

struct Region {
  XbdmMemoryRegion info;
  Bytes data;
  std::vector<bool> readable;
};

struct ArmedFault {
  XbdmFault fault;
  size_t seen = 0;
  int fired = 0;
};

struct Connection {
  size_t index = 0;
  net::TransportPtr transport;
  std::thread thread;
  std::atomic<bool> finished{false};
  std::mutex writeMutex;
  std::string input;
  bool discarding = false;
  // Guarded by the state mutex.
  bool dedicated = false;
  bool busy = false;
  bool counted = false;
};

// The server end of an accepted TCP connection. close() shuts the socket down,
// which wakes a recv or send blocked on another thread; the descriptor is released
// only by the destructor, after the serving thread has been joined.
class TcpServerTransport final : public net::ITransport {
public:
  explicit TcpServerTransport(SocketHandle socket) : socket_(std::move(socket)) {}

  bool isOpen() const noexcept override { return !closed_.load(); }
  void close() noexcept override {
    if (closed_.exchange(true)) return;
#if defined(_WIN32)
    ::shutdown(socket_.get(), SD_BOTH);
#else
    ::shutdown(socket_.get(), SHUT_RDWR);
#endif
  }
  std::string describe() const override { return "tcp-server://127.0.0.1"; }
  Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    timeoutMs_ = timeout.count();
    return {};
  }
  Result<size_t> readSome(std::span<uint8_t> buffer) override {
    if (closed_) return fail(ErrorCode::NotConnected, "closed");
    if (buffer.empty()) return size_t{0};
    const auto ms = timeoutMs_.load();
    if (ms > 0 && !waitReadableMs(socket_.get(), static_cast<int>(ms))) {
      if (closed_) return fail(ErrorCode::Cancelled, "closed");
      return fail(ErrorCode::Timeout, "read timed out");
    }
    const int n = ::recv(socket_.get(), reinterpret_cast<char *>(buffer.data()),
                         static_cast<int>(std::min<size_t>(buffer.size(), 1 << 20)), 0);
    if (closed_) return fail(ErrorCode::Cancelled, "closed");
    if (n < 0) return fail(ErrorCode::Disconnected, "recv failed", nativeError());
    return static_cast<size_t>(n);
  }
  Result<size_t> writeSome(std::span<const uint8_t> data) override {
    if (closed_) return fail(ErrorCode::NotConnected, "closed");
    if (data.empty()) return size_t{0};
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    const int n = ::send(socket_.get(), reinterpret_cast<const char *>(data.data()),
                         static_cast<int>(std::min<size_t>(data.size(), 1 << 20)), kFlags);
    if (closed_) return fail(ErrorCode::Cancelled, "closed");
    if (n <= 0) return fail(ErrorCode::Disconnected, "send failed", nativeError());
    return static_cast<size_t>(n);
  }

private:
  SocketHandle socket_;
  std::atomic<bool> closed_{false};
  std::atomic<long long> timeoutMs_{0};
};

const char *errorText(int code) {
  switch (code) {
  case 400: return "unknown error";
  case 401: return "max number of connections exceeded";
  case 402: return "file not found";
  case 404: return "memory not mapped";
  case 405: return "no such thread";
  case 406: return "line too long";
  case 407: return "unknown command";
  case 408: return "not stopped";
  case 409: return "file must be copied";
  case 410: return "file already exists";
  case 411: return "directory not empty";
  case 412: return "filename is invalid";
  case 413: return "file cannot be created";
  case 414: return "access denied";
  case 415: return "no room on device";
  case 423: return "invalid argument";
  case 426: return "already stopped";
  default: return "error";
  }
}

// What a command answers: a head, then an optional body produced in pieces.
struct Reply {
  enum class After { Continue, Close, Reboot, Dedicate };

  Bytes head;
  uint64_t bodyLength = 0;
  std::function<void(uint64_t offset, std::span<uint8_t> out)> body;
  // A length claim beyond the data: after it the console stalls (section 5.1).
  bool stallAtEnd = false;
  After after = After::Continue;
};

Reply line(std::string_view text, Reply::After after = Reply::After::Continue) {
  Reply r;
  append(r.head, text);
  append(r.head, "\r\n");
  r.after = after;
  return r;
}

Reply status(int code) { return line(format("%d- %s", code, errorText(code))); }
Reply status(int code, std::string_view text) { return line(format("%d- ", code) + std::string(text)); }

Reply multiline(const std::vector<std::string> &body) {
  Reply r;
  append(r.head, "202- multiline response follows\r\n");
  for (const auto &l : body) {
    append(r.head, l);
    append(r.head, "\r\n");
  }
  append(r.head, ".\r\n");
  return r;
}

uint8_t le(uint64_t value, int byte) { return static_cast<uint8_t>(value >> (8 * byte)); }

// Deterministic damage for the hostile fault.
void mangle(Bytes &bytes, uint64_t seed, bool &closeEarly) {
  std::mt19937_64 rng(seed);
  const int edits = 1 + static_cast<int>(rng() % 4);
  for (int e = 0; e < edits; ++e) {
    const auto op = rng() % 6;
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
    case 4:
      if (bytes.size() >= 3) {
        static constexpr std::array<const char *, 6> codes = {"299", "500", "1xx", "203", "202", "abc"};
        const char *code = codes[rng() % codes.size()];
        std::copy_n(code, 3, bytes.begin());
      }
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

XbdmFault XbdmFault::dropAfterBytes(size_t bytes) {
  XbdmFault f;
  f.dropAfter = bytes;
  return f;
}
XbdmFault XbdmFault::stall(size_t afterBytes, std::chrono::milliseconds duration) {
  XbdmFault f;
  f.stallAfter = afterBytes;
  f.stallFor = duration;
  return f;
}
XbdmFault XbdmFault::silence() {
  XbdmFault f = stall(0);
  f.times = -1;
  return f;
}
XbdmFault XbdmFault::reply(std::string_view raw) { return reply(bytesOf(raw)); }
XbdmFault XbdmFault::reply(Bytes raw) {
  XbdmFault f;
  f.replaceWith = std::move(raw);
  return f;
}
XbdmFault XbdmFault::statusLine(std::string_view text) { return reply(std::string(text) + "\r\n"); }
XbdmFault XbdmFault::refusal(std::string_view text) {
  XbdmFault f = statusLine(text);
  f.skipCommand = true;
  return f;
}
XbdmFault XbdmFault::oversizedLine(size_t length) {
  std::string text(length, 'x');
  text.replace(0, 5, "200- ");
  return reply(text + "\r\n");
}
XbdmFault XbdmFault::endlessLine() {
  XbdmFault f = reply(std::string_view("200- "));
  f.endless = true;
  return f;
}
XbdmFault XbdmFault::trickleBytes() {
  XbdmFault f;
  f.trickle = true;
  return f;
}
XbdmFault XbdmFault::hostile(uint64_t seed) {
  XbdmFault f;
  f.hostileSeed = seed;
  return f;
}
XbdmFault XbdmFault::claimLength(uint64_t length) {
  XbdmFault f;
  f.lengthClaim = length;
  return f;
}
XbdmFault XbdmFault::extraBytes(Bytes bytes) {
  XbdmFault f;
  f.trailer = std::move(bytes);
  return f;
}
XbdmFault XbdmFault::memexBlockHeader(uint16_t header) {
  XbdmFault f;
  f.memexHeader = header;
  return f;
}
XbdmFault XbdmFault::dropUploadAfterBytes(uint64_t bytes) {
  XbdmFault f;
  f.dropUploadAfter = bytes;
  return f;
}
XbdmFault XbdmFault::stallUploadAfterBytes(uint64_t bytes) {
  XbdmFault f;
  f.stallUploadAfter = bytes;
  return f;
}
XbdmFault XbdmFault::uploadAnswer(std::string text) {
  XbdmFault f;
  f.uploadReply = std::move(text);
  return f;
}

XbdmFault &XbdmFault::on(std::string_view commandName) {
  command = lower(commandName);
  greeting = false;
  return *this;
}
XbdmFault &XbdmFault::onGreeting() {
  greeting = true;
  command.clear();
  return *this;
}
XbdmFault &XbdmFault::onConnection(size_t index) {
  connection = index;
  return *this;
}
XbdmFault &XbdmFault::after(size_t matches) {
  skip = matches;
  return *this;
}
XbdmFault &XbdmFault::repeat(int count) {
  times = count;
  return *this;
}
XbdmFault &XbdmFault::always() {
  times = -1;
  return *this;
}

// --- Server state ---

struct XbdmMockServer::State {
  mutable std::mutex mutex;
  XbdmMockOptions options;
  XbdmConsoleInfo info;
  XbdmScreenshot shot;
  std::vector<std::unique_ptr<DriveState>> drives;
  std::vector<Region> regions;
  std::vector<XbdmModule> modules;
  std::vector<XbdmThread> threads;
  std::vector<ArmedFault> faults;
  std::vector<XbdmCommandRecord> commands;
  std::vector<XbdmUploadRecord> uploads;
  std::vector<std::string> events;
  std::vector<std::string> notificationsBefore;
  std::vector<std::shared_ptr<Connection>> connections;
  size_t accepted = 0;
  size_t refused = 0;
  size_t active = 0;
  XbdmUdpMode udpMode = XbdmUdpMode::Answer;
  size_t nameRequests = 0;
  bool stopping = false;

  std::atomic<bool> stopFlag{false};
  SocketHandle listener;
  std::thread acceptThread;
  uint16_t tcpPort = 0;
  std::unique_ptr<net::UdpSocket> udp;
  std::thread udpThread;
  uint16_t udpPort = 0;

  // --- Lookup, with the state mutex held ---

  DriveState *findDrive(std::string_view name) {
    for (auto &d : drives) {
      if (sameName(d->drive.name, name)) return d.get();
    }
    return nullptr;
  }

  struct Found {
    DriveState *drive = nullptr;
    Node *node = nullptr;
    Node *parent = nullptr;
    bool protectedPath = false;
  };

  // node is null when the last part is missing; parent is null when the folder that
  // should hold it is missing too.
  Found locate(const ConsolePath &path) {
    Found found;
    found.drive = findDrive(path.drive);
    if (!found.drive || !found.drive->drive.mounted) {
      found.drive = nullptr;
      return found;
    }
    Node *current = &found.drive->root;
    for (size_t i = 0; i < path.parts.size(); ++i) {
      if (current->options.protectedEntry) found.protectedPath = true;
      if (!current->directory) {
        found.parent = nullptr;
        return found;
      }
      found.parent = current;
      current = current->find(path.parts[i]);
      if (!current) {
        if (i + 1 < path.parts.size()) found.parent = nullptr;
        return found;
      }
    }
    if (current->options.protectedEntry) found.protectedPath = true;
    found.node = current;
    return found;
  }

  Result<Node *> create(std::string_view text, bool directory, bool makeParents) {
    const auto path = splitPath(text);
    if (!path || path->parts.empty()) return fail(ErrorCode::InvalidArgument, "bad path: " + std::string(text));
    DriveState *drive = findDrive(path->drive);
    if (!drive) return fail(ErrorCode::InvalidArgument, "no drive " + path->drive);
    Node *current = &drive->root;
    for (size_t i = 0; i + 1 < path->parts.size(); ++i) {
      Node *next = current->find(path->parts[i]);
      if (!next) {
        if (!makeParents) return fail(ErrorCode::InvalidArgument, "missing parent");
        auto child = std::make_unique<Node>();
        child->name = path->parts[i];
        child->directory = true;
        next = child.get();
        current->children.push_back(std::move(child));
      }
      if (!next->directory) return fail(ErrorCode::InvalidArgument, "a parent is a file");
      current = next;
    }
    Node *node = current->find(path->parts.back());
    if (!node) {
      auto child = std::make_unique<Node>();
      child->name = path->parts.back();
      node = child.get();
      current->children.push_back(std::move(child));
    }
    node->directory = directory;
    node->children.clear();
    node->data = std::make_shared<Bytes>();
    node->stored = true;
    node->isVirtual = false;
    node->size = 0;
    node->digest = Fnv1a{};
    return node;
  }

  Region *regionAt(uint32_t address) {
    for (auto &r : regions) {
      if (address >= r.info.base && address - r.info.base < r.info.size) return &r;
    }
    return nullptr;
  }

  std::optional<uint8_t> readByte(uint32_t address) {
    Region *r = regionAt(address);
    if (!r) return std::nullopt;
    const size_t at = address - r->info.base;
    if (!r->readable[at]) return std::nullopt;
    return r->data[at];
  }

  std::optional<XbdmFault> takeFault(std::string_view command, size_t connection, bool greeting) {
    for (size_t i = 0; i < faults.size(); ++i) {
      auto &armed = faults[i];
      const auto &f = armed.fault;
      if (f.greeting != greeting) continue;
      if (!greeting && !f.command.empty() && f.command != command) continue;
      if (f.connection && *f.connection != connection) continue;
      if (armed.seen < f.skip) {
        ++armed.seen;
        continue;
      }
      XbdmFault copy = f;
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

using State = XbdmMockServer::State;

// Sends one reply and applies the fault armed for it. Every byte of the reply, and
// for sendfile also the status after the data, counts toward the fault's offsets.
class Sender {
public:
  Sender(State &state, Connection &connection, std::optional<XbdmFault> fault)
      : state_(state), c_(connection), fault_(std::move(fault)) {}

  const std::optional<XbdmFault> &fault() const { return fault_; }

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

  bool put(std::string_view text) {
    return put(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  }

  // Sends a whole reply; false when the connection is over.
  bool send(Reply &reply) {
    bool closeEarly = false;
    if (fault_ && fault_->replaceWith) {
      // The replacement is the whole answer: a refused reboot does not reboot.
      reply.after = Reply::After::Continue;
      if (!put(*fault_->replaceWith)) return false;
      return finish();
    }
    if (fault_ && fault_->hostileSeed) {
      Bytes all = reply.head;
      const bool small = reply.bodyLength <= (1u << 20);
      if (small && reply.bodyLength > 0) {
        const size_t at = all.size();
        all.resize(at + static_cast<size_t>(reply.bodyLength));
        reply.body(0, std::span<uint8_t>(all).subspan(at));
      }
      mangle(all, *fault_->hostileSeed, closeEarly);
      if (!put(all)) return false;
      if (closeEarly) return drop();
      if (!small && !body(reply)) return false;
      return finish();
    }
    if (!put(reply.head)) return false;
    if (!body(reply)) return false;
    if (reply.stallAtEnd && !stall(std::chrono::milliseconds(0))) return false;
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
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
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
  bool body(Reply &reply) {
    Bytes chunk;
    for (uint64_t offset = 0; offset < reply.bodyLength;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, reply.bodyLength - offset));
      chunk.resize(n);
      reply.body(offset, chunk);
      if (!put(chunk)) return false;
      offset += n;
    }
    return true;
  }

  bool raw(std::span<const uint8_t> data) {
    std::lock_guard<std::mutex> lock(c_.writeMutex);
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
  std::optional<XbdmFault> fault_;
  uint64_t sent_ = 0;
  bool stalled_ = false;
};

// --- The connection loop ---

struct LineRead {
  std::string text;
  bool pipelined = false;
  bool overLong = false;
};

std::optional<LineRead> readLine(State &state, Connection &c) {
  size_t maxLine = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    maxLine = state.options.maxLineLength;
  }
  std::array<uint8_t, kChunk> buffer{};
  std::string kept;
  bool overLong = false;
  while (true) {
    const size_t lf = c.input.find('\n');
    if (lf != std::string::npos) {
      std::string piece = c.input.substr(0, lf);
      c.input.erase(0, lf + 1);
      if (!overLong) kept += piece;
      if (!kept.empty() && kept.back() == '\r') kept.pop_back();
      if (kept.size() > maxLine) {
        kept.resize(maxLine);
        overLong = true;
      }
      return LineRead{std::move(kept), !c.input.empty(), overLong};
    }
    if (!overLong) {
      kept += c.input;
      if (kept.size() > maxLine + 1) {
        kept.resize(maxLine);
        overLong = true;
      }
    }
    c.input.clear();
    auto n = c.transport->readSome(buffer);
    if (!n || *n == 0) return std::nullopt;
    c.input.append(reinterpret_cast<const char *>(buffer.data()), *n);
  }
}

// --- Commands ---

struct CommandContext {
  State &state;
  Connection &c;
  const Args &args;
};

std::string entryLine(const Node &node, bool withName) {
  std::string text;
  if (withName) text = "name=\"" + node.name + "\" ";
  const uint64_t size = node.directory ? 0 : node.length();
  text += "sizehi=" + hex32(hiHalf(size)) + " sizelo=" + hex32(loHalf(size));
  text += " createhi=" + hex32(hiHalf(node.options.created)) + " createlo=" + hex32(loHalf(node.options.created));
  text += " changehi=" + hex32(hiHalf(node.options.changed)) + " changelo=" + hex32(loHalf(node.options.changed));
  if (node.options.readOnly) text += " readonly";
  if (node.options.hidden) text += " hidden";
  if (node.directory) text += " directory";
  return text;
}

// Resolves a name= argument; the reply is set when the path cannot be used.
struct Target {
  std::optional<ConsolePath> path;
  State::Found found;
  std::optional<Reply> error;
};

Target target(CommandContext &ctx, std::string_view key = "name") {
  Target t;
  const auto *text = ctx.args.get(key);
  if (!text) {
    t.error = status(423);
    return t;
  }
  t.path = splitPath(*text);
  if (!t.path) {
    t.error = status(412);
    return t;
  }
  t.found = ctx.state.locate(*t.path);
  if (!t.found.drive) t.error = status(402);
  return t;
}

Reply dirlist(CommandContext &ctx) {
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (!t.found.node) return status(402);
  if (!t.found.node->directory) return status(414);
  std::vector<std::string> body;
  for (const auto &child : t.found.node->children) body.push_back(entryLine(*child, true));
  return multiline(body);
}

Reply attributes(CommandContext &ctx) {
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (!t.found.node) return status(402);
  const std::string fields = entryLine(*t.found.node, false);
  if (ctx.state.options.attributesAsSingleLine) return line("200- " + fields);
  return multiline({fields});
}

Reply makeDirectory(CommandContext &ctx) {
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (t.found.node) return status(410);
  if (!t.found.parent) return status(413);
  if (t.found.drive->drive.readOnly) return status(414);
  if (!validName(t.path->parts.back(), ctx.state.options.maxNameLength)) return status(412);
  auto child = std::make_unique<Node>();
  child->name = t.path->parts.back();
  child->directory = true;
  t.found.parent->children.push_back(std::move(child));
  return status(200, "OK");
}

Reply removeEntry(CommandContext &ctx) {
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (!t.found.node) return status(402);
  if (!t.found.parent || t.found.drive->drive.readOnly) return status(414);
  const bool dir = ctx.args.flag("dir");
  if (t.found.node->directory != dir) return status(414);
  if (dir && !t.found.node->children.empty()) return status(411);
  t.found.parent->remove(t.found.node);
  return status(200, "OK");
}

bool contains(const Node &folder, const Node *node) {
  if (&folder == node) return true;
  for (const auto &c : folder.children) {
    if (contains(*c, node)) return true;
  }
  return false;
}

Reply renameEntry(CommandContext &ctx) {
  auto from = target(ctx, "name");
  if (from.error) return *from.error;
  const auto *newText = ctx.args.get("newname");
  if (!newText) return status(423);
  const auto toPath = splitPath(*newText);
  if (!toPath || toPath->parts.empty()) return status(412);
  if (!sameName(toPath->drive, from.path->drive)) return status(409);
  auto to = ctx.state.locate(*toPath);
  if (from.found.protectedPath || to.protectedPath) return status(414, "access denied");
  if (!from.found.node) return status(402);
  if (!from.found.parent || from.found.drive->drive.readOnly) return status(414);
  if (!to.parent) return status(402);
  if (to.node && to.node != from.found.node) return status(410);
  if (!validName(toPath->parts.back(), ctx.state.options.maxNameLength)) return status(412);
  if (from.found.node->directory && contains(*from.found.node, to.parent)) return status(400);

  std::unique_ptr<Node> moving;
  auto &siblings = from.found.parent->children;
  for (auto it = siblings.begin(); it != siblings.end(); ++it) {
    if (it->get() == from.found.node) {
      moving = std::move(*it);
      siblings.erase(it);
      break;
    }
  }
  moving->name = toPath->parts.back();
  to.parent->children.push_back(std::move(moving));
  return status(200, "OK");
}

Reply getfile(CommandContext &ctx, const std::optional<XbdmFault> &fault) {
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (!t.found.node) return status(402);
  if (t.found.node->directory) return status(414);
  const Node &node = *t.found.node;
  if (!node.stored && !node.isVirtual) return status(414);

  uint64_t size = node.length();
  if (size > 0xFFFFFFFFull) {
    if (!ctx.state.options.truncateHugeGetfile) return status(414);
    size &= 0xFFFFFFFFull;
  }
  uint64_t claim = size;
  if (fault && fault->lengthClaim) claim = *fault->lengthClaim;

  Reply r;
  append(r.head, "203- binary response follows\r\n");
  for (int i = 0; i < 4; ++i) r.head.push_back(le(claim, i));
  r.bodyLength = size;
  r.stallAtEnd = claim > size;
  if (node.isVirtual) {
    r.body = [seed = node.seed](uint64_t offset, std::span<uint8_t> out) { virtualFileBytes(seed, offset, out); };
  } else {
    r.body = [data = node.data](uint64_t offset, std::span<uint8_t> out) {
      std::copy_n(data->begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
    };
  }
  return r;
}

Reply driveFreeSpace(CommandContext &ctx) {
  const auto *text = ctx.args.get("name");
  if (!text) return status(423);
  const auto path = splitPath(*text);
  if (!path || !path->parts.empty()) return status(412);
  DriveState *drive = ctx.state.findDrive(path->drive);
  if (!drive || !drive->drive.mounted) return status(402);
  if (drive->drive.refuseFreeSpace) return status(414);
  const auto &d = drive->drive;
  const std::string fields = "freetocallerhi=" + hex32(hiHalf(d.freeBytes)) + " freetocallerlo=" + hex32(loHalf(d.freeBytes)) +
                             " totalbyteshi=" + hex32(hiHalf(d.totalBytes)) + " totalbyteslo=" + hex32(loHalf(d.totalBytes)) +
                             " totalfreebyteshi=" + hex32(hiHalf(d.freeBytes)) +
                             " totalfreebyteslo=" + hex32(loHalf(d.freeBytes));
  if (ctx.state.options.freeSpaceAsSingleLine) return line("200- " + fields);
  return multiline({fields});
}

Reply getmem(CommandContext &ctx) {
  const auto addr = number32(ctx.args, "addr");
  const auto length = number32(ctx.args, "length");
  if (!addr || !length || *length == 0 || *length > ctx.state.options.maxGetmemLength) return status(423);
  if (uint64_t{*addr} + *length > 0x100000000ull) return status(423);
  const char *digits = ctx.state.options.lowercaseHex ? "0123456789abcdef" : "0123456789ABCDEF";
  const size_t perLine = std::max<size_t>(1, ctx.state.options.getmemBytesPerLine);
  std::vector<std::string> body;
  std::string current;
  for (uint32_t i = 0; i < *length; ++i) {
    const auto b = ctx.state.readByte(*addr + i);
    if (b) {
      current += digits[*b >> 4];
      current += digits[*b & 0xF];
    } else {
      current += "??";
    }
    if (current.size() == perLine * 2) {
      body.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty()) body.push_back(std::move(current));
  return multiline(body);
}

Reply getmemex(CommandContext &ctx, const std::optional<XbdmFault> &fault) {
  const auto addr = number32(ctx.args, "addr");
  const auto length = number32(ctx.args, "length");
  if (!addr || !length || *length == 0 || *length > ctx.state.options.maxGetmemexLength) return status(423);
  if (uint64_t{*addr} + *length > 0x100000000ull) return status(423);
  Reply r;
  append(r.head, "203- binary response follows\r\n");
  if (fault && fault->memexHeader) {
    const uint16_t header = *fault->memexHeader;
    r.head.push_back(static_cast<uint8_t>(header));
    r.head.push_back(static_cast<uint8_t>(header >> 8));
    for (uint32_t i = 0; i < (header & 0x7FFFu); ++i) r.head.push_back(ctx.state.readByte(*addr + i).value_or(0));
    return r;
  }
  const size_t block = std::clamp<size_t>(ctx.state.options.getmemexBlockSize, 1, 0x7FFF);
  uint32_t done = 0;
  while (done < *length) {
    Bytes data;
    bool unreadable = false;
    while (data.size() < block && done + data.size() < *length) {
      const auto b = ctx.state.readByte(*addr + done + static_cast<uint32_t>(data.size()));
      if (!b) {
        unreadable = true;
        break;
      }
      data.push_back(*b);
    }
    done += static_cast<uint32_t>(data.size());
    const bool last = unreadable || done == *length;
    uint16_t header = static_cast<uint16_t>(data.size());
    if (unreadable || (last && ctx.state.options.getmemexMarkLastBlock)) header = static_cast<uint16_t>(header | 0x8000u);
    r.head.push_back(static_cast<uint8_t>(header));
    r.head.push_back(static_cast<uint8_t>(header >> 8));
    append(r.head, data);
    if (unreadable) break;
  }
  return r;
}

Reply setmem(CommandContext &ctx) {
  const auto addr = number32(ctx.args, "addr");
  const auto *data = ctx.args.get("data");
  if (!addr || !data || data->empty() || data->size() % 2 != 0) return status(423);
  Bytes bytes;
  for (size_t i = 0; i < data->size(); i += 2) {
    const auto value = parseNumber("0x" + data->substr(i, 2));
    if (!value) return status(423);
    bytes.push_back(static_cast<uint8_t>(*value));
  }
  if (uint64_t{*addr} + bytes.size() > 0x100000000ull) return status(423);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (!ctx.state.readByte(*addr + static_cast<uint32_t>(i))) return status(404);
  }
  for (size_t i = 0; i < bytes.size(); ++i) {
    Region *r = ctx.state.regionAt(*addr + static_cast<uint32_t>(i));
    r->data[*addr + i - r->info.base] = bytes[i];
  }
  return status(200, format("set %zu bytes", bytes.size()));
}

Reply screenshot(CommandContext &ctx, const std::optional<XbdmFault> &fault) {
  const auto &s = ctx.state.shot;
  const uint64_t claim = fault && fault->lengthClaim ? *fault->lengthClaim : s.framebuffer.size();
  Reply r;
  append(r.head, "203- binary response follows\r\n");
  append(r.head, "pitch=" + hex32(s.pitch) + " width=" + hex32(s.width) + " height=" + hex32(s.height) +
                     " format=" + hex32(s.format) + " offsetx=" + format("0x%x", s.offsetX) +
                     " offsety=" + format("0x%x", s.offsetY) + ", framebuffersize=" + format("0x%llx", static_cast<unsigned long long>(claim)) +
                     "\r\n");
  r.bodyLength = s.framebuffer.size();
  r.stallAtEnd = claim > s.framebuffer.size();
  r.body = [data = std::make_shared<Bytes>(s.framebuffer)](uint64_t offset, std::span<uint8_t> out) {
    std::copy_n(data->begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
  };
  return r;
}

Reply xbeinfo(CommandContext &ctx) {
  if (ctx.args.flag("running")) {
    if (ctx.state.info.runningTitle.empty()) return status(402);
    return multiline({"timestamp=0x00000000 checksum=0x00000000", "name=\"" + ctx.state.info.runningTitle + "\""});
  }
  auto t = target(ctx);
  if (t.error) return *t.error;
  if (t.found.protectedPath) return status(414, "access denied");
  if (!t.found.node || t.found.node->directory) return status(402);
  return multiline({"timestamp=0x4b1d2a3c checksum=0x0012ab34", "name=\"" + *ctx.args.get("name") + "\""});
}

XbdmThread *findThread(State &state, const Args &args) {
  const auto id = number32(args, "thread");
  if (!id) return nullptr;
  for (auto &t : state.threads) {
    if (t.id == *id) return &t;
  }
  return nullptr;
}

std::optional<Reply> simpleCommand(CommandContext &ctx, const std::optional<XbdmFault> &fault) {
  State &st = ctx.state;
  const auto &name = ctx.args.name;
  auto &info = st.info;
  if (name == "bye") return line("200- bye", Reply::After::Close);
  if (name == "dbgname") {
    if (const auto *n = ctx.args.get("name")) {
      info.debugName = *n;
      return status(200, "OK");
    }
    return status(200, info.debugName);
  }
  if (name == "consoletype") return status(200, info.consoleType);
  if (name == "getconsoleid") return status(200, "consoleid=" + info.consoleId);
  if (name == "xbeinfo") return xbeinfo(ctx);
  if (name == "getexecstate") return status(200, info.execState);
  if (name == "altaddr") return status(200, "addr=" + hex32(info.titleAddress));
  if (name == "getpid") return status(200, "pid=" + hex32(info.processId));
  if (name == "drivelist") {
    std::vector<std::string> body;
    for (const auto &d : st.drives) {
      if (d->drive.mounted) body.push_back("drivename=\"" + d->drive.name + "\"");
    }
    return multiline(body);
  }
  if (name == "drivefreespace") return driveFreeSpace(ctx);
  if (name == "dirlist") return dirlist(ctx);
  if (name == "getfileattributes") return attributes(ctx);
  if (name == "getfile") return getfile(ctx, fault);
  if (name == "mkdir") return makeDirectory(ctx);
  if (name == "delete") return removeEntry(ctx);
  if (name == "rename") return renameEntry(ctx);
  if (name == "setsystime") {
    const auto hi = number32(ctx.args, "clockhi");
    const auto lo = number32(ctx.args, "clocklo");
    if (!hi || !lo) return status(423);
    info.systemTime = (uint64_t{*hi} << 32) | *lo;
    st.events.push_back(format("setsystime clock=0x%016llx", static_cast<unsigned long long>(info.systemTime)));
    return status(200, "OK");
  }
  if (name == "screenshot") return screenshot(ctx, fault);
  if (name == "magicboot") {
    const auto *title = ctx.args.get("title");
    if (title) {
      const auto *dir = ctx.args.get("directory");
      st.events.push_back("magicboot title=" + *title + " directory=" + (dir ? *dir : std::string()));
      return line("200- OK", Reply::After::Reboot);
    }
    if (ctx.args.flag("cold")) {
      st.events.push_back("magicboot cold");
      if (st.options.coldRebootAnswersFirst) return line("200- OK", Reply::After::Reboot);
      Reply r;
      r.after = Reply::After::Reboot;
      return r;
    }
    st.events.push_back("magicboot");
    return line("200- OK", Reply::After::Reboot);
  }
  if (name == "shutdown") {
    st.events.push_back("shutdown");
    Reply r;
    r.after = Reply::After::Reboot;
    return r;
  }
  if (name == "dvdeject") {
    st.events.push_back("dvdeject");
    return status(200, "OK");
  }
  if (name == "stop") {
    if (info.execState == "stop") return status(426);
    info.execState = "stop";
    st.events.push_back("stop");
    return status(200, "OK");
  }
  if (name == "go") {
    if (info.execState != "stop") return status(408);
    info.execState = "start";
    st.events.push_back("go");
    return status(200, "OK");
  }
  if (name == "suspend" || name == "resume") {
    XbdmThread *t = findThread(st, ctx.args);
    if (!t) return status(405);
    if (name == "suspend") ++t->suspendCount;
    else if (t->suspendCount > 0) --t->suspendCount;
    st.events.push_back(name + " thread=" + hex32(t->id));
    return status(200, "OK");
  }
  if (name == "getmem") return getmem(ctx);
  if (name == "getmemex") return getmemex(ctx, fault);
  if (name == "setmem") return setmem(ctx);
  if (name == "walkmem") {
    std::vector<const Region *> sorted;
    for (const auto &r : st.regions) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [](const Region *a, const Region *b) { return a->info.base < b->info.base; });
    std::vector<std::string> body;
    for (const auto *r : sorted) {
      body.push_back("base=" + hex32(r->info.base) + " size=" + hex32(r->info.size) + " protect=" + hex32(r->info.protect) +
                     " phys=" + hex32(r->info.phys));
    }
    return multiline(body);
  }
  if (name == "modules") {
    std::vector<std::string> body;
    for (const auto &m : st.modules) {
      body.push_back("name=\"" + m.name + "\" base=" + hex32(m.base) + " size=" + hex32(m.size) + " check=" + hex32(m.check) +
                     " timestamp=" + hex32(m.timestamp) + " pdata=0x00000000 psize=0x00000000 thread=0x00000000 osize=" +
                     hex32(m.originalSize));
    }
    return multiline(body);
  }
  if (name == "modsections") {
    const auto *module = ctx.args.get("name");
    if (!module) return status(423);
    for (const auto &m : st.modules) {
      if (!sameName(m.name, *module)) continue;
      std::vector<std::string> body;
      for (const auto &s : m.sections) {
        body.push_back("name=\"" + s.name + "\" base=" + hex32(s.base) + " size=" + hex32(s.size) +
                       format(" index=%u flags=%u", s.index, s.flags));
      }
      return multiline(body);
    }
    return status(402);
  }
  if (name == "threads") {
    std::vector<std::string> body;
    for (const auto &t : st.threads) body.push_back(std::to_string(t.id));
    return multiline(body);
  }
  if (name == "threadinfo") {
    XbdmThread *t = findThread(st, ctx.args);
    if (!t) return status(405);
    return multiline({format("suspend=0x%x priority=0x%x tlsbase=0x%08x base=0x%08x limit=0x%08x slack=0x0 nameaddr=0x0 "
                             "namelen=0x0 proc=0x1 lasterr=0x0",
                             t->suspendCount, t->priority, t->tlsBase, t->stackBase, t->stackLimit)});
  }
  if (name == "break") return status(200, "OK");
  if (name == "debugger") {
    std::string rest;
    for (const auto &f : ctx.args.flags) rest += (rest.empty() ? "" : " ") + f;
    for (const auto &[k, v] : ctx.args.values) rest += (rest.empty() ? "" : " ") + k + "=" + v;
    st.events.push_back("debugger " + rest);
    return status(200, "OK");
  }
  if (name == "help") {
    return multiline({"altaddr", "break", "bye", "consoletype", "dbgname", "debugger", "delete", "dirlist", "drivefreespace",
                      "drivelist", "dvdeject", "getconsoleid", "getexecstate", "getfile", "getfileattributes", "getmem",
                      "getmemex", "getpid", "go", "magicboot", "mkdir", "modsections", "modules", "notify", "rename",
                      "resume", "screenshot", "sendfile", "setmem", "setsystime", "shutdown", "stop", "suspend",
                      "threadinfo", "threads", "walkmem", "xbeinfo"});
  }
  return std::nullopt;
}

void recordUpload(State &st, const XbdmUploadRecord &record) {
  std::lock_guard<std::mutex> lock(st.mutex);
  st.uploads.push_back(record);
}

// sendfile (section 3.6): 204, exactly `length` bytes, then one status line. A drop
// keeps what arrived under the target name (section 5.1, item 8).
bool sendfile(State &st, Connection &c, const Args &args) {
  std::unique_lock<std::mutex> lock(st.mutex);
  Sender sender(st, c, st.takeFault("sendfile", c.index, false));
  CommandContext ctx{st, c, args};
  auto refuse = [&](Reply r) {
    lock.unlock();
    return sender.send(r);
  };
  auto t = target(ctx);
  if (t.error) return refuse(*t.error);
  const auto *lengthText = args.get("length");
  const auto length = lengthText ? parseNumber(*lengthText) : std::nullopt;
  if (!length) return refuse(status(423));
  if (t.found.protectedPath) return refuse(status(414, "access denied"));
  if (t.path->parts.empty() || (t.found.node && t.found.node->directory)) return refuse(status(414));
  if (!t.found.parent) return refuse(status(413));
  if (t.found.drive->drive.readOnly) return refuse(status(414));
  if (!validName(t.path->parts.back(), st.options.maxNameLength)) return refuse(status(412));
  if (*length > st.options.maxUploadBytes || *length > t.found.drive->drive.freeBytes) return refuse(status(415));
  if (sender.fault() && sender.fault()->replaceWith) return refuse(Reply{});

  const std::string path = *args.get("name");
  auto created = st.create(path, false, false);
  if (!created) return refuse(status(413));
  const bool store = st.options.storeUploads;
  (*created)->stored = store;
  // Growing the buffer piece by piece would copy it again and again under the lock.
  if (store) (*created)->data->reserve(static_cast<size_t>(*length));
  XbdmUploadRecord record{c.index, path, *length, 0, false};
  lock.unlock();

  if (!sender.put(std::string_view("204- send binary data\r\n"))) {
    recordUpload(st, record);
    return false;
  }

  const auto dropAt = sender.fault() ? sender.fault()->dropUploadAfter : std::nullopt;
  const auto stallAt = sender.fault() ? sender.fault()->stallUploadAfter : std::nullopt;
  std::array<uint8_t, kChunk> buffer{};
  auto keep = [&](std::span<const uint8_t> bytes) {
    std::lock_guard<std::mutex> guard(st.mutex);
    const auto p = splitPath(path);
    Node *node = p ? st.locate(*p).node : nullptr;
    if (!node || node->directory) return;
    if (node->data.use_count() > 1) node->data = std::make_shared<Bytes>(*node->data);
    if (node->stored) node->data->insert(node->data->end(), bytes.begin(), bytes.end());
    else node->digest.add(bytes);
    node->size += bytes.size();
  };
  while (record.received < *length) {
    if (dropAt && record.received >= *dropAt) {
      c.transport->close();
      recordUpload(st, record);
      return false;
    }
    if (stallAt && record.received >= *stallAt) {
      while (c.transport->isOpen() && !st.stopFlag) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      recordUpload(st, record);
      return false;
    }
    uint64_t want = *length - record.received;
    if (dropAt) want = std::min(want, *dropAt - record.received);
    if (stallAt) want = std::min(want, *stallAt - record.received);
    size_t n = 0;
    if (!c.input.empty()) {
      n = static_cast<size_t>(std::min<uint64_t>(want, c.input.size()));
      keep(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(c.input.data()), n));
      c.input.erase(0, n);
    } else {
      auto got = c.transport->readSome(std::span<uint8_t>(buffer).first(static_cast<size_t>(std::min<uint64_t>(want, buffer.size()))));
      if (!got || *got == 0) {
        recordUpload(st, record);
        return false;
      }
      n = *got;
      keep(std::span<const uint8_t>(buffer).first(n));
    }
    record.received += n;
  }
  record.completed = true;
  recordUpload(st, record);
  const std::string answer = sender.fault() && sender.fault()->uploadReply ? *sender.fault()->uploadReply : "200- OK";
  if (!sender.put(answer + "\r\n")) return false;
  return sender.finish();
}

void dedicate(State &st, Connection &c) {
  std::vector<std::string> before;
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.events.push_back("notify");
    before = st.notificationsBefore;
  }
  std::string text;
  for (const auto &l : before) text += l + "\r\n";
  text += "205- now a notification channel\r\n";
  {
    std::lock_guard<std::mutex> lock(c.writeMutex);
    if (!c.transport->writeAll(bytesOf(text))) return;
  }
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    c.dedicated = true;
  }
  // A dedicated connection takes no commands (section 1.8); lines are recorded only.
  while (auto received = readLine(st, c)) {
    std::lock_guard<std::mutex> lock(st.mutex);
    st.commands.push_back({c.index, received->text, lower(parseCommand(received->text).name), received->pipelined,
                           received->overLong});
  }
}

void serveConnection(State &st, Connection &c, bool refuse) {
  auto end = [&] {
    c.transport->close();
    std::lock_guard<std::mutex> lock(st.mutex);
    if (c.counted) --st.active;
    c.busy = false;
  };
  if (refuse) {
    (void)c.transport->writeAll(bytesOf("401- max number of connections exceeded\r\n"));
    end();
    return;
  }

  std::optional<XbdmFault> greetingFault;
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    greetingFault = st.takeFault("", c.index, true);
  }
  {
    Sender sender(st, c, std::move(greetingFault));
    Reply greeting = line("201- connected");
    if (!sender.send(greeting)) {
      end();
      return;
    }
  }

  while (true) {
    auto received = readLine(st, c);
    if (!received) break;
    const Args args = parseCommand(received->text);
    bool closeNow = false;
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      st.commands.push_back({c.index, received->text, args.name, received->pipelined, received->overLong});
      c.busy = true;
      closeNow = received->pipelined && st.options.closeOnPipelining;
    }
    if (closeNow) break;

    if (!received->overLong && args.name == "sendfile") {
      if (!sendfile(st, c, args)) break;
      std::lock_guard<std::mutex> lock(st.mutex);
      c.busy = false;
      continue;
    }
    if (!received->overLong && args.name == "notify") {
      dedicate(st, c);
      break;
    }

    std::unique_lock<std::mutex> lock(st.mutex);
    auto fault = st.takeFault(args.name, c.index, false);
    CommandContext ctx{st, c, args};
    Reply reply;
    if (received->overLong) reply = status(406);
    else if (args.name.empty()) reply = status(407);
    else if (fault && fault->skipCommand && fault->replaceWith) reply = Reply{};
    else if (auto r = simpleCommand(ctx, fault)) reply = std::move(*r);
    else reply = status(407);
    // A reboot drops the connections open when it was asked for, not ones made
    // after its answer.
    std::vector<std::shared_ptr<Connection>> going;
    if (reply.after == Reply::After::Reboot && st.options.rebootDropsAllConnections) {
      for (const auto &other : st.connections) {
        if (!other->finished) going.push_back(other);
      }
    }
    lock.unlock();

    Sender sender(st, c, std::move(fault));
    if (!sender.send(reply)) break;
    if (reply.after == Reply::After::Close) break;
    if (reply.after == Reply::After::Reboot) {
      for (const auto &other : going) other->transport->close();
      break;
    }
    lock.lock();
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
  const bool refuse = st->active >= st->options.connectionLimit;
  if (refuse) {
    ++st->refused;
  } else {
    c->index = st->accepted++;
    c->counted = true;
    ++st->active;
  }
  st->connections.push_back(c);
  c->thread = std::thread([st, c, refuse] {
    serveConnection(*st, *c, refuse);
    c->finished = true;
  });
}

// --- In-memory UDP for discovery ---

class MockDatagramSocket final : public net::IDatagramSocket {
public:
  MockDatagramSocket(std::function<std::optional<Bytes>(std::span<const uint8_t>)> answer, std::string address)
      : answer_(std::move(answer)), address_(std::move(address)) {}

  Result<void> bind(uint16_t, bool) override {
    bound_ = true;
    return {};
  }
  Result<void> bindWith(const net::DatagramBindOptions &) override {
    bound_ = true;
    return {};
  }
  Result<size_t> sendTo(std::span<const uint8_t> data, std::string_view address, uint16_t port) override {
    if (!bound_) return fail(ErrorCode::NotConnected, "not bound");
    if (port == kNamePort && (address == "255.255.255.255" || address == address_)) {
      if (auto reply = answer_(data)) {
        std::lock_guard<std::mutex> lock(mutex_);
        net::Datagram d;
        d.data = std::move(*reply);
        d.senderAddress = address_;
        d.senderPort = kNamePort;
        queue_.push_back(std::move(d));
        changed_.notify_all();
      }
    }
    return data.size();
  }
  Result<std::optional<net::Datagram>> receive(std::chrono::milliseconds timeout) override {
    if (!bound_) return fail(ErrorCode::NotConnected, "not bound");
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, timeout, [&] { return !queue_.empty(); });
    if (queue_.empty()) return std::optional<net::Datagram>{};
    auto d = std::move(queue_.front());
    queue_.pop_front();
    return std::optional<net::Datagram>(std::move(d));
  }
  void close() noexcept override { bound_ = false; }

private:
  std::function<std::optional<Bytes>(std::span<const uint8_t>)> answer_;
  std::string address_;
  bool bound_ = false;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<net::Datagram> queue_;
};

void addDefaultConsole(XbdmMockServer &server) {
  const uint64_t t0 = filetimeFromUnix(1700000000);
  const uint64_t t1 = filetimeFromUnix(1710000000);
  (void)server.addDrive({"HDD", 250000000000ull, 100000000000ull});
  (void)server.addDrive({"DEVKIT", 8ull << 30, 4ull << 30});
  XbdmDrive flash{"FLASH", 16ull << 20, 4ull << 20};
  flash.readOnly = true;
  (void)server.addDrive(flash);
  (void)server.addDrive({"USB0", 16ull << 30, 15ull << 30});
  XbdmDrive usb1{"USB1", 8ull << 30, 8ull << 30};
  usb1.mounted = false;
  (void)server.addDrive(usb1);

  XbdmEntryOptions times;
  times.created = t0;
  times.changed = t1;
  (void)server.addDirectory("HDD:\\Content", times);
  (void)server.addDirectory("HDD:\\Content\\0000000000000000", times);
  (void)server.addFile("HDD:\\default.xex", patternBytes(20000, 7), times);
  (void)server.addFile("HDD:\\Games\\Mock\\default.xex", patternBytes(4096, 8), times);
  (void)server.addDirectory("HDD:\\Empty", times);
  XbdmEntryOptions protectedEntry = times;
  protectedEntry.protectedEntry = true;
  (void)server.addDirectory("HDD:\\Protected", protectedEntry);
  (void)server.addFile("HDD:\\Protected\\secret.bin", patternBytes(64, 9), times);
  XbdmEntryOptions readOnly = times;
  readOnly.readOnly = true;
  XbdmEntryOptions hidden = times;
  hidden.hidden = true;
  (void)server.addFile("HDD:\\Attrs\\ro.txt", bytesOf("read only"), readOnly);
  (void)server.addFile("HDD:\\Attrs\\hidden.txt", bytesOf("hidden"), hidden);
  (void)server.addFile("DEVKIT:\\Mock\\default.xex", patternBytes(8192, 10), times);
  (void)server.addFile("FLASH:\\kernel.bin", patternBytes(1024, 11), times);

  (void)server.addMemoryRegion({0x82000000u, 0x10000u, 0x4u, 0x0u});
  (void)server.addMemoryRegion({0x30000000u, 0x2000u, 0x4u, 0x0u});
  (void)server.setMemoryReadable(0x30001000u, 0x1000u, false);

  server.addModule({"xboxkrnl.exe", 0x80040000u, 0x1a0000u, 0x00123456u, 1600000000u, 0x1a0000u,
                    {{".text", 0x80040000u, 0x100000u, 0u, 0x60000020u}, {".data", 0x80140000u, 0xa0000u, 1u, 0xc0000040u}}});
  server.addModule({"default.xex", 0x82000000u, 0x10000u, 0x0u, 1700000000u, 0x10000u,
                    {{".rdata", 0x82000000u, 0x4000u, 0u, 0x40000040u},
                     {".text", 0x82004000u, 0x8000u, 1u, 0x60000020u},
                     {".data", 0x8200c000u, 0x4000u, 2u, 0xc0000040u}}});
  server.addThread({0xF8000004u, 0, 8, 0x7004e000u, 0x70050000u, 0x7004c000u});
  server.addThread({0xF8000008u, 0, 10, 0x7005e000u, 0x70060000u, 0x7005c000u});
  server.addThread({0xFB000010u, 1, 15, 0x7006e000u, 0x70070000u, 0x7006c000u});
}

} // namespace

// --- XbdmMockServer ---

XbdmMockServer::XbdmMockServer(XbdmMockOptions options) : state_(std::make_shared<State>()) {
  state_->options = options;
  state_->shot.framebuffer = patternBytes(state_->shot.pitch * state_->shot.height, 12);
  addDefaultConsole(*this);
}

XbdmMockServer::~XbdmMockServer() { stop(); }

XbdmMockOptions XbdmMockServer::options() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->options;
}

void XbdmMockServer::setOptions(const XbdmMockOptions &options) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->options = options;
}

Result<net::TransportPtr> XbdmMockServer::connect() {
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

std::function<Result<net::TransportPtr>()> XbdmMockServer::connector() {
  return [this] { return connect(); };
}

void XbdmMockServer::serve(net::TransportPtr transport) { startConnection(state_, std::move(transport)); }

Result<uint16_t> XbdmMockServer::listenTcp() {
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
      // A reply leaves in several writes; without this, Nagle and the client's
      // delayed ACK add 40 ms to most commands.
      const int on = 1;
      (void)::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
      auto st = weak.lock();
      if (!st) return;
      startConnection(st, std::make_unique<TcpServerTransport>(SocketHandle(accepted)));
    }
  });
  return state_->tcpPort;
}

uint16_t XbdmMockServer::tcpPort() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->tcpPort;
}

void XbdmMockServer::dropAllConnections() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->closeAllConnections();
}

void XbdmMockServer::stop() {
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true;
    state_->stopFlag = true;
  }
  if (state_->acceptThread.joinable()) state_->acceptThread.join();
  if (state_->udpThread.joinable()) state_->udpThread.join();
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
  if (state_->udp) state_->udp->close();
}

size_t XbdmMockServer::connectionsAccepted() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->accepted;
}

size_t XbdmMockServer::connectionsRefused() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->refused;
}

size_t XbdmMockServer::activeConnections() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->active;
}

namespace {
template <class Pred> bool pollUntil(Pred pred, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
} // namespace

bool XbdmMockServer::waitForActiveConnections(size_t count, std::chrono::milliseconds limit) const {
  return pollUntil([&] { return activeConnections() == count; }, limit);
}

bool XbdmMockServer::waitForCommands(size_t count, std::chrono::milliseconds limit) const {
  return pollUntil(
      [&] {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->commands.size() >= count;
      },
      limit);
}

bool XbdmMockServer::waitUntilIdle(std::chrono::milliseconds limit) const {
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

std::optional<Bytes> XbdmMockServer::answerNameRequest(std::span<const uint8_t> request) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ++state_->nameRequests;
  if (request.empty()) return std::nullopt;
  std::string name = state_->info.debugName.substr(0, 255);
  if (request[0] == 1) {
    if (request.size() < 2 || request[1] + size_t{2} != request.size()) return std::nullopt;
    const std::string asked(request.begin() + 2, request.end());
    if (!sameName(asked, name)) return std::nullopt;
  } else if (request[0] != 3) {
    return std::nullopt;
  }
  Bytes reply{2, static_cast<uint8_t>(name.size())};
  append(reply, name);
  switch (state_->udpMode) {
  case XbdmUdpMode::Answer: break;
  case XbdmUdpMode::Silent: return std::nullopt;
  case XbdmUdpMode::WrongType: reply[0] = 4; break;
  case XbdmUdpMode::LengthBeyondDatagram: reply[1] = static_cast<uint8_t>(std::min<size_t>(255, name.size() + 10)); break;
  case XbdmUdpMode::TrailingBytes: append(reply, std::string_view("\0junk", 5)); break;
  }
  return reply;
}

void XbdmMockServer::setUdpMode(XbdmUdpMode mode) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->udpMode = mode;
}

XbdmUdpMode XbdmMockServer::udpMode() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->udpMode;
}

net::DatagramSocketFactory XbdmMockServer::datagramFactory(std::string address) {
  auto answer = [this](std::span<const uint8_t> request) { return answerNameRequest(request); };
  return [answer, address]() -> std::unique_ptr<net::IDatagramSocket> {
    return std::make_unique<MockDatagramSocket>(answer, address);
  };
}

Result<uint16_t> XbdmMockServer::listenUdp() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->udpPort != 0) return state_->udpPort;
  auto socket = std::make_unique<net::UdpSocket>();
  net::DatagramBindOptions bind;
  bind.address = "127.0.0.1";
  if (auto bound = socket->bindWith(bind); !bound) return updclient::unexpected<updclient::Error>(bound.error());
  state_->udpPort = socket->boundPort();
  state_->udp = std::move(socket);
  state_->udpThread = std::thread([this, raw = state_.get()] {
    while (!raw->stopFlag) {
      auto received = raw->udp->receive(std::chrono::milliseconds(20));
      if (!received || !*received) continue;
      if (auto reply = answerNameRequest((*received)->data)) {
        (void)raw->udp->sendTo(*reply, (*received)->senderAddress, (*received)->senderPort);
      }
    }
  });
  return state_->udpPort;
}

size_t XbdmMockServer::nameRequestsSeen() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->nameRequests;
}

void XbdmMockServer::inject(XbdmFault fault) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->faults.push_back({std::move(fault), 0, 0});
}

void XbdmMockServer::clearFaults() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->faults.clear();
}

size_t XbdmMockServer::pendingFaults() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->faults.size();
}

std::vector<XbdmCommandRecord> XbdmMockServer::commands() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->commands;
}

std::vector<std::string> XbdmMockServer::commandLines() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<std::string> lines;
  for (const auto &c : state_->commands) lines.push_back(c.line);
  return lines;
}

void XbdmMockServer::clearCommands() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->commands.clear();
}

std::vector<XbdmUploadRecord> XbdmMockServer::uploads() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->uploads;
}

std::vector<std::string> XbdmMockServer::events() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->events;
}

void XbdmMockServer::clearEvents() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->events.clear();
}

XbdmConsoleInfo XbdmMockServer::info() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->info;
}

void XbdmMockServer::setInfo(const XbdmConsoleInfo &info) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->info = info;
}

XbdmScreenshot XbdmMockServer::screenshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->shot;
}

void XbdmMockServer::setScreenshot(XbdmScreenshot screenshot) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->shot = std::move(screenshot);
}

void XbdmMockServer::clearConsole() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->drives.clear();
  state_->regions.clear();
  state_->modules.clear();
  state_->threads.clear();
}

Result<void> XbdmMockServer::addDrive(const XbdmDrive &drive) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (drive.name.empty() || drive.name.find_first_of(":\\") != std::string::npos) {
    return fail(ErrorCode::InvalidArgument, "bad drive name");
  }
  if (state_->findDrive(drive.name)) return fail(ErrorCode::InvalidArgument, "drive exists");
  auto d = std::make_unique<DriveState>();
  d->drive = drive;
  d->root.directory = true;
  state_->drives.push_back(std::move(d));
  return {};
}

Result<void> XbdmMockServer::updateDrive(const XbdmDrive &drive) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  DriveState *d = state_->findDrive(drive.name);
  if (!d) return fail(ErrorCode::InvalidArgument, "no such drive");
  d->drive = drive;
  return {};
}

std::optional<XbdmDrive> XbdmMockServer::drive(std::string_view name) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  DriveState *d = state_->findDrive(name);
  if (!d) return std::nullopt;
  return d->drive;
}

Result<void> XbdmMockServer::addDirectory(std::string_view path, XbdmEntryOptions options) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto node = state_->create(path, true, true);
  if (!node) return updclient::unexpected<updclient::Error>(node.error());
  (*node)->options = options;
  return {};
}

Result<void> XbdmMockServer::addFile(std::string_view path, Bytes data, XbdmEntryOptions options) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto node = state_->create(path, false, true);
  if (!node) return updclient::unexpected<updclient::Error>(node.error());
  (*node)->data = std::make_shared<Bytes>(std::move(data));
  (*node)->options = options;
  return {};
}

Result<void> XbdmMockServer::addVirtualFile(std::string_view path, uint64_t size, uint32_t seed,
                                            XbdmEntryOptions options) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto node = state_->create(path, false, true);
  if (!node) return updclient::unexpected<updclient::Error>(node.error());
  (*node)->stored = false;
  (*node)->isVirtual = true;
  (*node)->seed = seed;
  (*node)->size = size;
  (*node)->options = options;
  return {};
}

Result<void> XbdmMockServer::setEntryOptions(std::string_view path, XbdmEntryOptions options) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node) return fail(ErrorCode::InvalidArgument, "no such path");
  node->options = options;
  return {};
}

Result<void> XbdmMockServer::removePath(std::string_view path) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  if (!p) return fail(ErrorCode::InvalidArgument, "bad path");
  auto found = state_->locate(*p);
  if (!found.node || !found.parent) return fail(ErrorCode::InvalidArgument, "no such path");
  found.parent->remove(found.node);
  return {};
}

std::optional<XbdmEntry> XbdmMockServer::entry(std::string_view path) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node) return std::nullopt;
  return XbdmEntry{node->name, node->directory, node->directory ? 0 : node->length(), node->options};
}

std::optional<Bytes> XbdmMockServer::fileData(std::string_view path) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node || node->directory || !node->stored) return std::nullopt;
  return *node->data;
}

std::optional<uint64_t> XbdmMockServer::fileSize(std::string_view path) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node || node->directory) return std::nullopt;
  return node->length();
}

std::optional<uint64_t> XbdmMockServer::fileDigest(std::string_view path) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node || node->directory) return std::nullopt;
  if (node->stored) {
    Fnv1a digest;
    digest.add(*node->data);
    return digest.value;
  }
  if (!node->isVirtual) return node->digest.value;
  Fnv1a digest;
  Bytes chunk;
  for (uint64_t offset = 0; offset < node->size;) {
    chunk.resize(static_cast<size_t>(std::min<uint64_t>(kChunk, node->size - offset)));
    virtualFileBytes(node->seed, offset, chunk);
    digest.add(chunk);
    offset += chunk.size();
  }
  return digest.value;
}

std::optional<std::vector<std::string>> XbdmMockServer::listNames(std::string_view path) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto p = splitPath(path);
  Node *node = p ? state_->locate(*p).node : nullptr;
  if (!node || !node->directory) return std::nullopt;
  std::vector<std::string> names;
  for (const auto &c : node->children) names.push_back(c->name);
  return names;
}

Result<void> XbdmMockServer::addMemoryRegion(const XbdmMemoryRegion &region, Bytes data) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (region.size == 0 || uint64_t{region.base} + region.size > 0x100000000ull) {
    return fail(ErrorCode::InvalidArgument, "bad region");
  }
  for (const auto &r : state_->regions) {
    if (region.base < uint64_t{r.info.base} + r.info.size && r.info.base < uint64_t{region.base} + region.size) {
      return fail(ErrorCode::InvalidArgument, "regions overlap");
    }
  }
  Region r;
  r.info = region;
  r.data = patternBytes(region.size, region.base);
  std::copy_n(data.begin(), std::min<size_t>(data.size(), r.data.size()), r.data.begin());
  r.readable.assign(region.size, true);
  state_->regions.push_back(std::move(r));
  return {};
}

Result<void> XbdmMockServer::setMemoryReadable(uint32_t address, uint32_t length, bool readable) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (uint64_t a = address; a < uint64_t{address} + length; ++a) {
    Region *r = state_->regionAt(static_cast<uint32_t>(a));
    if (!r) return fail(ErrorCode::InvalidArgument, "memory not mapped");
    r->readable[static_cast<size_t>(a - r->info.base)] = readable;
  }
  return {};
}

std::optional<Bytes> XbdmMockServer::memory(uint32_t address, uint32_t length) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  Bytes out;
  for (uint64_t a = address; a < uint64_t{address} + length; ++a) {
    Region *r = state_->regionAt(static_cast<uint32_t>(a));
    if (!r) return std::nullopt;
    out.push_back(r->data[static_cast<size_t>(a - r->info.base)]);
  }
  return out;
}

void XbdmMockServer::addModule(XbdmModule module) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->modules.push_back(std::move(module));
}

void XbdmMockServer::addThread(XbdmThread thread) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->threads.push_back(thread);
}

std::optional<XbdmThread> XbdmMockServer::thread(uint32_t id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const auto &t : state_->threads) {
    if (t.id == id) return t;
  }
  return std::nullopt;
}

void XbdmMockServer::setNotificationsBeforeDedicated(std::vector<std::string> lines) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->notificationsBefore = std::move(lines);
}

size_t XbdmMockServer::notify(std::string_view text) {
  std::vector<std::shared_ptr<Connection>> targets;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto &c : state_->connections) {
      if (c->dedicated && !c->finished) targets.push_back(c);
    }
  }
  size_t sent = 0;
  const Bytes bytes = bytesOf(std::string(text) + "\r\n");
  for (auto &c : targets) {
    std::lock_guard<std::mutex> lock(c->writeMutex);
    if (c->transport->writeAll(bytes)) ++sent;
  }
  return sent;
}

} // namespace ut
