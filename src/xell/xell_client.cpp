#include "xell/xell_client.hpp"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

namespace updclient::xell {

static bool parseHexBytes(const std::string &hexStr, uint8_t *outBuf, size_t outLen) {
  if (hexStr.length() < outLen * 2) return false;
  for (size_t i = 0; i < outLen; ++i) {
    std::string byteStr = hexStr.substr(i * 2, 2);
    char *endPtr = nullptr;
    outBuf[i] = static_cast<uint8_t>(std::strtoul(byteStr.c_str(), &endPtr, 16));
    if (endPtr != byteStr.c_str() + 2) return false;
  }
  return true;
}

bool XellClient::connect(const std::string &ipAddress, uint16_t port) {
  bool ok = tcpSocket_.connect(ipAddress, port, 5000);
  if (ok) {
    spdlog::info("Connected to XeLL HTTPD at {}:{}", ipAddress, port);
  }
  return ok;
}

void XellClient::disconnect() {
  tcpSocket_.disconnect();
}

expected<XellClient::HttpResponse, std::string>
XellClient::sendHttpGet(const std::string &path, bool streamToDisk,
                        const std::string &diskPath,
                        std::function<void(size_t, size_t)> progressCb) {
  if (!isConnected()) {
    return unexpected("Not connected to XeLL server");
  }

  std::string request = std::format("GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: UpdClient/2.0\r\nConnection: close\r\n\r\n",
                                    path, targetIp());
  if (!tcpSocket_.sendString(request)) {
    return unexpected("Failed to send HTTP GET request");
  }

  // 1. Read HTTP response status line & headers byte-by-byte or chunk until double CRLF
  std::string headersBuffer;
  char ch;
  while (tcpSocket_.recvRaw(&ch, 1)) {
    headersBuffer.push_back(ch);
    if (headersBuffer.size() >= 4 &&
        headersBuffer.substr(headersBuffer.size() - 4) == "\r\n\r\n") {
      break;
    }
  }

  if (headersBuffer.empty()) {
    return unexpected("Empty HTTP response from XeLL server");
  }

  HttpResponse resp{};
  resp.headersRaw = headersBuffer;

  // Parse HTTP Status Line
  std::istringstream stream(headersBuffer);
  std::string httpVer;
  stream >> httpVer >> resp.statusCode;
  std::getline(stream, resp.statusMessage);

  if (resp.statusCode != 200) {
    return unexpected(std::format("HTTP GET {} failed with status {}", path, resp.statusCode));
  }

  // Parse Header lines
  std::string headerLine;
  while (std::getline(stream, headerLine) && headerLine != "\r") {
    auto colonPos = headerLine.find(':');
    if (colonPos != std::string::npos) {
      std::string key = headerLine.substr(0, colonPos);
      std::string val = headerLine.substr(colonPos + 1);

      // Trim whitespace & \r
      key.erase(key.find_last_not_of(" \t\r\n") + 1);
      val.erase(0, val.find_first_not_of(" \t"));
      val.erase(val.find_last_not_of(" \t\r\n") + 1);

      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      if (key == "content-length") {
        resp.contentLength = std::strtoull(val.c_str(), nullptr, 10);
      } else if (key == "content-type") {
        resp.contentType = val;
      }
    }
  }

  // 2. Read body payload
  if (streamToDisk) {
    std::ofstream outFile(diskPath, std::ios::binary);
    if (!outFile) {
      return unexpected(std::format("Failed to open output file: {}", diskPath));
    }

    size_t totalReceived = 0;
    constexpr size_t CHUNK_SIZE = 16384;
    std::vector<uint8_t> chunk(CHUNK_SIZE);

    while (resp.contentLength == 0 || totalReceived < resp.contentLength) {
      size_t toRead = CHUNK_SIZE;
      if (resp.contentLength > 0) {
        toRead = std::min<size_t>(CHUNK_SIZE, resp.contentLength - totalReceived);
      }
      if (toRead == 0) break;

      if (!tcpSocket_.recvRaw(chunk.data(), toRead)) {
        if (resp.contentLength == 0 && totalReceived > 0) {
          // Socket closed naturally at EOF for unchunked stream
          break;
        }
        return unexpected("HTTP body download interrupted");
      }
      outFile.write(reinterpret_cast<const char *>(chunk.data()), toRead);
      totalReceived += toRead;
      if (progressCb) {
        progressCb(totalReceived, resp.contentLength);
      }
    }
  } else {
    if (resp.contentLength > 0) {
      resp.body.resize(resp.contentLength);
      if (!tcpSocket_.recvRaw(resp.body.data(), resp.contentLength)) {
        return unexpected("Failed to receive full HTTP body payload");
      }
    } else {
      // Read until EOF
      uint8_t buf[2048];
      while (true) {
        // Simple chunk read
        size_t read = 0;
        char c;
        if (tcpSocket_.recvRaw(&c, 1)) {
          resp.body.push_back(static_cast<uint8_t>(c));
        } else {
          break;
        }
      }
    }
  }

  return resp;
}

expected<XellInfo, std::string> XellClient::getInfo() {
  auto respRes = sendHttpGet("/");
  if (!respRes) {
    return unexpected(respRes.error());
  }

  std::string html(respRes->body.begin(), respRes->body.end());
  XellInfo info{};

  // XeLL HTML output contains 32-character hex strings for CPU key and DVD key
  // or formatted table cells: e.g. "0123456789ABCDEF0123456789ABCDEF"
  std::regex hexKeyRegex(R"(([0-9A-Fa-f]{32}))");
  std::smatch match;

  std::vector<std::string> keysFound;
  auto searchStart = html.cbegin();
  while (std::regex_search(searchStart, html.cend(), match, hexKeyRegex)) {
    keysFound.push_back(match[1].str());
    searchStart = match.suffix().first;
  }

  if (keysFound.size() >= 1) {
    if (parseHexBytes(keysFound[0], info.cpuKey.data(), 16)) {
      info.cpuKeyValid = true;
    }
  }
  if (keysFound.size() >= 2) {
    if (parseHexBytes(keysFound[1], info.dvdKey.data(), 16)) {
      info.dvdKeyValid = true;
    }
  }

  // Color extraction (e.g. #RRGGBB)
  std::regex colorRegex(R"(#(?:[0-9A-Fa-f]{6}))");
  std::vector<std::string> colorsFound;
  searchStart = html.cbegin();
  while (std::regex_search(searchStart, html.cend(), match, colorRegex)) {
    colorsFound.push_back(match.str());
    searchStart = match.suffix().first;
  }

  if (colorsFound.size() >= 1) info.bgColor = colorsFound[0];
  if (colorsFound.size() >= 2) info.fgColor = colorsFound[1];

  return info;
}

expected<void, std::string> XellClient::dumpFlash(
    const std::string &outputPath,
    std::function<void(size_t bytesRead, size_t totalSize)> progressCb) {
  auto respRes = sendHttpGet("/FLASH", true, outputPath, progressCb);
  if (!respRes) {
    return unexpected(respRes.error());
  }
  return {};
}

expected<std::string, std::string> XellClient::getFuses() {
  auto respRes = sendHttpGet("/FUSE");
  if (!respRes) {
    return unexpected(respRes.error());
  }
  return std::string(respRes->body.begin(), respRes->body.end());
}

expected<std::vector<uint8_t>, std::string>
XellClient::getKeyvault(KeyvaultMode mode) {
  std::string path;
  switch (mode) {
  case KeyvaultMode::Decrypted:
    path = "/KV";
    break;
  case KeyvaultMode::Raw:
    path = "/KVRAW";
    break;
  case KeyvaultMode::RawBlock:
    path = "/KVRAW2";
    break;
  }

  auto respRes = sendHttpGet(path);
  if (!respRes) {
    return unexpected(respRes.error());
  }
  return respRes->body;
}

} // namespace updclient::xell
