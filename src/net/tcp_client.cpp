#include "updclient/tcp_client.hpp"
#include "updclient/net_compat.hpp"
#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace updclient {

TcpClient::~TcpClient() { disconnect(); }

bool TcpClient::connect(const std::string &ipAddress, uint16_t port) {
  disconnect();

  socketFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd_ == INVALID_SOCKET_FD) {
    spdlog::error("Failed to create TCP socket (err {})", get_last_socket_error());
    return false;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(port);

  if (::inet_pton(AF_INET, ipAddress.c_str(), &serverAddr.sin_addr) <= 0) {
    spdlog::error("Invalid IP address: {}", ipAddress);
    disconnect();
    return false;
  }

  // Set connection timeout (5 seconds)
  set_socket_timeout(socketFd_, 5000);

  if (::connect(socketFd_, reinterpret_cast<sockaddr *>(&serverAddr),
                sizeof(serverAddr)) < 0) {
    spdlog::error("Failed to connect to UpdServer at {}:{}: (err {})", ipAddress,
                  port, get_last_socket_error());
    disconnect();
    return false;
  }

  targetIp_ = ipAddress;
  spdlog::info("Connected to UpdServer at {}:{}", ipAddress, port);
  return true;
}

void TcpClient::disconnect() {
  if (socketFd_ != INVALID_SOCKET_FD) {
    close_socket(socketFd_);
    socketFd_ = INVALID_SOCKET_FD;
  }
}

bool TcpClient::sendCommandString(const std::string &cmdStr) {
  if (!isConnected())
    return false;
  spdlog::debug("Sending Command String: '{}'", cmdStr);
  return sendRawData(cmdStr.c_str(), cmdStr.length());
}

bool TcpClient::sendRawData(const void *data, size_t length) {
  if (!isConnected())
    return false;
  size_t totalSent = 0;
  const auto *ptr = static_cast<const char *>(data);

  while (totalSent < length) {
    auto sent = ::send(socketFd_, ptr + totalSent, static_cast<int>(length - totalSent), 0);
    if (sent <= 0) {
      spdlog::error("Socket send failed (err {})", get_last_socket_error());
      return false;
    }
    totalSent += sent;
  }
  return true;
}

bool TcpClient::receiveRawData(void *data, size_t length) {
  if (!isConnected())
    return false;
  size_t totalRead = 0;
  auto *ptr = static_cast<char *>(data);

  while (totalRead < length) {
    auto bytes = ::recv(socketFd_, ptr + totalRead, static_cast<int>(length - totalRead), 0);
    if (bytes <= 0) {
      spdlog::error("Socket receive failed (err {})", get_last_socket_error());
      return false;
    }
    totalRead += bytes;
  }
  return true;
}

std::string TcpClient::receiveStringResponse() {
  if (!isConnected())
    return {};
  char buf[1024];
  auto bytes = ::recv(socketFd_, buf, static_cast<int>(sizeof(buf) - 1), 0);
  if (bytes > 0) {
    buf[bytes] = '\0';
    return std::string(buf);
  }
  return {};
}

// ---------------- COMMAND IMPLEMENTATIONS ----------------

expected<NandInfo, std::string> TcpClient::getInfo() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetInfo));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETINFO command code");
  }

  NandInfo info{};
  if (!receiveRawData(&info, sizeof(NandInfo))) {
    return unexpected("Failed to receive NAND_INFO payload from server");
  }

  return swap_nand_info(info);
}

expected<std::string, std::string> TcpClient::getVersion() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetVer));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETVER command code");
  }

  uint32_t versionBe = 0;
  if (!receiveRawData(&versionBe, sizeof(versionBe))) {
    return unexpected("Failed to receive version from server");
  }

  uint32_t ver = swap_be(versionBe);
  return std::format("{}.{}.{}", (ver >> 16) & 0xFF, (ver >> 8) & 0xFF,
                     ver & 0xFF);
}

