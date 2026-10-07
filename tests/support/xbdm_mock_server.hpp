#pragma once

// An XBDM console for tests: the server side of docs/XBDM_PROTOCOL.md (the
// obligations of section 5.1), written from that document alone so that it checks
// the client rather than mirroring it. It serves an in-memory console (drives and
// files, a sparse memory map, modules, threads, console information) over in-memory
// pipes or a loopback TCP listener, answers the UDP name protocol through fake or
// real datagram sockets, records every command line it receives, and misbehaves on
// request, per connection or per command.
//
// Every public member is thread-safe. Each connection is served on its own thread;
// stop() (or the destructor) closes all of them and joins the threads.

#include <core/error.hpp>
#include <net/datagram.hpp>
#include <net/transport.hpp>

#include "support/test_util.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ut {

// FILETIME: 100 ns intervals since 1601-01-01 (section 4.3).
inline constexpr uint64_t kFiletimePerSecond = 10000000ull;
inline constexpr uint64_t kFiletimeUnixEpoch = 116444736000000000ull;
inline constexpr uint64_t filetimeFromUnix(uint64_t seconds) {
  return kFiletimeUnixEpoch + seconds * kFiletimePerSecond;
}

// The content of a virtual file (XbdmMockServer::addVirtualFile): a function of the
// offset, so files larger than memory can be served and checked.
inline constexpr uint64_t virtualFileWord(uint32_t seed, uint64_t index) {
  uint64_t x = (index + 1) * 0x9E3779B97F4A7C15ull + seed;
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  return x ^ (x >> 29);
}
inline constexpr uint8_t virtualFileByte(uint32_t seed, uint64_t offset) {
  return static_cast<uint8_t>(virtualFileWord(seed, offset / 8) >> (8 * (offset % 8)));
}
inline void virtualFileBytes(uint32_t seed, uint64_t offset, std::span<uint8_t> out) {
  size_t i = 0;
  while (i < out.size() && (offset + i) % 8 != 0) {
    out[i] = virtualFileByte(seed, offset + i);
    ++i;
  }
  for (; i + 8 <= out.size(); i += 8) {
    const uint64_t word = virtualFileWord(seed, (offset + i) / 8);
    for (int b = 0; b < 8; ++b) out[i + static_cast<size_t>(b)] = static_cast<uint8_t>(word >> (8 * b));
  }
  for (; i < out.size(); ++i) out[i] = virtualFileByte(seed, offset + i);
}

// FNV-1a over a byte stream, for uploads the mock counts instead of storing.
struct Fnv1a {
  uint64_t value = 0xcbf29ce484222325ull;
  void add(std::span<const uint8_t> bytes) {
    for (uint8_t b : bytes) value = (value ^ b) * 0x100000001b3ull;
  }
};

struct XbdmDrive {
  // Without colon or backslash: "HDD", "DEVKIT", "FLASH", "USB0".
  std::string name;
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
  // An unmounted drive is left out of drivelist and every path on it is refused.
  bool mounted = true;
  // Reads work; anything that would change the drive is refused with 414.
  bool readOnly = false;
  // drivefreespace fails on this drive (NSx:61-63 suggests some drives do).
  bool refuseFreeSpace = false;
};

struct XbdmEntryOptions {
  uint64_t created = 0;
  uint64_t changed = 0;
  // Extra dirlist flags. No reference has seen them (section 3.3), so they test that
  // the client ignores flags it does not know.
  bool readOnly = false;
  bool hidden = false;
  // Every command on this path, or below it, fails with "414- access denied".
  bool protectedEntry = false;
};

struct XbdmEntry {
  std::string name;
  bool directory = false;
  uint64_t size = 0;
  XbdmEntryOptions options;
};

struct XbdmMemoryRegion {
  uint32_t base = 0;
  uint32_t size = 0;
  uint32_t protect = 0x4;
  uint32_t phys = 0;
};

struct XbdmSection {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
  uint32_t index = 0;
  uint32_t flags = 0;
};

struct XbdmModule {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
  uint32_t check = 0;
  // Unix seconds, as ME reads it (MEf:411).
  uint32_t timestamp = 0;
  uint32_t originalSize = 0;
  std::vector<XbdmSection> sections;
};

struct XbdmThread {
  uint32_t id = 0;
  uint32_t suspendCount = 0;
  uint32_t priority = 8;
  uint32_t tlsBase = 0;
  uint32_t stackBase = 0;
  uint32_t stackLimit = 0;
};

