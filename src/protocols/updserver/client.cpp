#include <protocols/updserver/client.hpp>

#include <net/transport_registry.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace updclient::updserver {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kBlockAddressSpace = uint64_t{1} << 32;

std::span<const uint8_t> bytesOf(std::string_view text) {
  return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

template <class T> std::span<uint8_t> podBytes(T &value) {
  return {reinterpret_cast<uint8_t *>(&value), sizeof(T)};
}

Error withContext(Error error, std::string_view context) {
  error.message = std::string(context) + ": " + error.message;
  return error;
}

// The protocol has no framing; after a failure that may have left a reply partly
// consumed, close the transport so a later command cannot misread stale bytes.
unexpected<Error> fatal(net::ITransport &t, Error error, std::string_view context) {
  t.close();
  return unexpected<Error>(withContext(std::move(error), context));
}

Result<void> sendBytes(net::ITransport &t, std::span<const uint8_t> data, std::string_view context) {
  if (auto r = t.writeAll(data); !r) return fatal(t, r.error(), context);
  return {};
}

Result<void> sendOp(net::ITransport &t, CommandOp op, std::string_view context) {
  uint32_t be = swapBe(static_cast<uint32_t>(op));
  return sendBytes(t, {reinterpret_cast<const uint8_t *>(&be), sizeof(be)}, context);
}

Result<void> recvBytes(net::ITransport &t, std::span<uint8_t> out, std::string_view context) {
  if (auto r = t.readExact(out); !r) return fatal(t, r.error(), context);
  return {};
}

Result<uint32_t> recvU32(net::ITransport &t, std::string_view context) {
  uint32_t be = 0;
  if (auto r = recvBytes(t, podBytes(be), context); !r) return unexpected<Error>(r.error());
  return swapBe(be);
}

Result<std::vector<uint8_t>> recvPayload(net::ITransport &t, uint64_t length, uint64_t limit,
                                         std::string_view context) {
  if (length > limit) {
    return fatal(t,
                 makeError(ErrorCode::LimitExceeded, "server announced " + std::to_string(length) +
                                                         " bytes, limit is " + std::to_string(limit)),
                 context);
  }
  std::vector<uint8_t> buffer(static_cast<size_t>(length));
  if (auto r = recvBytes(t, buffer, context); !r) return unexpected<Error>(r.error());
  return buffer;
}

Result<std::vector<uint8_t>> recvSizedPayload(net::ITransport &t, uint64_t limit, std::string_view context) {
  auto size = recvU32(t, context);
  if (!size) return unexpected<Error>(size.error());
  return recvPayload(t, *size, limit, context);
}

void appendHex(std::string &out, uint64_t value, unsigned minDigits) {
  static constexpr char digits[] = "0123456789ABCDEF";
  char buffer[16];
  unsigned count = 0;
  do {
    buffer[count++] = digits[value & 0xF];
    value >>= 4;
  } while (value != 0);
  for (unsigned pad = count; pad < minDigits; ++pad) out.push_back('0');
  while (count > 0) out.push_back(buffer[--count]);
}

enum class TextKind {
  Token,    // one whitespace-delimited argument
  Remainder // last argument; may contain spaces
};

// Builds "<VERB> <arg> <arg>\n". Numeric arguments cannot break the line; text
// arguments are checked so a caller-supplied string can never end the command early
// or smuggle in a second one.
class LineBuilder {
public:
  explicit LineBuilder(std::string_view verb) : line_(verb) {}

  LineBuilder &hex(uint64_t value, unsigned minDigits = 1) {
    line_.push_back(' ');
    appendHex(line_, value, minDigits);
    return *this;
  }

  LineBuilder &text(std::string_view value, std::string_view what, TextKind kind) {
    if (error_) return *this;
    if (value.empty()) {
      error_ = makeError(ErrorCode::InvalidArgument, std::string(what) + " must not be empty");
      return *this;
    }
    for (char c : value) {
      const bool forbidden = c == '\r' || c == '\n' || c == '\0' ||
                             (kind == TextKind::Token && (c == ' ' || c == '\t'));
      if (forbidden) {
        error_ = makeError(ErrorCode::InvalidArgument,
                           std::string(what) + " contains a character that is not allowed in a command");
        return *this;
      }
    }
    line_.push_back(' ');
    line_.append(value);
    return *this;
  }

  Result<std::string> finish() {
    if (error_) return unexpected<Error>(*error_);
    line_.push_back('\n');
    if (line_.size() > kMaxCommandLineBytes) {
      return fail(ErrorCode::LimitExceeded, "command line of " + std::to_string(line_.size()) +
                                                " bytes exceeds " + std::to_string(kMaxCommandLineBytes));
    }
    return std::move(line_);
  }

private:
  std::string line_;
  std::optional<Error> error_;
};

Result<void> checkBlockRange(uint32_t block, uint32_t count, uint32_t maxCount) {
  if (count == 0) return fail(ErrorCode::InvalidArgument, "block count must be at least 1");
  if (count > maxCount) {
    return fail(ErrorCode::LimitExceeded,
                "block count " + std::to_string(count) + " exceeds limit " + std::to_string(maxCount));
  }
  if (uint64_t{block} + count > kBlockAddressSpace) {
    return fail(ErrorCode::InvalidArgument, "block range wraps past the 32-bit block space");
  }
  return {};
}

// Writes to "<final>.part" and moves it over the final name only on commit().
class AtomicFile {
public:
  AtomicFile() = default;
  AtomicFile(const AtomicFile &) = delete;
  AtomicFile &operator=(const AtomicFile &) = delete;
  ~AtomicFile() {
    if (committed_ || temp_.empty()) return;
    out_.close();
    std::error_code ec;
    fs::remove(temp_, ec);
  }

  Result<void> open(const fs::path &finalPath) {
    if (finalPath.empty()) return fail(ErrorCode::InvalidArgument, "local path must not be empty");
    std::error_code ec;
    if (fs::is_directory(finalPath, ec)) {
      return fail(ErrorCode::InvalidArgument, "'" + pathToUtf8(finalPath) + "' is a directory");
    }
    final_ = finalPath;
    temp_ = finalPath;
    temp_ += ".part";
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
    if (!out_) return fail(ErrorCode::Io, "flush of '" + pathToUtf8(temp_) + "' failed");
    out_.close();
    if (out_.fail()) return fail(ErrorCode::Io, "closing '" + pathToUtf8(temp_) + "' failed");
    std::error_code ec;
    fs::rename(temp_, final_, ec);
    if (ec) {
      return fail(ErrorCode::Io, "cannot move '" + pathToUtf8(temp_) + "' to '" + pathToUtf8(final_) + "': " + ec.message(),
                  ec.value());
    }
    committed_ = true;
    return {};
  }

private:
  std::ofstream out_;
  fs::path final_;
  fs::path temp_;
  bool committed_ = false;
};

} // namespace