expected<std::vector<uint16_t>, std::string> TcpClient::getBadBlockList() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetBbList));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETBBLIST command code");
  }

  uint32_t countBe = 0;
  if (!receiveRawData(&countBe, sizeof(countBe))) {
    return unexpected("Failed to receive bad block count");
  }

  uint32_t count = swap_be(countBe);
  std::vector<uint16_t> bbList(count);
  if (count > 0) {
    if (!receiveRawData(bbList.data(), count * sizeof(uint16_t))) {
      return unexpected("Failed to receive bad block data payload");
    }
    for (auto &bb : bbList) {
      bb = swap_be(bb);
    }
  }
  return bbList;
}

expected<std::vector<uint8_t>, std::string> TcpClient::peek(uint32_t addr,
                                                                 uint32_t len) {
  std::string cmd = std::format("PEEK {:08X} {:08X}\n", addr, len);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send PEEK command");
  }

  std::vector<uint8_t> buffer(len);
  if (!receiveRawData(buffer.data(), len)) {
    return unexpected("Failed to receive memory PEEK data payload");
  }
  return buffer;
}

expected<void, std::string> TcpClient::poke(uint32_t addr,
                                                 uint32_t value) {
  std::string cmd = std::format("POKE {:08X} {:08X}\n", addr, value);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send POKE command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string>
TcpClient::hvPeek(uint64_t addr, uint32_t len) {
  std::string cmd = std::format("HVPE {:016X} {:08X}\n", addr, len);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send HVPEEK command");
  }

  std::vector<uint8_t> buffer(len);
  if (!receiveRawData(buffer.data(), len)) {
    return unexpected("Failed to receive HVPEEK data payload");
  }
  return buffer;
}

expected<void, std::string> TcpClient::hvPoke(uint64_t addr,
                                                   uint64_t value) {
  std::string cmd = std::format("HVPO {:016X} {:016X}\n", addr, value);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send HVPOKE command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string> TcpClient::get1bl() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Get1Bl));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GET1BL command");
  }

  std::vector<uint8_t> buffer(0x8000); // 1BL size 32KB
  if (!receiveRawData(buffer.data(), buffer.size())) {
    return unexpected("Failed to receive 1BL payload");
  }
  return buffer;
}

expected<std::vector<uint8_t>, std::string> TcpClient::getBootloaders() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetBootloaders));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETBOOTLOADERS command");
  }

  uint32_t sizeBe = 0;
  if (!receiveRawData(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to receive bootloader size");
  }

  uint32_t totalSize = swap_be(sizeBe);
  std::vector<uint8_t> buffer(totalSize);
  if (!receiveRawData(buffer.data(), totalSize)) {
    return unexpected("Failed to receive bootloaders payload");
  }
  return buffer;
}

expected<void, std::string>
TcpClient::getFile(const std::string &remotePath, const std::string &localPath,
                   std::function<void(size_t bytesRead)> progressCb) {
  std::string cmd = std::format("GETF {}\n", remotePath);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send GETFILE command");
  }

  uint32_t sizeBe = 0;
  if (!receiveRawData(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to receive file size from server");
  }
  uint32_t fileSize = swap_be(sizeBe);

  std::ofstream outFile(localPath, std::ios::binary);
  if (!outFile) {
    return unexpected(
        std::format("Failed to open local destination file: {}", localPath));
  }

  size_t totalReceived = 0;
  constexpr size_t CHUNK_SIZE = 1452;
  std::vector<uint8_t> chunk(CHUNK_SIZE);

  while (totalReceived < fileSize) {
    size_t toRead = std::min<size_t>(CHUNK_SIZE, fileSize - totalReceived);
    if (!receiveRawData(chunk.data(), toRead)) {
      return unexpected("File download payload interrupted");
    }
    outFile.write(reinterpret_cast<const char *>(chunk.data()), toRead);
    totalReceived += toRead;
    if (progressCb)
      progressCb(totalReceived);
  }
  return {};
}

