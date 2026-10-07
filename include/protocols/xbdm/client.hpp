#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <core/path.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>
#include <net/transport_registry.hpp>
#include <protocols/xbdm/path.hpp>
#include <protocols/xbdm/protocol.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::xbdm {

namespace detail {
struct Session;
} // namespace detail

// What a trace hook is told, on the thread doing the I/O: every command line sent
// and every text line received (greeting, status lines, body lines, the screenshot
// geometry), as text without CR LF; and for binary data in either direction (file
// contents, memory blocks, frame buffers) only how many bytes moved, never the
// bytes themselves.
enum class TraceEvent { Sent, Received, BinarySent, BinaryReceived };
using TraceHook = std::function<void(TraceEvent event, std::string_view text, uint64_t bytes)>;

// Every timeout is idle-based: the longest wait for the next byte (or for room to
// send one), restarted whenever data moves. commandTimeout, and greetingTimeout
// and byeTimeout for their one line, also bound the whole answer, so a console
// that trickles bytes cannot stretch a call. Zero disables one, which lets a call
// wait forever; the defaults never do.
struct ClientOptions {
  // For "201- connected" after the connection is made, as a whole.
  std::chrono::milliseconds greetingTimeout{5000};
  // Inside every answer, and between pieces of a file transfer.
  std::chrono::milliseconds idleTimeout{10000};
  // Answers the console is slow to give: screenshot, the status after sendfile
  // data, reboot, launch and shutdown.
  std::chrono::milliseconds slowIdleTimeout{30000};
  // Upper bound for one whole command, from sending it to its last byte, and for
  // the status after sendfile data. The body of a file transfer is exempt: it may
  // take as long as bytes keep moving.
  std::chrono::milliseconds commandTimeout{60000};
  // For the answer to "bye" when the client closes, as a whole.
  std::chrono::milliseconds byeTimeout{2000};

  // The greeting, the answer to bye and the status after sendfile data are short
  // phrases and limited to 512 bytes (or maxLineBytes, if lower).
  size_t maxLineBytes = kMaxLineBytes;
  // Every line of a 202 body counts as at least 64 bytes, about what an empty line
  // costs once parsed.
  size_t maxBodyBytes = kMaxBodyBytes;
  size_t maxCommandBytes = kMaxCommandBytes;
  uint64_t maxScreenshotBytes = kMaxScreenshotBytes;
  uint32_t maxMemoryReadBytes = kMaxMemoryReadBytes;
  // Largest openWrite size. sendfile beyond 4 GiB - 1 has never been seen working
  // (section 3.6); raising this sends the length as a wider hex number.
  uint64_t maxUploadBytes = kMaxUploadBytes;
  // Largest getfile length accepted from the console.
  uint64_t maxDownloadBytes = 0xFFFFFFFFull;

  // Protocol trace, for diagnosing a console; empty for none.
  TraceHook trace;
};

// For an Error this client raised because the console answered 4xx: the status
// code, which is also in Error::sysError, with the console's line at the end of
// Error::message ("...: console answered 410- already exists"). nullopt for every
// other error, including transport errors whose sysError is an OS error number.
UPDCLIENT_API std::optional<int> consoleStatusCode(const Error &error) noexcept;

// Error codes used for 4xx answers: LimitExceeded for 401 (too many connections),
// 446 (line too long) and 406 (clock not set, or line too long); Unsupported for
// 407 (unknown command); Io for every other 4xx. A 4xx leaves the connection
// usable.

enum class ExecState { Unknown, Start, Stop, Pending, Reboot, PendingTitle, RebootTitle };

struct ExecStatus {
  ExecState state = ExecState::Unknown;
  std::string text; // the word the console sent
};

struct XbeInfo {
  std::string name; // as the console sent it; may be a device path
  std::optional<uint32_t> timestamp;
  std::optional<uint32_t> checksum;
};

// altaddr: the title address of a devkit. 0xC0A80102 is 192.168.1.2.
struct TitleAddress {
  uint32_t raw = 0;
  std::string text;
};