struct XbdmConsoleInfo {
  std::string debugName = "MockDevkit";
  std::string consoleType = "devkit";
  std::string consoleId = "0123456789ab";
  // xbeinfo running; empty answers 402 (nothing is running).
  std::string runningTitle = "\\Device\\Harddisk0\\Partition1\\DEVKIT\\Mock\\default.xex";
  // getexecstate: start, stop, pending, reboot, pending_title, reboot_title.
  std::string execState = "start";
  // altaddr: the title address, network order (0xC0A80102 is 192.168.1.2).
  uint32_t titleAddress = 0xC0A80102;
  uint32_t processId = 0x00000001;
  // Set by setsystime.
  uint64_t systemTime = 0x01d11fb559683c00ull;
};

struct XbdmScreenshot {
  uint32_t pitch = 0x100;
  uint32_t width = 0x40;
  uint32_t height = 0x20;
  uint32_t format = 0x18280186;
  uint32_t offsetX = 0;
  uint32_t offsetY = 0;
  // Sent as is; framebuffersize is its length unless a fault claims otherwise.
  Bytes framebuffer;
};

struct XbdmMockOptions {
  // Simultaneous connections; one more gets "401- max number of connections
  // exceeded" and is closed. The number on a console is unknown (section 1.12).
  size_t connectionLimit = 4;
  // Command lines longer than this (without CR LF) are answered "406- line too
  // long". A mock setting, not a console fact (section 1.3).
  size_t maxLineLength = 512;
  // magicboot cold answers 200 before it closes, instead of closing at once.
  bool coldRebootAnswersFirst = false;
  // A reboot or shutdown closes every connection, not only the one that sent it.
  bool rebootDropsAllConnections = true;
  // drivefreespace answers XT's single 200 line instead of a 202 body.
  bool freeSpaceAsSingleLine = false;
  // getfileattributes answers one 200 line instead of a 202 body.
  bool attributesAsSingleLine = false;
  // getmem hex digits per byte pair: upper case (ME) or lower case (XL).
  bool lowercaseHex = false;
  size_t getmemBytesPerLine = 128;
  uint32_t maxGetmemLength = 0x10000;
  // getmemex block size (at most 0x7FFF) and the largest accepted request.
  size_t getmemexBlockSize = 0x400;
  uint32_t maxGetmemexLength = 0x100000;
  // Set bit 15 on the block that completes a getmemex request. Off, bit 15 appears
  // only on an early end at unreadable memory (ME's reading, MEc:603-622).
  bool getmemexMarkLastBlock = true;
  // sendfile lengths above this are refused before 204, whatever the free space.
  uint64_t maxUploadBytes = 64ull << 20;
  // Off, an upload keeps only its size and an Fnv1a digest (fileDigest), so a test
  // can send more than fits in memory.
  bool storeUploads = true;
  // getfile of a file of 4 GiB or more, whose size the 4-byte length cannot carry
  // (section 3.5, unknown on hardware): refuse with 414, or announce the size modulo
  // 2^32 and send that many bytes, the worst a console could do.
  bool truncateHugeGetfile = true;
  // Longest file or folder name accepted (FATX: 42); 0 for no limit.
  size_t maxNameLength = 42;
  // A command line that arrives while earlier bytes of the same connection are
  // still unanswered is pipelining (section 1.10). It is always recorded; with this
  // set the connection is also closed without an answer.
  bool closeOnPipelining = false;
  // Capacity of each direction of an in-memory connection.
  size_t pipeCapacity = 64 * 1024;
};

// One received command line, in arrival order.
struct XbdmCommandRecord {
  // Order of acceptance, from 0, counting only connections that were not refused.
  size_t connection = 0;
  // Without the line terminator; cut at maxLineLength when overLong.
  std::string line;
  // The first word in lower case; empty for an empty line.
  std::string name;
  // More bytes were already waiting on the connection when this line was taken.
  bool pipelined = false;
  bool overLong = false;
};

struct XbdmUploadRecord {
  size_t connection = 0;
  std::string path;
  uint64_t announced = 0;
  uint64_t received = 0;
  bool completed = false;
};

// A misbehaviour, armed with XbdmMockServer::inject(). The target fields choose
// which replies it applies to; the effect fields can be combined. The command still
// takes effect on the console state unless it is a sendfile whose 204 was replaced.
struct XbdmFault {
  // Lower-case command name; empty matches every command.
  std::string command;
  // Applies to the greeting instead of a command reply.
  bool greeting = false;
  // Only on the connection with this acceptance index.
  std::optional<size_t> connection;
  // Matching replies to let through before the first firing.
  size_t skip = 0;
  // Firings before the fault disarms itself; negative fires forever.
  int times = 1;