UpdServerClient::UpdServerClient(net::TransportPtr transport, ClientLimits limits)
    : transport_(std::move(transport)), limits_(limits) {}

UpdServerClient::~UpdServerClient() = default;
UpdServerClient::UpdServerClient(UpdServerClient &&) noexcept = default;
UpdServerClient &UpdServerClient::operator=(UpdServerClient &&) noexcept = default;

Result<UpdServerClient> UpdServerClient::connect(const net::Endpoint &endpoint, ClientLimits limits) {
  net::Endpoint target = endpoint;
  if (target.scheme.empty()) target.scheme = "tcp";

  auto &registry = net::TransportRegistry::instance();
  auto transport = registry.connect(registry.withDefaultPort(target, NANDSVR_PORT));
  if (!transport) return unexpected<Error>(transport.error());
  if (!*transport) return fail(ErrorCode::ConnectFailed, "transport registry returned no transport");
  spdlog::debug("connected to UpdServer at {}", (*transport)->describe());
  return UpdServerClient(std::move(*transport), limits);
}

bool UpdServerClient::isConnected() const noexcept {
  return transport_ && transport_->isOpen();
}

void UpdServerClient::disconnect() noexcept {
  if (transport_) transport_->close();
}

std::string UpdServerClient::describe() const {
  return transport_ ? transport_->describe() : std::string("<no transport>");
}

Result<net::ITransport *> UpdServerClient::channel() {
  if (!isConnected()) return fail(ErrorCode::NotConnected, "not connected to UpdServer");
  return transport_.get();
}

Result<NandInfo> UpdServerClient::getInfo() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendOp(t, CommandOp::GetInfo, "GETINFO"); !r) return unexpected<Error>(r.error());
  NandInfo info{};
  if (auto r = recvBytes(t, podBytes(info), "NAND_INFO payload"); !r) return unexpected<Error>(r.error());
  info = swapNandInfo(info);
  blockSize_ = info.blockSize;
  return info;
}