// Fields whose command the console refused (any 4xx) are empty.
struct ConsoleInfo {
  std::optional<std::string> debugName;
  std::optional<std::string> consoleType;
  std::optional<std::string> consoleId;
  std::optional<XbeInfo> runningTitle;
  std::optional<ExecStatus> execState;
  std::optional<TitleAddress> titleAddress;
};

struct DriveSpace {
  uint64_t freeToCaller = 0;
  uint64_t totalBytes = 0;
  uint64_t totalFreeBytes = 0;

  uint64_t usedBytes() const noexcept { return totalBytes > freeToCaller ? totalBytes - freeToCaller : 0; }
};

struct FileAttributes {
  uint64_t size = 0;
  // False when the console sent no sizehi and sizelo; size is then 0.
  bool sizeKnown = false;
  // Raw FILETIMEs; empty when the console did not send them.
  std::optional<uint64_t> createdFileTime;
  std::optional<uint64_t> changedFileTime;
  bool isDirectory = false;
  bool isReadOnly = false;
  bool isHidden = false;

  std::optional<FileTimePoint> created() const noexcept {
    return createdFileTime ? fileTimeToTimePoint(*createdFileTime) : std::nullopt;
  }
  std::optional<FileTimePoint> changed() const noexcept {
    return changedFileTime ? fileTimeToTimePoint(*changedFileTime) : std::nullopt;
  }
};

struct DirEntry : FileAttributes {
  std::string name;
};

struct DirListing {
  std::vector<DirEntry> entries;
  // Lines that were not a usable entry: no name, a number that does not parse, or
  // a name validateName refuses (a path or drive separator, a wildcard, a control
  // character or a byte above 0x7E), which no command could take back. "." and
  // ".." are dropped without counting.
  size_t skipped = 0;
};

// Whether the console said so before going away. Both count as success.
enum class PowerResult { Acknowledged, ConnectionClosed };

// The references disagree on what a warm magicboot does (section 3.14): the
// dashboard for ClementDreptin's XBDM and OpenNeighborhood, a title restart for
// EmDbg and MemoryEngine360.
enum class RebootMode {
  Warm, // magicboot
  Cold  // magicboot cold
};

// The frame buffer as the console sends it: the GPU's tiled layout, format as
// given. Untiling is left to the caller (section 3.13).
struct Screenshot {
  uint32_t pitch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t offsetX = 0;
  uint32_t offsetY = 0;
  std::vector<uint8_t> data;
};

// data has the requested length; unreadable bytes are zero and false in readable.
struct MemoryRead {
  uint32_t address = 0;
  std::vector<uint8_t> data;
  std::vector<bool> readable;

  size_t readableBytes() const noexcept {
    size_t count = 0;
    for (bool b : readable) count += b ? 1 : 0;
    return count;
  }
};

struct MemoryRegion {
  uint32_t base = 0;
  uint32_t size = 0;
  uint32_t protect = 0;
  uint32_t phys = 0;
};

struct Module {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
  std::optional<uint32_t> checksum;
  std::optional<uint32_t> timestamp; // Unix seconds
  std::optional<uint32_t> originalSize;
};

struct ModuleSection {
  std::string name;
  uint32_t base = 0;
  uint32_t size = 0;
  std::optional<uint32_t> index;
  std::optional<uint32_t> flags;
};

// The answer to a command line sent as typed (XbdmClient::rawCommand).
struct RawAnswer {
  StatusLine status;
  // The lines of a 202 answer, without the closing ".".
  std::vector<std::string> body;
};

// (bytes done, bytes in total)
using Progress = std::function<void(uint64_t, uint64_t)>;