expected<void, std::string>
TcpClient::sendFile(const std::string &localPath, const std::string &remotePath,
                    std::function<void(size_t bytesSent)> progressCb) {
  std::ifstream inFile(localPath, std::ios::binary | std::ios::ate);
  if (!inFile) {
    return unexpected(
        std::format("Failed to open local source file: {}", localPath));
  }
  size_t fileSize = inFile.tellg();
  inFile.seekg(0, std::ios::beg);

  std::string cmd = std::format("SNDF {}\n", remotePath);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send SENDFILE command");
  }

  uint32_t sizeBe = swap_be(static_cast<uint32_t>(fileSize));
  if (!sendRawData(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to send file size header");
  }

  size_t totalSent = 0;
  constexpr size_t CHUNK_SIZE = 1452;
  std::vector<uint8_t> chunk(CHUNK_SIZE);

  while (totalSent < fileSize) {
    size_t toSend = std::min<size_t>(CHUNK_SIZE, fileSize - totalSent);
    inFile.read(reinterpret_cast<char *>(chunk.data()), toSend);
    if (!sendRawData(chunk.data(), toSend)) {
      return unexpected("File send upload interrupted");
    }
    totalSent += toSend;
    if (progressCb)
      progressCb(totalSent);
  }

  return {};
}

expected<void, std::string>
TcpClient::mount(const std::string &mountPoint, const std::string &devicePath) {
  std::string cmd = std::format("MTPT {} {}\n", mountPoint, devicePath);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send MOUNTPATH command");
  }
  return {};
}

expected<void, std::string>
TcpClient::unmount(const std::string &mountPoint) {
  std::string cmd = std::format("UMPT {}\n", mountPoint);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send UNMOUNTPATH command");
  }
  return {};
}

expected<void, std::string>
TcpClient::mkDir(const std::string &remotePath) {
  std::string cmd = std::format("MKDR {}\n", remotePath);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send MKDIR command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string>
TcpClient::readBlock(uint32_t block, uint32_t count) {
  std::string cmd = std::format("RBLK {:X} {:X}\n", block, count);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send READBLOCK command");
  }

  uint32_t sizeBe = 0;
  if (!receiveRawData(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to receive block payload size");
  }
  uint32_t totalSize = swap_be(sizeBe);

  std::vector<uint8_t> buffer(totalSize);
  if (!receiveRawData(buffer.data(), totalSize)) {
    return unexpected("Failed to receive block data");
  }
  return buffer;
}

expected<void, std::string>
TcpClient::writeBlock(uint32_t block, const std::vector<uint8_t> &data) {
  std::string cmd = std::format("WBLK {:X} 1\n", block);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send WRITEBLOCK command");
  }

  if (!sendRawData(data.data(), data.size())) {
    return unexpected("Failed to send block payload");
  }
  return {};
}

expected<void, std::string> TcpClient::eraseBlock(uint32_t block,
                                                       uint32_t count) {
  std::string cmd = std::format("ERBL {:X} {:X}\n", block, count);
  if (!sendCommandString(cmd)) {
    return unexpected("Failed to send ERASEBLOCK command");
  }
  return {};
}

expected<void, std::string> TcpClient::dumpFlash(
    const std::string &outputPath, size_t dumpSize,
    std::function<void(size_t bytesRead, size_t totalSize)> progressCb) {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetFlash));
  if (!sendRawData(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETFLASH command");
  }

  std::ofstream outFile(outputPath, std::ios::binary);
  if (!outFile) {
    return unexpected(
        std::format("Failed to open output NAND dump file: {}", outputPath));
  }

  size_t totalReceived = 0;
  constexpr size_t CHUNK_SIZE = 64 * 1024;
  std::vector<uint8_t> chunk(CHUNK_SIZE);

  while (totalReceived < dumpSize) {
    size_t toRead = std::min<size_t>(CHUNK_SIZE, dumpSize - totalReceived);
    if (!receiveRawData(chunk.data(), toRead)) {
      return unexpected("NAND flash dump payload stream interrupted");
    }
    outFile.write(reinterpret_cast<const char *>(chunk.data()), toRead);
    totalReceived += toRead;
    if (progressCb)
      progressCb(totalReceived, dumpSize);
  }
  return {};
}

expected<void, std::string> TcpClient::reboot() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Reboot));
  sendRawData(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> TcpClient::smcReboot() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::SmcReboot));
  sendRawData(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> TcpClient::shutdownConsole() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Shutdown));
  sendRawData(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> TcpClient::quit() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Quit));
  sendRawData(&cmdBe, sizeof(cmdBe));
  return {};
}

std::string format_hex_bytes(const uint8_t *data, size_t length) {
  std::stringstream ss;
  for (size_t i = 0; i < length; ++i) {
    ss << std::format("{:02X}", data[i]);
  }
  return ss.str();
}

} // namespace updclient