Result<std::string> UpdServerClient::getVersion() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendOp(t, CommandOp::GetVer, "GETVER"); !r) return unexpected<Error>(r.error());
  auto ver = recvU32(t, "version");
  if (!ver) return unexpected<Error>(ver.error());
  return std::to_string((*ver >> 16) & 0xFF) + "." + std::to_string((*ver >> 8) & 0xFF) + "." +
         std::to_string(*ver & 0xFF);
}

Result<std::vector<uint16_t>> UpdServerClient::getBadBlockList() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendOp(t, CommandOp::GetBbList, "GETBBLIST"); !r) return unexpected<Error>(r.error());
  auto count = recvU32(t, "bad block count");
  if (!count) return unexpected<Error>(count.error());
  if (*count > limits_.maxBadBlockCount) {
    return fatal(t,
                 makeError(ErrorCode::LimitExceeded, "server announced " + std::to_string(*count) +
                                                         " bad blocks, limit is " +
                                                         std::to_string(limits_.maxBadBlockCount)),
                 "bad block count");
  }

  std::vector<uint8_t> raw(size_t{*count} * sizeof(uint16_t));
  if (auto r = recvBytes(t, raw, "bad block data"); !r) return unexpected<Error>(r.error());
  std::vector<uint16_t> list(*count);
  for (size_t i = 0; i < list.size(); ++i) list[i] = loadBe16(raw.data() + i * sizeof(uint16_t));
  return list;
}

Result<std::vector<uint8_t>> UpdServerClient::peek(uint32_t addr, uint32_t len) {
  if (len == 0) return fail(ErrorCode::InvalidArgument, "peek length must be at least 1");
  if (len > limits_.maxPeekBytes) {
    return fail(ErrorCode::LimitExceeded, "peek length " + std::to_string(len) + " exceeds limit " +
                                              std::to_string(limits_.maxPeekBytes));
  }
  if (uint64_t{addr} + len > kBlockAddressSpace) {
    return fail(ErrorCode::InvalidArgument, "peek range wraps past the 32-bit address space");
  }
  auto line = LineBuilder("PEEK").hex(addr, 8).hex(len, 8).finish();
  if (!line) return unexpected<Error>(line.error());

  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendBytes(t, bytesOf(*line), "PEEK"); !r) return unexpected<Error>(r.error());
  return recvPayload(t, len, len, "PEEK data");
}

Result<void> UpdServerClient::poke(uint32_t addr, uint32_t value) {
  auto line = LineBuilder("POKE").hex(addr, 8).hex(value, 8).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "POKE");
}

Result<std::vector<uint8_t>> UpdServerClient::hvPeek(uint64_t addr, uint32_t len) {
  if (len == 0) return fail(ErrorCode::InvalidArgument, "hvPeek length must be at least 1");
  if (len > limits_.maxPeekBytes) {
    return fail(ErrorCode::LimitExceeded, "hvPeek length " + std::to_string(len) + " exceeds limit " +
                                              std::to_string(limits_.maxPeekBytes));
  }
  if (uint64_t{len} - 1 > (std::numeric_limits<uint64_t>::max)() - addr) {
    return fail(ErrorCode::InvalidArgument, "hvPeek range wraps past the 64-bit address space");
  }
  auto line = LineBuilder("HVPE").hex(addr, 16).hex(len, 8).finish();
  if (!line) return unexpected<Error>(line.error());

  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendBytes(t, bytesOf(*line), "HVPEEK"); !r) return unexpected<Error>(r.error());
  return recvPayload(t, len, len, "HVPEEK data");
}

Result<void> UpdServerClient::hvPoke(uint64_t addr, uint64_t value) {
  auto line = LineBuilder("HVPO").hex(addr, 16).hex(value, 16).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "HVPOKE");
}

Result<std::vector<uint8_t>> UpdServerClient::get1bl() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendOp(t, CommandOp::Get1Bl, "GET1BL"); !r) return unexpected<Error>(r.error());
  return recvPayload(t, k1blBytes, k1blBytes, "1BL payload");
}

Result<std::vector<uint8_t>> UpdServerClient::getBootloaders() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendOp(t, CommandOp::GetBootloaders, "GETBOOTLOADERS"); !r) return unexpected<Error>(r.error());
  auto data = recvSizedPayload(t, limits_.maxBootloaderBytes, "bootloaders payload");
  if (!data) return data;
  if (data->empty()) return fatal(t, makeError(ErrorCode::Protocol, "server returned no bootloaders"), "GETBOOTLOADERS");
  return data;
}