// A getfile in progress. size() is the length the console announced; read() hands
// out the bytes and returns 0 once all of them were read, which also gives the
// connection back to the client. The protocol cannot skip the rest of a download,
// so close() or abort() before the end, and every failure, close the connection;
// reconnect the client to continue. Usable from one thread at a time, except
// cancel(), which any thread may call to end a read() that is waiting.
class UPDCLIENT_API FileReader {
public:
  FileReader(FileReader &&) noexcept;
  FileReader &operator=(FileReader &&) noexcept;
  FileReader(const FileReader &) = delete;
  FileReader &operator=(const FileReader &) = delete;
  ~FileReader();

  uint64_t size() const noexcept;
  uint64_t position() const noexcept;
  // True until the last byte was read, the reader was closed, or a read failed.
  bool isOpen() const noexcept;

  Result<size_t> read(std::span<uint8_t> buffer);
  void close() noexcept;
  void abort() noexcept;
  // Thread-safe. Closes the connection; a read() waiting on it fails with Cancelled.
  void cancel() noexcept;

private:
  friend class XbdmClient;
  struct State;
  explicit FileReader(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

// A sendfile in progress, to a temporary name in the target folder. write() sends
// the bytes; finish() wants exactly size() of them, waits for the console's answer
// and only then renames the temporary file to the final name (deleting a file of
// that name first). A refusal after the data, or a failed rename while no file of
// the final name was deleted, deletes the temporary file on the same connection.
// Once the old file was deleted, or its delete got no answer, the temporary file
// may be the only copy of either version: a failed rename then keeps it, names it
// in the error ("the upload is kept as ...") and never queues it for deletion.
// abort() or a failure while data is pending has to close the connection, because
// the console waits for the announced length; the temporary name is then
// remembered and deleted, best effort, by the client's next reconnect(). The final
// name is never touched before the console confirmed the data. Destroying an
// unfinished writer aborts it. Usable from one thread at a time, except cancel().
class UPDCLIENT_API FileWriter {
public:
  FileWriter(FileWriter &&) noexcept;
  FileWriter &operator=(FileWriter &&) noexcept;
  FileWriter(const FileWriter &) = delete;
  FileWriter &operator=(const FileWriter &) = delete;
  ~FileWriter();

  uint64_t size() const noexcept;
  uint64_t written() const noexcept;
  bool isOpen() const noexcept;
  const std::string &path() const noexcept;
  const std::string &temporaryPath() const noexcept;

  // More bytes than size() - written() is InvalidArgument; nothing is sent then.
  Result<void> write(std::span<const uint8_t> data);
  // Before all bytes were written: InvalidArgument, and the writer stays usable.
  Result<void> finish();
  void abort() noexcept;
  // Thread-safe. Closes the connection; a write() or finish() waiting on it fails
  // with Cancelled.
  void cancel() noexcept;

private:
  friend class XbdmClient;
  struct State;
  explicit FileWriter(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

// Client for XBDM on TCP port 730. One command at a time and no pipelining: a
// command is sent only after the previous answer was read to its end. Not
// thread-safe; the exceptions are cancel(), and FileReader::cancel() and
// FileWriter::cancel(), which any thread may call.
//
// Connections. One client is one connection, and one connection serves one
// command or one active file transfer: while a FileReader or FileWriter is open,
// every other call fails with InvalidArgument. For transfers in parallel open a
// second client (XbdmClient::connect again); the console limits how many
// connections it accepts and refuses the next one with 401, reported as
// LimitExceeded with status 401 (section 1.12; the limit is not known).
//
// Failures. A 4xx answer is an error carrying the status (consoleStatusCode) and
// leaves the connection usable. Anything else that goes wrong while an answer is
// being read (a timeout, a malformed or unexpected status, a line or body over
// its limit, bad binary framing, data the console sent unasked, a dropped
// connection, cancel()) closes the connection, because the position in the stream
// is then unknown. The client never reconnects on its own: isConnected() turns
// false and reconnect() opens a new connection when the caller decides to.
//
// Paths are console paths ("HDD:\dir\file", see path.hpp) and are checked before
// anything is sent; toConsolePath converts the "/HDD/dir/file" form.
class UPDCLIENT_API XbdmClient {
public:
  using Connector = std::function<Result<net::TransportPtr>()>;

  // xbdm://host[:port] connects over TCP directly, port 730 by default. Any other
  // scheme, a bare host (tcp) included, goes through the TransportRegistry, so
  // registerBuiltins() must have registered tcp; a missing port becomes 730 there
  // too. Endpoint::timeout bounds the TCP connect.
  static Result<XbdmClient> connect(const net::Endpoint &endpoint, ClientOptions options = {});
  // As above; a stop request ends the connect (xbdm and bare hosts: the TCP connect
  // and its name lookup; other schemes: checked before and after the registry's
  // connect) and the greeting at once with Cancelled. The token is used only during
  // the call; afterwards cancel() ends calls in progress, reconnect() included.
  static Result<XbdmClient> connect(const net::Endpoint &endpoint, ClientOptions options, std::stop_token stop);
  // Opens a connection through the connector and reads the greeting. reconnect()
  // uses the same connector.
  static Result<XbdmClient> open(Connector connector, ClientOptions options = {});
  // As above; a stop request ends the greeting at once with Cancelled. The
  // connector runs to its own end, and the token is checked once it returns.
  static Result<XbdmClient> open(Connector connector, ClientOptions options, std::stop_token stop);
  // Reads the greeting from a connection that was just made. Without a connector,
  // reconnect() is Unsupported.
  static Result<XbdmClient> attach(net::TransportPtr transport, ClientOptions options = {},
                                   Connector connector = {});

  XbdmClient(XbdmClient &&) noexcept;
  XbdmClient &operator=(XbdmClient &&) noexcept;
  XbdmClient(const XbdmClient &) = delete;
  XbdmClient &operator=(const XbdmClient &) = delete;
  // As close(), unless a transfer is open: it keeps the connection until it ends.
  ~XbdmClient();

  bool isConnected() const noexcept;
  bool transferActive() const noexcept;
  std::string describe() const;
  const ClientOptions &options() const noexcept;
  void setOptions(const ClientOptions &options);
  // The status line of the last answer, if there was one.
  std::optional<StatusLine> lastStatus() const;
  // Temporary upload names left behind by aborted uploads, and by a sendfile whose
  // answer never arrived, deleted by reconnect().
  std::vector<std::string> pendingCleanup() const;

  // Closes the current connection (if any), opens a new one through the connector
  // and reads the greeting, then deletes the pending temporary files, best effort.
  // Refused while a transfer is active.
  Result<void> reconnect();
  // Sends "bye" when the connection is idle, then closes it. A transfer that is
  // still open fails from then on.
  void close() noexcept;
  // Thread-safe. Closes the connection at once; the call in progress fails with
  // Cancelled, a reconnect() that is still connecting included. Does not wait for it.
  // A client made by connect() also ends a reconnect()'s TCP connect at once; with a
  // connector given to open() or attach(), cancel() takes effect when the connector
  // returns.
  void cancel() noexcept;

  // Console information (section 3.1).
  Result<std::string> debugName();
  Result<std::string> consoleType();
  Result<std::string> consoleId();
  Result<XbeInfo> runningTitle();
  Result<XbeInfo> executableInfo(const std::string &path);
  Result<ExecStatus> execState();
  Result<TitleAddress> titleAddress();
  // All of the above that the console answers.
  Result<ConsoleInfo> consoleInfo();

  // Drives and files (sections 3.2 to 3.10).
  // Names compared case-insensitively are listed once, at most 64 of them.
  Result<std::vector<std::string>> drives();
  // "HDD", "HDD:" or "HDD:\".
  Result<DriveSpace> driveSpace(const std::string &drive);
  Result<DirListing> list(const std::string &directory);
  // Any 4xx means "does not exist or not accessible" (section 3.4). getfileattributes
  // only; sizeKnown tells whether the answer had a size.
  Result<FileAttributes> attributes(const std::string &path);
  // One level; the parent must exist. 410 when the name exists.
  Result<void> makeDirectory(const std::string &path);
  Result<void> removeFile(const std::string &path);
  // The folder must be empty.
  Result<void> removeDirectory(const std::string &path);
  // Within one drive. Fails with InvalidArgument when the new name exists (checked
  // with getfileattributes before the rename is sent). A new name that differs only
  // in case is allowed: it is sent without the check, and if the console refuses it
  // with 410 or 400 the file goes through an intermediate name in the same folder
  // (three renames; when the last cannot be undone, the error names where the file
  // is).
  Result<void> rename(const std::string &from, const std::string &to);

  // File transfers. expectedSize, when given (from a listing), bounds the length
  // the console may announce; a getfile length can only describe files below
  // 4 GiB, so an expectedSize of 4 GiB or more is Unsupported.
  Result<FileReader> openRead(const std::string &path, std::optional<uint64_t> expectedSize = std::nullopt);
  Result<FileWriter> openWrite(const std::string &path, uint64_t size);
  // Writes "<hostPath>.part" (or "<hostPath>.<random>.part" when that name is
  // taken) and renames it to hostPath after the last byte; a failed download
  // leaves nothing under hostPath. The size from getfileattributes bounds the
  // length getfile announces, so a file of 4 GiB or more is Unsupported instead of
  // arriving cut at its size modulo 4 GiB. When the console does not know
  // getfileattributes (407) or answers without a size, the size comes from the
  // parent folder's listing; when neither gives one, or the path is refused with
  // another 4xx, the download goes ahead without that bound.
  Result<void> downloadToFile(const std::string &path, const std::filesystem::path &hostPath,
                              Progress progress = nullptr);
  Result<void> uploadFromFile(const std::filesystem::path &hostPath, const std::string &path,
                              Progress progress = nullptr);

  // Power (section 3.14). The console drops every connection afterwards, so the
  // client closes its own; reconnect() once the console is back.
  Result<PowerResult> launch(const std::string &executablePath);
  Result<PowerResult> reboot(RebootMode mode = RebootMode::Warm);
  Result<PowerResult> shutdown();
  Result<void> ejectTray();

  Result<Screenshot> screenshot();
  Result<void> setSystemTime(FileTimePoint time);
  Result<void> setSystemTimeRaw(uint64_t fileTime);

  // Raw memory, 32-bit addresses (section 3.16). length must be 1..maxMemoryReadBytes
  // and address + length must not pass 4 GiB.
  // getmem: hex text, unreadable bytes are "??". Sent as requests of at most 0x400
  // bytes (kGetMemChunkBytes); a refusal of any of them fails the call.
  Result<MemoryRead> getMemory(uint32_t address, uint32_t length);
  // getmemex: binary blocks; the bytes after an early last block are unreadable.
  Result<MemoryRead> getMemoryEx(uint32_t address, uint32_t length);
  // setmem in pieces of 64 bytes. A refusal (404 for unmapped memory) stops at that
  // piece; the pieces before it were written.
  Result<void> setMemory(uint32_t address, std::span<const uint8_t> data);
  Result<std::vector<MemoryRegion>> memoryRegions();
  Result<std::vector<Module>> modules();
  Result<std::vector<ModuleSection>> moduleSections(const std::string &module);

  // For diagnostics and hardware tests: sends one line as it is (printable ASCII,
  // not empty, CR LF added) and returns the status line, with the body of a 202.
  // Here a 4xx is an answer, not an error. An answer with binary data or a
  // dedicated connection (203, 204, 205) cannot be read this way: the connection
  // is closed and the call fails with Unsupported.
  Result<RawAnswer> rawCommand(const std::string &line);

private:
  explicit XbdmClient(std::shared_ptr<detail::Session> session);
  std::shared_ptr<detail::Session> session_;
};

// Registers the "xbdm" scheme: TCP with port 730 when the endpoint has none.
UPDCLIENT_API void registerXbdmScheme(net::TransportRegistry &registry = net::TransportRegistry::instance());

} // namespace updclient::xbdm
