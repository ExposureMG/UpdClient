#pragma once

#include "cpp_compat.hpp"
#include "network/tcp_socket.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace updclient::xell {

struct XellInfo {
  std::array<uint8_t, 16> cpuKey{};
  std::array<uint8_t, 16> dvdKey{};
  std::string bgColor;
  std::string fgColor;
  bool cpuKeyValid = false;
  bool dvdKeyValid = false;
};

enum class KeyvaultMode {
  Decrypted, // /KV
  Raw,       // /KVRAW
  RawBlock   // /KVRAW2
};

class UPDCLIENT_API XellClient {
public:
  XellClient() = default;
  ~XellClient() = default;

  bool connect(const std::string &ipAddress, uint16_t port = 80);
  void disconnect();
  bool isConnected() const noexcept { return tcpSocket_.isConnected(); }
  const std::string &targetIp() const noexcept { return tcpSocket_.targetIp(); }

  expected<XellInfo, std::string> getInfo();

  expected<void, std::string>
  dumpFlash(const std::string &outputPath,
            std::function<void(size_t bytesRead, size_t totalSize)> progressCb = nullptr);

  expected<std::string, std::string> getFuses();

  expected<std::vector<uint8_t>, std::string>
  getKeyvault(KeyvaultMode mode = KeyvaultMode::Decrypted);

private:
  network::TcpSocket tcpSocket_;

  struct HttpResponse {
    int statusCode = 0;
    std::string statusMessage;
    size_t contentLength = 0;
    std::string contentType;
    std::string headersRaw;
    std::vector<uint8_t> body;
  };

  expected<HttpResponse, std::string> sendHttpGet(const std::string &path,
                                                  bool streamToDisk = false,
                                                  const std::string &diskPath = {},
                                                  std::function<void(size_t, size_t)> progressCb = nullptr);
};

} // namespace updclient::xell
