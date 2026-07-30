#pragma once

#include "cpp_compat.hpp"
#include "updserver/protocol.hpp"
#include "network/tcp_socket.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace updclient::updserver {

class UPDCLIENT_API UpdServerClient {
public:
  UpdServerClient() = default;
  ~UpdServerClient() = default;

  bool connect(const std::string &ipAddress, uint16_t port = NANDSVR_PORT);
  void disconnect();
  bool isConnected() const noexcept { return tcpSocket_.isConnected(); }
  const std::string &targetIp() const noexcept { return tcpSocket_.targetIp(); }

  // Core Command Methods
  expected<NandInfo, std::string> getInfo();
  expected<std::string, std::string> getVersion();
  expected<std::vector<uint16_t>, std::string> getBadBlockList();

  // Memory & Hardware
  expected<std::vector<uint8_t>, std::string> peek(uint32_t addr, uint32_t len);
  expected<void, std::string> poke(uint32_t addr, uint32_t value);
  expected<std::vector<uint8_t>, std::string> hvPeek(uint64_t addr, uint32_t len);
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
  expected<std::vector<uint8_t>, std::string> readBlock(uint32_t block,
                                                        uint32_t count = 1);
  expected<void, std::string> writeBlock(uint32_t block,
                                         const std::vector<uint8_t> &data);
  expected<void, std::string> eraseBlock(uint32_t block, uint32_t count = 1);
  expected<void, std::string>
  dumpFlash(const std::string &outputPath, size_t dumpSize,
            std::function<void(size_t bytesRead, size_t totalSize)> progressCb = nullptr);

  // Power
  expected<void, std::string> reboot();
  expected<void, std::string> smcReboot();
  expected<void, std::string> shutdownConsole();
  expected<void, std::string> quit();

private:
  network::TcpSocket tcpSocket_;
};

} // namespace updclient::updserver