Result<void> UpdServerClient::getFile(const std::string &remotePath, const fs::path &localPath,
                                      std::function<void(size_t bytesRead)> progressCb) {
  auto line = LineBuilder("GETF").text(remotePath, "remote path", TextKind::Remainder).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  AtomicFile out;
  if (auto r = out.open(localPath); !r) return r;

  if (auto r = sendBytes(t, bytesOf(*line), "GETFILE"); !r) return unexpected<Error>(r.error());
  auto announced = recvU32(t, "file size");
  if (!announced) return unexpected<Error>(announced.error());
  const uint64_t fileSize = *announced;
  if (fileSize > limits_.maxFileBytes) {
    return fatal(t,
                 makeError(ErrorCode::LimitExceeded, "server announced a file of " + std::to_string(fileSize) +
                                                         " bytes, limit is " + std::to_string(limits_.maxFileBytes)),
                 "GETFILE");
  }

  std::vector<uint8_t> chunk(kFileChunkBytes);
  uint64_t received = 0;
  while (received < fileSize) {
    const size_t toRead = static_cast<size_t>(std::min<uint64_t>(chunk.size(), fileSize - received));
    if (auto r = recvBytes(t, std::span<uint8_t>(chunk.data(), toRead), "file download"); !r) {
      return unexpected<Error>(r.error());
    }
    if (auto r = out.write(std::span<const uint8_t>(chunk.data(), toRead)); !r) {
      return fatal(t, r.error(), "file download");
    }
    received += toRead;
    if (progressCb) progressCb(static_cast<size_t>(received));
  }
  return out.commit();
}

Result<void> UpdServerClient::sendFile(const fs::path &localPath, const std::string &remotePath,
                                       std::function<void(size_t bytesSent)> progressCb) {
  auto line = LineBuilder("SNDF").text(remotePath, "remote path", TextKind::Remainder).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (localPath.empty()) return fail(ErrorCode::InvalidArgument, "local path must not be empty");
  const std::string localName = pathToUtf8(localPath);
  std::error_code ec;
  if (!fs::is_regular_file(localPath, ec)) {
    return fail(ErrorCode::InvalidArgument, "'" + localName + "' is not a readable regular file");
  }
  std::ifstream in(localPath, std::ios::binary | std::ios::ate);
  if (!in) return fail(ErrorCode::Io, "cannot open local file '" + localName + "'");
  const auto endPos = in.tellg();
  if (endPos < 0) return fail(ErrorCode::Io, "cannot determine size of '" + localName + "'");
  in.seekg(0, std::ios::beg);
  if (!in) return fail(ErrorCode::Io, "cannot rewind '" + localName + "'");

  const uint64_t fileSize = static_cast<uint64_t>(endPos);
  if (fileSize > kWireMaxSize) {
    return fail(ErrorCode::LimitExceeded, "'" + localName + "' is " + std::to_string(fileSize) +
                                              " bytes; the protocol carries at most 4 GiB - 1");
  }
  if (fileSize > limits_.maxFileBytes) {
    return fail(ErrorCode::LimitExceeded, "'" + localName + "' is " + std::to_string(fileSize) +
                                              " bytes, limit is " + std::to_string(limits_.maxFileBytes));
  }

  std::vector<uint8_t> header(bytesOf(*line).begin(), bytesOf(*line).end());
  uint32_t sizeBe = swapBe(static_cast<uint32_t>(fileSize));
  const auto *sizeBytes = reinterpret_cast<const uint8_t *>(&sizeBe);
  header.insert(header.end(), sizeBytes, sizeBytes + sizeof(sizeBe));
  if (auto r = sendBytes(t, header, "SENDFILE header"); !r) return r;

  std::vector<uint8_t> chunk(kFileChunkBytes);
  uint64_t sent = 0;
  while (sent < fileSize) {
    const size_t toSend = static_cast<size_t>(std::min<uint64_t>(chunk.size(), fileSize - sent));
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(toSend));
    if (static_cast<size_t>(in.gcount()) != toSend) {
      return fatal(t, makeError(ErrorCode::Io, "read from '" + localName + "' failed or file shrank"),
                   "file upload");
    }
    if (auto r = sendBytes(t, std::span<const uint8_t>(chunk.data(), toSend), "file upload"); !r) return r;
    sent += toSend;
    if (progressCb) progressCb(static_cast<size_t>(sent));
  }
  return {};
}

Result<void> UpdServerClient::mount(const std::string &mountPoint, const std::string &devicePath) {
  auto line = LineBuilder("MTPT")
                  .text(mountPoint, "mount point", TextKind::Token)
                  .text(devicePath, "device path", TextKind::Remainder)
                  .finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "MOUNTPATH");
}

