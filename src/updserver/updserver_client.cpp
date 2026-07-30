#include "updserver/updserver_client.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

namespace updclient::updserver {

bool UpdServerClient::connect(const std::string &ipAddress, uint16_t port) {
  bool success = tcpSocket_.connect(ipAddress, port, 5000);
  if (success) {
    spdlog::info("Connected to UpdServer at {}:{}", ipAddress, port);
  }
  return success;
}

void UpdServerClient::disconnect() {
  tcpSocket_.disconnect();
}

// ---------------- COMMAND IMPLEMENTATIONS ----------------

expected<NandInfo, std::string> UpdServerClient::getInfo() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetInfo));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETINFO command code");
  }

  NandInfo info{};
  if (!tcpSocket_.recvRaw(&info, sizeof(NandInfo))) {
    return unexpected("Failed to receive NAND_INFO payload from server");
  }

  return swap_nand_info(info);
}

expected<std::string, std::string> UpdServerClient::getVersion() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetVer));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETVER command code");
  }

  uint32_t versionBe = 0;
  if (!tcpSocket_.recvRaw(&versionBe, sizeof(versionBe))) {
    return unexpected("Failed to receive version from server");
  }

  uint32_t ver = swap_be(versionBe);
  return std::format("{}.{}.{}", (ver >> 16) & 0xFF, (ver >> 8) & 0xFF,
                     ver & 0xFF);
}

expected<std::vector<uint16_t>, std::string> UpdServerClient::getBadBlockList() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetBbList));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETBBLIST command code");
  }

  uint32_t countBe = 0;
  if (!tcpSocket_.recvRaw(&countBe, sizeof(countBe))) {
    return unexpected("Failed to receive bad block count");
  }

  uint32_t count = swap_be(countBe);
  std::vector<uint16_t> bbList(count);
  if (count > 0) {
    if (!tcpSocket_.recvRaw(bbList.data(), count * sizeof(uint16_t))) {
      return unexpected("Failed to receive bad block data payload");
    }
    for (auto &bb : bbList) {
      bb = swap_be(bb);
    }
  }
  return bbList;
}

expected<std::vector<uint8_t>, std::string> UpdServerClient::peek(uint32_t addr,
                                                                  uint32_t len) {
  std::string cmd = std::format("PEEK {:08X} {:08X}\n", addr, len);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send PEEK command");
  }

  std::vector<uint8_t> buffer(len);
  if (!tcpSocket_.recvRaw(buffer.data(), len)) {
    return unexpected("Failed to receive memory PEEK data payload");
  }
  return buffer;
}

expected<void, std::string> UpdServerClient::poke(uint32_t addr, uint32_t value) {
  std::string cmd = std::format("POKE {:08X} {:08X}\n", addr, value);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send POKE command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string> UpdServerClient::hvPeek(uint64_t addr,
                                                                    uint32_t len) {
  std::string cmd = std::format("HVPE {:016X} {:08X}\n", addr, len);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send HVPEEK command");
  }

  std::vector<uint8_t> buffer(len);
  if (!tcpSocket_.recvRaw(buffer.data(), len)) {
    return unexpected("Failed to receive HVPEEK data payload");
  }
  return buffer;
}

expected<void, std::string> UpdServerClient::hvPoke(uint64_t addr, uint64_t value) {
  std::string cmd = std::format("HVPO {:016X} {:016X}\n", addr, value);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send HVPOKE command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string> UpdServerClient::get1bl() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Get1Bl));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GET1BL command");
  }

  std::vector<uint8_t> buffer(0x8000); // 1BL size 32KB
  if (!tcpSocket_.recvRaw(buffer.data(), buffer.size())) {
    return unexpected("Failed to receive 1BL payload");
  }
  return buffer;
}

expected<std::vector<uint8_t>, std::string> UpdServerClient::getBootloaders() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetBootloaders));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
    return unexpected("Failed to send GETBOOTLOADERS command");
  }

  uint32_t sizeBe = 0;
  if (!tcpSocket_.recvRaw(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to receive bootloader size");
  }

  uint32_t totalSize = swap_be(sizeBe);
  std::vector<uint8_t> buffer(totalSize);
  if (!tcpSocket_.recvRaw(buffer.data(), totalSize)) {
    return unexpected("Failed to receive bootloaders payload");
  }
  return buffer;
}