  // Close the connection after this many bytes of the reply (0 closes at once).
  std::optional<size_t> dropAfter;
  // After this many bytes of the reply, send nothing for stallFor (zero: until the
  // client closes or the server stops), then carry on. Never sleeps blindly: the
  // connection keeps being read, so a close is noticed at once.
  std::optional<size_t> stallAfter;
  std::chrono::milliseconds stallFor{0};
  // Send these bytes instead of the reply.
  std::optional<Bytes> replaceWith;
  // Bytes sent after the reply: an over-long binary frame, or a second status line.
  Bytes trailer;
  // After the reply, send bytes that never form a line until the connection closes.
  bool endless = false;
  // Write the reply one byte per write.
  bool trickle = false;
  // Deterministically mangle the reply: flipped, inserted and dropped bytes, cut
  // lines, wrong status codes, early closes. The same seed gives the same damage.
  std::optional<uint64_t> hostileSeed;
  // getfile: the 4-byte length announces this instead of the size; screenshot: the
  // framebuffersize field. When the claim exceeds the data, the data is sent and the
  // connection then stalls, as a console would; when it is shorter, all the data is
  // still sent (an over-long frame).
  std::optional<uint64_t> lengthClaim;
  // getmemex: the first block header carries this raw value and that many bytes
  // follow (bits 0-14), then the reply ends.
  std::optional<uint16_t> memexHeader;
  // sendfile: close after receiving this many bytes of the upload data.
  std::optional<uint64_t> dropUploadAfter;
  // sendfile: stop reading after this many bytes of the upload data, so that the
  // client's writes block, until the mock closes the connection (drop or stop).
  std::optional<uint64_t> stallUploadAfter;
  // sendfile: the line after the upload data, instead of "200- OK" (no CR LF needed).
  std::optional<std::string> uploadReply;

  static XbdmFault dropAfterBytes(size_t bytes);
  static XbdmFault stall(size_t afterBytes, std::chrono::milliseconds duration = std::chrono::milliseconds(0));
  // Never answer anything after the greeting, on every matching command.
  static XbdmFault silence();
  static XbdmFault reply(std::string_view raw);
  static XbdmFault reply(Bytes raw);
  // A status line: statusLine("299- odd") sends "299- odd\r\n".
  static XbdmFault statusLine(std::string_view line);
  // A line of `length` characters (default: one past the client's 64 KiB cap).
  static XbdmFault oversizedLine(size_t length = 64 * 1024 + 1);
  static XbdmFault endlessLine();
  static XbdmFault trickleBytes();
  static XbdmFault hostile(uint64_t seed);
  static XbdmFault claimLength(uint64_t length);
  static XbdmFault extraBytes(Bytes bytes);
  static XbdmFault memexBlockHeader(uint16_t header);
  static XbdmFault dropUploadAfterBytes(uint64_t bytes);
  static XbdmFault stallUploadAfterBytes(uint64_t bytes);
  static XbdmFault uploadAnswer(std::string line);

  XbdmFault &on(std::string_view commandName);
  XbdmFault &onGreeting();
  XbdmFault &onConnection(size_t index);
  XbdmFault &after(size_t matches);
  XbdmFault &repeat(int count);
  XbdmFault &always();
};

// How the UDP name responder answers (section 2.1).
enum class XbdmUdpMode {
  Answer,
  // Blocked UDP: nothing is answered.
  Silent,
  // Replies whose first byte is not 2.
  WrongType,
  // Replies whose length byte claims more than the datagram carries.
  LengthBeyondDatagram,
  // Replies with bytes after the name.
  TrailingBytes,
};

class XbdmMockServer {
public:
  struct State;

  // Starts with the default console described in the .cpp: drives HDD, DEVKIT,
  // FLASH (read-only), USB0 and an unmounted USB1, a few folders and files, memory
  // at 0x82000000 (readable) and 0x30000000 (one unreadable page), two modules and
  // three threads.
  explicit XbdmMockServer(XbdmMockOptions options = {});
  ~XbdmMockServer();
  XbdmMockServer(const XbdmMockServer &) = delete;
  XbdmMockServer &operator=(const XbdmMockServer &) = delete;

  XbdmMockOptions options() const;
  void setOptions(const XbdmMockOptions &options);

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

  size_t connectionsAccepted() const;
  size_t connectionsRefused() const;
  size_t activeConnections() const;
  // Poll until the condition holds or limit passes. For tests only, never used by
  // the protocol path.
  bool waitForActiveConnections(size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) const;
  bool waitForCommands(size_t count, std::chrono::milliseconds limit = std::chrono::seconds(5)) const;
  bool waitUntilIdle(std::chrono::milliseconds limit = std::chrono::seconds(5)) const;

