#pragma once

#include "cli/output.hpp"

#include <updclient/updclient.hpp>

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
};

template <class T> using Outcome = expected<T, Failure>;

int exitCodeFor(ErrorCode code) noexcept;
unexpected<Failure> failWith(int exitCode, std::string code, std::string message);
unexpected<Failure> usageError(std::string message);
unexpected<Failure> fromError(const Error &error);

struct GlobalOptions {
  std::string target;
  std::string ip;
  std::optional<uint16_t> port;
  std::optional<uint16_t> xellPort;
  std::optional<uint32_t> timeoutMs;
  uint32_t discoveryTimeoutMs = 3000;
  bool json = false;
  bool verbose = false;
  bool yes = false;
};

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
  Outcome<net::Endpoint> explicitEndpoint(bool forXell) const;

  // Explicit target, or the first UpdServer console found by discovery.
  Outcome<net::Endpoint> resolveUpdServerEndpoint() const;
  // XeLL is never auto-discovered: --target or --ip is required.
  Outcome<net::Endpoint> resolveXellEndpoint() const;

  // Fails right away when a destructive command could not be confirmed at all.
  Outcome<void> requireConfirmationPossible() const;
  // Passes with --yes, or when the user types "yes" at an interactive prompt.
  Outcome<void> confirmDestructive(const std::string &action, const std::string &target) const;

  // Reports a failure (stderr log, JSON error document) and records its exit code.
  void finish(const Outcome<void> &outcome);
};

} // namespace updclient::cli
