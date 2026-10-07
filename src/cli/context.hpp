#pragma once

#include "cli/output.hpp"

#include <updclient.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace updclient::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntime = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitDiscovery = 3;

// A command failure: what to report and which exit code to use.
struct Failure {
  std::string code;
  std::string message;
  int sysError = 0;
  int exitCode = kExitRuntime;
  // The 4xx status of an XBDM refusal; sysError is 0 then.
  int consoleStatus = 0;
  // An XBDM upload that failed but is kept on the console under this name.
  std::optional<std::string> keptUpload = std::nullopt;
  // How far the failed XBDM command got: "not_sent", or "unknown" when it may
  // have been carried out.
  std::optional<std::string> delivery = std::nullopt;
};

template <class T> using Outcome = expected<T, Failure>;

int exitCodeFor(ErrorCode code) noexcept;
unexpected<Failure> failWith(int exitCode, std::string code, std::string message);
unexpected<Failure> usageError(std::string message);
unexpected<Failure> fromError(const Error &error);

struct GlobalOptions {
  std::string target;
  std::string trace;
  std::string ip;
  std::optional<uint16_t> port;
  std::optional<uint16_t> xellPort;
  std::optional<uint32_t> timeoutMs;
  uint32_t discoveryTimeoutMs = 3000;
  bool json = false;
  bool verbose = false;
  bool yes = false;
};

// Which kind of console a command talks to; it decides the default port.
enum class Service { UpdServer, Xell, Xbdm };

// State shared by every command: global options, stdout rendering and the exit code.
class Context {
public:
  GlobalOptions options;
  Output output;
  int exitCode = kExitOk;

  // Applies options that must take effect before any command runs.
  void applyGlobals();

  // The endpoint named by --target or --ip, with --port / --xell-port and
  // --timeout-ms applied. Empty when neither was given.
  bool hasExplicitTarget() const noexcept { return !options.target.empty() || !options.ip.empty(); }
  Outcome<net::Endpoint> explicitEndpoint(Service service) const;
  // --target names an xbdm:// endpoint: the file, mem, power and info commands
  // then speak XBDM instead of UpdServer.
  bool targetsXbdm() const;

  // Explicit target, or the first UpdServer console found by discovery.
  Outcome<net::Endpoint> resolveUpdServerEndpoint() const;
  // XeLL is never auto-discovered: --target or --ip is required.
  Outcome<net::Endpoint> resolveXellEndpoint() const;
  // Explicit target (a bare host means xbdm://), or the first console that answers
  // XBDM discovery.
  Outcome<net::Endpoint> resolveXbdmEndpoint() const;

  // Fails right away when a destructive command could not be confirmed at all.
  Outcome<void> requireConfirmationPossible() const;
  // Passes with --yes, or when the user types "yes" at an interactive prompt.
  Outcome<void> confirmDestructive(const std::string &action, const std::string &target) const;

  // Reports a failure (stderr log, JSON error document) and records its exit code.
  void finish(const Outcome<void> &outcome);
};

} // namespace updclient::cli
