#include <protocols/updserver/discovery.hpp>

#include <net/udp_socket.hpp>

#include "net/deadline.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace updclient::updserver {

namespace {

constexpr std::chrono::milliseconds kPollSlice{200};

static_assert(offsetof(UdpBcastMsg, magic) == 0 && offsetof(UdpBcastMsg, ipAddr) == 4);

struct Announcement {
  uint8_t octets[4];
};

// The magic is compared once, as the big-endian value of the first four wire bytes.
std::optional<Announcement> parseAnnouncement(const std::vector<uint8_t> &data) {
  if (data.size() != sizeof(UdpBcastMsg)) return std::nullopt;
  if (loadBe32(data.data() + offsetof(UdpBcastMsg, magic)) != CMD_MAGIC_BE) return std::nullopt;
  Announcement announcement{};
  std::copy_n(data.data() + offsetof(UdpBcastMsg, ipAddr), sizeof(announcement.octets), announcement.octets);
  return announcement;
}

std::string bindFailureMessage(uint16_t port, const Error &cause) {
  std::string text = "UpdServer discovery cannot bind UDP port " + std::to_string(port) + ": " + cause.message;
  if (port < 1024) {
    text += "; port " + std::to_string(port) +
            " is privileged on Linux (run as root, grant CAP_NET_BIND_SERVICE, or lower "
            "net.ipv4.ip_unprivileged_port_start), and it must not be in use by another process";
  }
  return text;
}

} // namespace

UpdServerDiscovery::UpdServerDiscovery(net::DatagramSocketFactory socketFactory, uint16_t announcePort)
    : socketFactory_(socketFactory ? std::move(socketFactory) : net::DatagramSocketFactory(net::makeUdpSocket)),
      announcePort_(announcePort) {}

std::string UpdServerDiscovery::name() const {
  return "updserver";
}

Result<std::vector<discovery::DiscoveredDevice>>
UpdServerDiscovery::discover(std::chrono::milliseconds timeout, bool stopAfterFirst) {
  auto socket = socketFactory_();
  if (!socket) return fail(ErrorCode::Unknown, "datagram socket factory returned no socket");
  if (auto r = socket->bind(announcePort_, true); !r) {
    Error error = r.error();
    error.message = bindFailureMessage(announcePort_, error);
    return unexpected<Error>(std::move(error));
  }

  spdlog::debug("listening for UpdServer broadcasts on UDP port {}", announcePort_);

  std::vector<discovery::DiscoveredDevice> devices;
  std::set<std::string> seen;

  const auto deadline = net::deadlineAfter(timeout);
  // Always make at least one receive attempt, even for a zero timeout.
  for (bool first = true;; first = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!first && now >= deadline) break;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto slice = std::clamp(remaining, std::chrono::milliseconds(0), kPollSlice);

    auto packet = socket->receive(slice);
    if (!packet) {
      if (!devices.empty()) {
        spdlog::warn("UpdServer discovery stopped early: {}", formatError(packet.error()));
        break;
      }
      return unexpected<Error>(packet.error());
    }
    if (!*packet) continue;

    const auto &datagram = **packet;
    auto announcement = parseAnnouncement(datagram.data);
    if (!announcement) {
      spdlog::debug("ignoring {} byte datagram from {} that is not an UpdServer announcement",
                    datagram.data.size(), datagram.senderAddress);
      continue;
    }
    if (!seen.insert(datagram.senderAddress).second) continue;

    discovery::DiscoveredDevice device;
    device.protocol = "updserver";
    device.address = datagram.senderAddress;
    device.info["port"] = std::to_string(NANDSVR_PORT);
    const auto &o = announcement->octets;
    if (o[0] != 0 || o[1] != 0 || o[2] != 0 || o[3] != 0) {
      device.info["announcedAddress"] = std::to_string(o[0]) + "." + std::to_string(o[1]) + "." +
                                        std::to_string(o[2]) + "." + std::to_string(o[3]);
    }
    device.lastSeen = std::chrono::system_clock::now();
    spdlog::debug("discovered UpdServer console at {}", device.address);
    devices.push_back(std::move(device));

    if (stopAfterFirst) break;
  }

  return devices;
}

void registerUpdServerDiscovery(discovery::DiscoveryRegistry &registry) {
  registry.add(std::make_unique<UpdServerDiscovery>());
}

} // namespace updclient::updserver
