#pragma once

#include "protocol.hpp"
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace updclient {

class TcpClient {
public:
  TcpClient() = default;
  ~TcpClient();

  bool connect(const std::string &ipAddress, uint16_t port = NANDSVR_PORT);
  void disconnect();
  bool isConnected() const noexcept { return socketFd_ >= 0; }
  const std::string &targetIp() const noexcept { return targetIp_; }

  // Core Command Methods
  std::expected<NandInfo, std::string> getInfo();
  std::expected<std::string, std::string> getVersion();
  std::expected<std::vector<uint16_t>, std::string> getBadBlockList();

  // Memory & Hardware
  std::expected<std::vector<uint8_t>, std::string> peek(uint32_t addr,
                                                        uint32_t len);
  std::expected<void, std::string> poke(uint32_t addr, uint32_t value);
  std::expected<std::vector<uint8_t>, std::string> hvPeek(uint64_t addr,
                                                          uint32_t len);
  std::expected<void, std::string> hvPoke(uint64_t addr, uint64_t value);
  std::expected<std::vector<uint8_t>, std::string> get1bl();
  std::expected<std::vector<uint8_t>, std::string> getBootloaders();

  // File & Storage
  std::expected<void, std::string>
  getFile(const std::string &remotePath, const std::string &localPath,
          std::function<void(size_t bytesRead)> progressCb = nullptr);
  std::expected<void, std::string>
  sendFile(const std::string &localPath, const std::string &remotePath,
           std::function<void(size_t bytesSent)> progressCb = nullptr);
  std::expected<void, std::string> mount(const std::string &mountPoint,
                                         const std::string &devicePath);
  std::expected<void, std::string> unmount(const std::string &mountPoint);
  std::expected<void, std::string> mkDir(const std::string &remotePath);

  // NAND Operations
  std::expected<std::vector<uint8_t>, std::string>
  readBlock(uint32_t block, uint32_t count = 1);
  std::expected<void, std::string> writeBlock(uint32_t block,
                                              const std::vector<uint8_t> &data);
  std::expected<void, std::string> eraseBlock(uint32_t block,
                                              uint32_t count = 1);
  std::expected<void, std::string>
  dumpFlash(const std::string &outputPath, size_t dumpSize,
            std::function<void(size_t bytesRead, size_t totalSize)> progressCb =
                nullptr);

  // Power
  std::expected<void, std::string> reboot();
  std::expected<void, std::string> smcReboot();
  std::expected<void, std::string> shutdownConsole();
  std::expected<void, std::string> quit();

private:
  int socketFd_ = -1;
  std::string targetIp_;

  bool sendCommandString(const std::string &cmdStr);
  bool sendRawData(const void *data, size_t length);
  bool receiveRawData(void *data, size_t length);
  std::string receiveStringResponse();
};

} // namespace updclient