expected<void, std::string>
UpdServerClient::getFile(const std::string &remotePath, const std::string &localPath,
                          std::function<void(size_t bytesRead)> progressCb) {
  std::string cmd = std::format("GETF {}\n", remotePath);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send GETFILE command");
  }

  uint32_t sizeBe = 0;
  if (!tcpSocket_.recvRaw(&sizeBe, sizeof(sizeBe))) {
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
    if (!tcpSocket_.recvRaw(chunk.data(), toRead)) {
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
UpdServerClient::sendFile(const std::string &localPath, const std::string &remotePath,
                           std::function<void(size_t bytesSent)> progressCb) {
  std::ifstream inFile(localPath, std::ios::binary | std::ios::ate);
  if (!inFile) {
    return unexpected(
        std::format("Failed to open local source file: {}", localPath));
  }
  size_t fileSize = inFile.tellg();
  inFile.seekg(0, std::ios::beg);

  std::string cmd = std::format("SNDF {}\n", remotePath);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send SENDFILE command");
  }

  uint32_t sizeBe = swap_be(static_cast<uint32_t>(fileSize));
  if (!tcpSocket_.sendRaw(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to send file size header");
  }

  size_t totalSent = 0;
  constexpr size_t CHUNK_SIZE = 1452;
  std::vector<uint8_t> chunk(CHUNK_SIZE);

  while (totalSent < fileSize) {
    size_t toSend = std::min<size_t>(CHUNK_SIZE, fileSize - totalSent);
    inFile.read(reinterpret_cast<char *>(chunk.data()), toSend);
    if (!tcpSocket_.sendRaw(chunk.data(), toSend)) {
      return unexpected("File send upload interrupted");
    }
    totalSent += toSend;
    if (progressCb)
      progressCb(totalSent);
  }

  return {};
}

expected<void, std::string>
UpdServerClient::mount(const std::string &mountPoint, const std::string &devicePath) {
  std::string cmd = std::format("MTPT {} {}\n", mountPoint, devicePath);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send MOUNTPATH command");
  }
  return {};
}

expected<void, std::string> UpdServerClient::unmount(const std::string &mountPoint) {
  std::string cmd = std::format("UMPT {}\n", mountPoint);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send UNMOUNTPATH command");
  }
  return {};
}

expected<void, std::string> UpdServerClient::mkDir(const std::string &remotePath) {
  std::string cmd = std::format("MKDR {}\n", remotePath);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send MKDIR command");
  }
  return {};
}

expected<std::vector<uint8_t>, std::string>
UpdServerClient::readBlock(uint32_t block, uint32_t count) {
  std::string cmd = std::format("RBLK {:X} {:X}\n", block, count);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send READBLOCK command");
  }

  uint32_t sizeBe = 0;
  if (!tcpSocket_.recvRaw(&sizeBe, sizeof(sizeBe))) {
    return unexpected("Failed to receive block payload size");
  }
  uint32_t totalSize = swap_be(sizeBe);

  std::vector<uint8_t> buffer(totalSize);
  if (!tcpSocket_.recvRaw(buffer.data(), totalSize)) {
    return unexpected("Failed to receive block data");
  }
  return buffer;
}

expected<void, std::string>
UpdServerClient::writeBlock(uint32_t block, const std::vector<uint8_t> &data) {
  std::string cmd = std::format("WBLK {:X} 1\n", block);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send WRITEBLOCK command");
  }

  if (!tcpSocket_.sendRaw(data.data(), data.size())) {
    return unexpected("Failed to send block payload");
  }
  return {};
}

expected<void, std::string> UpdServerClient::eraseBlock(uint32_t block,
                                                         uint32_t count) {
  std::string cmd = std::format("ERBL {:X} {:X}\n", block, count);
  if (!tcpSocket_.sendString(cmd)) {
    return unexpected("Failed to send ERASEBLOCK command");
  }
  return {};
}

expected<void, std::string> UpdServerClient::dumpFlash(
    const std::string &outputPath, size_t dumpSize,
    std::function<void(size_t bytesRead, size_t totalSize)> progressCb) {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::GetFlash));
  if (!tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe))) {
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
    if (!tcpSocket_.recvRaw(chunk.data(), toRead)) {
      return unexpected("NAND flash dump payload stream interrupted");
    }
    outFile.write(reinterpret_cast<const char *>(chunk.data()), toRead);
    totalReceived += toRead;
    if (progressCb)
      progressCb(totalReceived, dumpSize);
  }
  return {};
}

expected<void, std::string> UpdServerClient::reboot() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Reboot));
  tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> UpdServerClient::smcReboot() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::SmcReboot));
  tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> UpdServerClient::shutdownConsole() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Shutdown));
  tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe));
  return {};
}

expected<void, std::string> UpdServerClient::quit() {
  uint32_t cmdBe = swap_be(static_cast<uint32_t>(CommandOp::Quit));
  tcpSocket_.sendRaw(&cmdBe, sizeof(cmdBe));
  return {};
}

} // namespace updclient::updserver

namespace updclient {

std::string format_hex_bytes(const uint8_t *data, size_t length) {
  std::stringstream ss;
  for (size_t i = 0; i < length; ++i) {
    ss << std::format("{:02X}", data[i]);
  }
  return ss.str();
}

} // namespace updclient

