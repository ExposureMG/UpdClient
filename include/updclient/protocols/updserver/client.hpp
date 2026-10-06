#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>
#include <updclient/core/path.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/transport.hpp>
#include <updclient/protocols/updserver/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::updserver {

// Bounds applied to every size that comes from the server or from the caller.
// Exceeding one yields ErrorCode::LimitExceeded. Defaults are the k* constants
// in protocol.hpp.
struct ClientLimits {
  size_t maxBadBlockCount = kMaxBadBlockCount;
  size_t maxBootloaderBytes = kMaxBootloaderBytes;
  size_t maxBlockPayload = kMaxBlockPayload;     // reply of readBlock
  size_t maxBlockWriteBytes = kMaxBlockWriteBytes; // payload of writeBlock
  uint32_t maxBlockCount = kMaxBlockCount;       // blocks per readBlock/eraseBlock
  size_t maxPeekBytes = kMaxPeekBytes;           // peek / hvPeek length
  uint64_t maxFileBytes = kMaxFileBytes;         // getFile / sendFile
  uint64_t maxDumpBytes = kMaxDumpBytes;         // dumpFlash
};

// Client for the UpdServer console service. The byte stream is injected, so any
// net::ITransport can carry it. Not thread-safe: one command at a time.
//
// Acknowledgements. The server is not known to reply to the commands below, so a
// successful Result<void> only means "every byte was handed to the transport", not
// that the console executed it. Verify through a follow-up query (getInfo,
// peek, readBlock, ...) when the effect matters.
//   reboot, smcReboot, shutdownConsole, quit, poke, hvPoke, mount, unmount, mkDir,
//   eraseBlock, writeBlock, and the upload performed by sendFile.
// All other commands read a reply, and any short or malformed reply is an error.
//
// Stream integrity. The protocol has no framing, so once a command fails after
// bytes were exchanged (transport error, oversized reply, local disk error while
// receiving) the position in the stream is unknown. The client then closes the
// transport and isConnected() turns false; reconnect to continue. Errors raised
// before anything is sent (InvalidArgument, a local file that cannot be opened,
// LimitExceeded on a request) leave the connection usable.
//
// Local files are std::filesystem::path so non-ASCII names survive on Windows; build
// one from UTF-8 text with pathFromUtf8 (core/path.hpp). Remote paths are plain
// bytes sent to the console as given.
//
// Text arguments (remote paths, mount points) must not contain CR, LF or NUL, and a
// mount point must not contain spaces or tabs; otherwise ErrorCode::InvalidArgument.
class UPDCLIENT_API UpdServerClient {
public:
  explicit UpdServerClient(net::TransportPtr transport, ClientLimits limits = {});
  ~UpdServerClient();

  UpdServerClient(UpdServerClient &&) noexcept;
  UpdServerClient &operator=(UpdServerClient &&) noexcept;
  UpdServerClient(const UpdServerClient &) = delete;
  UpdServerClient &operator=(const UpdServerClient &) = delete;

  // Connects through the TransportRegistry; port 0 selects NANDSVR_PORT and an
  // empty scheme selects tcp.
  static Result<UpdServerClient> connect(const net::Endpoint &endpoint, ClientLimits limits = {});

  bool isConnected() const noexcept;
  void disconnect() noexcept;
  std::string describe() const;

  const ClientLimits &limits() const noexcept { return limits_; }
  void setLimits(const ClientLimits &limits) noexcept { limits_ = limits; }
  // Block size reported by the last successful getInfo(), or 0 if not yet known.
  // When known, writeBlock requires payloads of exactly this size.
  uint32_t knownBlockSize() const noexcept { return blockSize_; }

  Result<NandInfo> getInfo();
  Result<std::string> getVersion();
  Result<std::vector<uint16_t>> getBadBlockList();

  // len must be in 1..limits().maxPeekBytes and addr + len must not wrap.
  Result<std::vector<uint8_t>> peek(uint32_t addr, uint32_t len);
  Result<void> poke(uint32_t addr, uint32_t value);
  Result<std::vector<uint8_t>> hvPeek(uint64_t addr, uint32_t len);
  Result<void> hvPoke(uint64_t addr, uint64_t value);
  Result<std::vector<uint8_t>> get1bl();
  Result<std::vector<uint8_t>> getBootloaders();

  // The local file is opened (as "<localPath>.part") before the command is sent and
  // renamed to localPath only after a complete, flushed download, so a failed
  // transfer never leaves a truncated file under the final name.
  Result<void> getFile(const std::string &remotePath, const std::filesystem::path &localPath,
                       std::function<void(size_t bytesRead)> progressCb = nullptr);
  // The local file is validated before the command is sent. Files of 4 GiB or more
  // cannot be expressed by the protocol and are rejected with LimitExceeded.
  Result<void> sendFile(const std::filesystem::path &localPath, const std::string &remotePath,
                        std::function<void(size_t bytesSent)> progressCb = nullptr);
  Result<void> mount(const std::string &mountPoint, const std::string &devicePath);
  Result<void> unmount(const std::string &mountPoint);
  Result<void> mkDir(const std::string &remotePath);

  // count must be in 1..limits().maxBlockCount and block + count must not wrap.
  Result<std::vector<uint8_t>> readBlock(uint32_t block, uint32_t count = 1);
  // Writes exactly one block. data must be non-empty, at most
  // limits().maxBlockWriteBytes, and equal to knownBlockSize() when that is known.
  Result<void> writeBlock(uint32_t block, std::span<const uint8_t> data);
  Result<void> eraseBlock(uint32_t block, uint32_t count = 1);
  // Streams dumpSize bytes of flash into outputPath, with the same temp-file and
  // rename guarantees as getFile.
  Result<void> dumpFlash(const std::filesystem::path &outputPath, size_t dumpSize,
                         std::function<void(size_t bytesRead, size_t totalSize)> progressCb = nullptr);

  Result<void> reboot();
  Result<void> smcReboot();
  Result<void> shutdownConsole();
  Result<void> quit();

private:
  Result<net::ITransport *> channel();

  net::TransportPtr transport_;
  ClientLimits limits_;
  uint32_t blockSize_ = 0;
};

} // namespace updclient::updserver