  // --- UDP name answering (section 2) ---

  // The reply to one request datagram, or nothing (honours the UDP mode).
  std::optional<Bytes> answerNameRequest(std::span<const uint8_t> request) const;
  void setUdpMode(XbdmUdpMode mode);
  XbdmUdpMode udpMode() const;
  // In-memory datagram sockets for the client's discovery. A sendTo() to port 730 at
  // a broadcast address, or at `address`, is answered with a reply from `address`
  // that the same socket's receive() returns.
  updclient::net::DatagramSocketFactory datagramFactory(std::string address = "127.0.0.1");
  // A real UDP responder through net::UdpSocket on 127.0.0.1; returns its port.
  updclient::Result<uint16_t> listenUdp();
  size_t nameRequestsSeen() const;

  // --- Fault injection ---

  void inject(XbdmFault fault);
  void clearFaults();
  // Armed faults that have not fired their last time yet.
  size_t pendingFaults() const;

  // --- Records ---

  std::vector<XbdmCommandRecord> commands() const;
  std::vector<std::string> commandLines() const;
  void clearCommands();
  std::vector<XbdmUploadRecord> uploads() const;
  // Actions the mock records instead of performing, in order: "magicboot",
  // "magicboot cold", "magicboot title=<path> directory=<dir>", "shutdown",
  // "dvdeject", "stop", "go", "suspend thread=0x..", "resume thread=0x..",
  // "setsystime clock=0x<16 digits>", "debugger <arguments>", "notify".
  std::vector<std::string> events() const;
  void clearEvents();

  // --- Console state ---

  XbdmConsoleInfo info() const;
  void setInfo(const XbdmConsoleInfo &info);
  XbdmScreenshot screenshot() const;
  void setScreenshot(XbdmScreenshot screenshot);

  // Removes every drive, region, module and thread, for a test that builds its own.
  void clearConsole();
  updclient::Result<void> addDrive(const XbdmDrive &drive);
  updclient::Result<void> updateDrive(const XbdmDrive &drive);
  std::optional<XbdmDrive> drive(std::string_view name) const;
  // Paths are console paths ("HDD:\a\b"); missing parent folders are created.
  updclient::Result<void> addDirectory(std::string_view path, XbdmEntryOptions options = {});
  updclient::Result<void> addFile(std::string_view path, Bytes data, XbdmEntryOptions options = {});
  // A file of any size whose byte at offset o is virtualFileByte(seed, o).
  updclient::Result<void> addVirtualFile(std::string_view path, uint64_t size, uint32_t seed,
                                         XbdmEntryOptions options = {});
  updclient::Result<void> setEntryOptions(std::string_view path, XbdmEntryOptions options);
  updclient::Result<void> removePath(std::string_view path);
  std::optional<XbdmEntry> entry(std::string_view path) const;
  // nullopt for folders, virtual files and uploads that were not stored.
  std::optional<Bytes> fileData(std::string_view path) const;
  std::optional<uint64_t> fileSize(std::string_view path) const;
  // Fnv1a of the content; for a virtual file it is computed, so keep those small.
  std::optional<uint64_t> fileDigest(std::string_view path) const;
  // Names directly inside a folder, in listing order; nullopt if it is not a folder.
  std::optional<std::vector<std::string>> listNames(std::string_view path) const;

  // `data` fills the region from its base; the rest is a pattern.
  updclient::Result<void> addMemoryRegion(const XbdmMemoryRegion &region, Bytes data = {});
  // Marks bytes of mapped memory unreadable (getmem "??", getmemex ends early) and
  // unwritable (setmem 404), or readable again.
  updclient::Result<void> setMemoryReadable(uint32_t address, uint32_t length, bool readable);
  // Current bytes; nullopt unless the whole range is mapped.
  std::optional<Bytes> memory(uint32_t address, uint32_t length) const;
  void addModule(XbdmModule module);
  void addThread(XbdmThread thread);
  std::optional<XbdmThread> thread(uint32_t id) const;

  // --- Notifications (section 3.18) ---

  // Lines sent on a connection that sent notify, before its 205 (MEc:1168-1188).
  void setNotificationsBeforeDedicated(std::vector<std::string> lines);
  // Sends a line to every notification connection; returns how many got it.
  size_t notify(std::string_view line);

private:
  std::shared_ptr<State> state_;
};

} // namespace ut
