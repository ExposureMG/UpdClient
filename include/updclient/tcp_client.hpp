#pragma once

#include "updclient/protocol.hpp"
#include "updclient/expected.hpp"
#include "updclient/net_compat.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace updclient {

class UPDCLIENT_API TcpClient {
public:
  TcpClient() = default;
  ~TcpClient();

  bool connect(const std::string &ipAddress, uint16_t port = NANDSVR_PORT);
  void disconnect();
  bool isConnected() const noexcept { return socketFd_ != INVALID_SOCKET_FD; }
  const std::string &targetIp() const noexcept { return targetIp_; }

  // Core Command Methods
  expected<NandInfo, std::string> getInfo();
  expected<std::string, std::string> getVersion();
  expected<std::vector<uint16_t>, std::string> getBadBlockList();

  // Memory & Hardware
  expected<std::vector<uint8_t>, std::string> peek(uint32_t addr,
                                                         uint32_t len);
  expected<void, std::string> poke(uint32_t addr, uint32_t value);
  expected<std::vector<uint8_t>, std::string> hvPeek(uint64_t addr,
                                                          uint32_t len);
  expected<void, std::string> hvPoke(uint64_t addr, uint64_t value);
  expected<std::vector<uint8_t>, std::string> get1bl();
  expected<std::vector<uint8_t>, std::string> getBootloaders();

  // File & Storage
  expected<void, std::string>
  getFile(const std::string &remotePath, const std::string &localPath,
          std::function<void(size_t bytesRead)> progressCb = nullptr);
  expected<void, std::string>
  sendFile(const std::string &localPath, const std::string &remotePath,
           std::function<void(size_t bytesSent)> progressCb = nullptr);
  expected<void, std::string> mount(const std::string &mountPoint,
                                         const std::string &devicePath);
  expected<void, std::string> unmount(const std::string &mountPoint);
  expected<void, std::string> mkDir(const std::string &remotePath);

  // NAND Operations
  expected<std::vector<uint8_t>, std::string>
  readBlock(uint32_t block, uint32_t count = 1);
  expected<void, std::string> writeBlock(uint32_t block,
                                              const std::vector<uint8_t> &data);
  expected<void, std::string> eraseBlock(uint32_t block,
                                              uint32_t count = 1);
  expected<void, std::string>
  dumpFlash(const std::string &outputPath, size_t dumpSize,
            std::function<void(size_t bytesRead, size_t totalSize)> progressCb =
                nullptr);

  // Power
  expected<void, std::string> reboot();
  expected<void, std::string> smcReboot();
  expected<void, std::string> shutdownConsole();
  expected<void, std::string> quit();

private:
  socket_t socketFd_ = INVALID_SOCKET_FD;
  std::string targetIp_;

  bool sendCommandString(const std::string &cmdStr);
  bool sendRawData(const void *data, size_t length);
  bool receiveRawData(void *data, size_t length);
  std::string receiveStringResponse();
};

} // namespace updclient