Result<void> UpdServerClient::unmount(const std::string &mountPoint) {
  auto line = LineBuilder("UMPT").text(mountPoint, "mount point", TextKind::Remainder).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "UNMOUNTPATH");
}

Result<void> UpdServerClient::mkDir(const std::string &remotePath) {
  auto line = LineBuilder("MKDR").text(remotePath, "remote path", TextKind::Remainder).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "MKDIR");
}

Result<std::vector<uint8_t>> UpdServerClient::readBlock(uint32_t block, uint32_t count) {
  if (auto r = checkBlockRange(block, count, limits_.maxBlockCount); !r) return unexpected<Error>(r.error());
  auto line = LineBuilder("RBLK").hex(block).hex(count).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  if (auto r = sendBytes(t, bytesOf(*line), "READBLOCK"); !r) return unexpected<Error>(r.error());
  auto data = recvSizedPayload(t, limits_.maxBlockPayload, "block data");
  if (!data) return data;
  if (data->empty()) return fatal(t, makeError(ErrorCode::Protocol, "server returned an empty block"), "READBLOCK");
  return data;
}

Result<void> UpdServerClient::writeBlock(uint32_t block, std::span<const uint8_t> data) {
  if (data.empty()) return fail(ErrorCode::InvalidArgument, "block data must not be empty");
  if (data.size() > limits_.maxBlockWriteBytes) {
    return fail(ErrorCode::LimitExceeded, "block data of " + std::to_string(data.size()) +
                                              " bytes exceeds limit " + std::to_string(limits_.maxBlockWriteBytes));
  }
  if (blockSize_ != 0 && data.size() != blockSize_) {
    return fail(ErrorCode::InvalidArgument, "block data is " + std::to_string(data.size()) +
                                                " bytes, the device block size is " + std::to_string(blockSize_));
  }
  auto line = LineBuilder("WBLK").hex(block).hex(1).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());

  std::vector<uint8_t> message(bytesOf(*line).begin(), bytesOf(*line).end());
  message.insert(message.end(), data.begin(), data.end());
  return sendBytes(**ch, message, "WRITEBLOCK");
}

Result<void> UpdServerClient::eraseBlock(uint32_t block, uint32_t count) {
  if (auto r = checkBlockRange(block, count, limits_.maxBlockCount); !r) return r;
  auto line = LineBuilder("ERBL").hex(block).hex(count).finish();
  if (!line) return unexpected<Error>(line.error());
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendBytes(**ch, bytesOf(*line), "ERASEBLOCK");
}

Result<void> UpdServerClient::dumpFlash(const fs::path &outputPath, size_t dumpSize,
                                        std::function<void(size_t, size_t)> progressCb) {
  if (dumpSize == 0) return fail(ErrorCode::InvalidArgument, "dump size must be at least 1");
  if (dumpSize > limits_.maxDumpBytes) {
    return fail(ErrorCode::LimitExceeded, "dump size " + std::to_string(dumpSize) + " exceeds limit " +
                                              std::to_string(limits_.maxDumpBytes));
  }
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  auto &t = **ch;

  AtomicFile out;
  if (auto r = out.open(outputPath); !r) return r;

  if (auto r = sendOp(t, CommandOp::GetFlash, "GETFLASH"); !r) return r;

  std::vector<uint8_t> chunk(kDumpChunkBytes);
  size_t received = 0;
  while (received < dumpSize) {
    const size_t toRead = std::min(chunk.size(), dumpSize - received);
    if (auto r = recvBytes(t, std::span<uint8_t>(chunk.data(), toRead), "NAND dump stream"); !r) return r;
    if (auto r = out.write(std::span<const uint8_t>(chunk.data(), toRead)); !r) {
      return fatal(t, r.error(), "NAND dump stream");
    }
    received += toRead;
    if (progressCb) progressCb(received, dumpSize);
  }
  return out.commit();
}

Result<void> UpdServerClient::reboot() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendOp(**ch, CommandOp::Reboot, "REBOOT");
}

Result<void> UpdServerClient::smcReboot() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendOp(**ch, CommandOp::SmcReboot, "SMCREBOOT");
}

Result<void> UpdServerClient::shutdownConsole() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendOp(**ch, CommandOp::Shutdown, "SHUTDOWN");
}

Result<void> UpdServerClient::quit() {
  auto ch = channel();
  if (!ch) return unexpected<Error>(ch.error());
  return sendOp(**ch, CommandOp::Quit, "QUIT");
}

} // namespace updclient::updserver
