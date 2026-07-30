#include "xell/xell_discovery.hpp"
#include "network/tcp_socket.hpp"
#include <spdlog/spdlog.h>

namespace updclient::xell {

bool XellDiscovery::probeConsole(const std::string &ipAddress, uint16_t port, int timeoutMs) {
  network::TcpSocket socket;
  if (!socket.connect(ipAddress, port, timeoutMs)) {
    return false;
  }

  std::string probeReq = "GET / HTTP/1.0\r\nUser-Agent: UpdClient-Probe\r\n\r\n";
  if (!socket.sendString(probeReq)) {
    return false;
  }

  std::string response = socket.recvString(512);
  socket.disconnect();

  if (response.find("200 OK") != std::string::npos ||
      response.find("XeLL") != std::string::npos) {
    spdlog::debug("Detected XeLL HTTPD server at {}:{}", ipAddress, port);
    return true;
  }

  return false;
}

} // namespace updclient::xell
