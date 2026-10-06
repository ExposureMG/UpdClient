# Extending UpdClient

Three things vary independently, and each has its own extension point:

| To add | Implement | Registered with | Files |
| --- | --- | --- | --- |
| A transport (serial, USB, TLS, ...) | `net::ITransport` | `net::TransportRegistry`, by URI scheme | `include/updclient/net/<name>_transport.hpp`, `src/net/<name>_transport.cpp` |
| A protocol client | a class that takes a `net::TransportPtr` | nothing: callers construct it | `include/updclient/protocols/<name>/client.hpp`, `src/protocols/<name>/client.cpp` |
| A discovery provider | `discovery::IDiscoveryProvider` | `discovery::DiscoveryRegistry`, by name | `include/updclient/protocols/<name>/discovery.hpp`, `src/protocols/<name>/discovery.cpp` |

Sources under `src/` are globbed by CMake (`CONFIGURE_DEPENDS`), so a new `.cpp` needs no CMake edit; re-run
CMake once if a build directory already exists. Tests under `tests/` are globbed the same way. A new
dependency (a TLS or USB library) is the exception: link it in `CMakeLists.txt`.

Read [ARCHITECTURE.md](ARCHITECTURE.md) first for the dependency rules. In short: headers under `include/`
include only `<updclient/...>` and the standard library, never OS socket headers or spdlog; spdlog and
platform code live in `.cpp` files and private headers under `src/`; protocols never include each other.

The examples below use made-up names (`serial`, `acme`). The transport, client and discovery examples
and their tests were compiled against the library and run with a stubbed serial port and the repository's
test fakes; the CLI example was compiled against the CLI headers. The only code not shown is the per-OS
serial port implementation.

Conventions: C++20 only, `#pragma once`, two-space indent, camelCase functions and methods, PascalCase
types, `Result<T>` for every fallible call, no exceptions for expected failures, `UPDCLIENT_API` on every
exported class and free function.

## 1. Add a transport

A transport is a reliable ordered byte stream. Implement the six primitives of `net::ITransport`; the
base class already provides `writeAll`, `readExact` and `readUntilEof`.

Contract:

- `readSome` returns the number of bytes read, and 0 only for an orderly end of stream. A read that
  waits too long fails with `ErrorCode::Timeout`. A medium with no concept of end of stream (a serial
  line) never returns 0 while open.
- `writeSome` returns the number of bytes accepted. Returning 0 for a non-empty buffer makes `writeAll`
  fail with `ErrorCode::Io`.
- `setTimeout(0)` disables the timeout. `Endpoint::timeout` is the initial value.
- `close()` is idempotent and `noexcept`; after it, `isOpen()` is false and reads and writes fail with
  `ErrorCode::NotConnected`.
- Translate OS errors into `ErrorCode` values (`ConnectFailed` when opening, `Timeout`, `Disconnected`,
  `Io`). Put the OS error number in `Error::sysError`.
- Read device-specific settings from `Endpoint::options`; reject bad ones with `InvalidArgument`.
- Protocol clients call `TransportRegistry::withDefaultPort` before connecting. For `tcp` that fills in
  the protocol's port (UpdServer 49, XeLL 80); a scheme registered without `SchemeTraits` is passed
  `Endpoint::port == 0` untouched, which is what a serial or USB transport wants. A scheme that has a
  fixed default port (say `tls` on 443) registers it with `SchemeTraits::defaultPort`; one that carries
  the protocol's own port sets `usesProtocolPort`.
- `Endpoint::timeout` is at most `Endpoint::kMaxTimeout`. When computing a deadline from a timeout that
  came from a caller, do not add it to `steady_clock::now()` unchecked; clamp it first.

### Step 1: the public header

`include/updclient/net/serial_transport.hpp`

```cpp
#pragma once

#include <updclient/core/export.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/transport.hpp>
#include <updclient/net/transport_registry.hpp>

#include <memory>
#include <string>

namespace updclient::net {

// serial:///dev/ttyUSB0?baud=115200   or   serial://COM3?baud=115200
class UPDCLIENT_API SerialTransport final : public ITransport {
public:
  static Result<TransportPtr> open(const Endpoint &endpoint);

  ~SerialTransport() override;

  bool isOpen() const noexcept override;
  void close() noexcept override;
  std::string describe() const override;
  Result<void> setTimeout(std::chrono::milliseconds timeout) override;
  Result<size_t> readSome(std::span<uint8_t> buffer) override;
  Result<size_t> writeSome(std::span<const uint8_t> data) override;

private:
  struct Impl;
  explicit SerialTransport(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// Registers the "serial" scheme. Not part of registerBuiltins().
UPDCLIENT_API void registerSerialTransport(TransportRegistry &registry = TransportRegistry::instance());

} // namespace updclient::net
```

The `Impl` idiom keeps the OS types out of the header, as `TcpTransport` does.

### Step 2: the private platform wrapper

Everything that differs per operating system goes behind a private header next to the sources, the way
`src/net/platform/socket_platform.hpp` does for sockets. It is never included from `include/`.

`src/net/serial/serial_port.hpp`

```cpp
#pragma once

// Private to the library. Wraps the operating system's serial API (termios on
// POSIX, CreateFile/SetCommState on Windows); nothing outside src/ includes it.

#include <updclient/core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace updclient::net::serial {

class SerialPort {
public:
  static Result<SerialPort> open(const std::string &device, unsigned baud);

  SerialPort(SerialPort &&) noexcept;
  SerialPort &operator=(SerialPort &&) noexcept;
  ~SerialPort();

  bool isOpen() const noexcept;
  void close() noexcept;
  // Zero disables the timeout.
  Result<void> setReadTimeout(std::chrono::milliseconds timeout);
  // Fails with ErrorCode::Timeout when nothing arrives in time.
  Result<size_t> read(std::span<uint8_t> buffer);
  Result<size_t> write(std::span<const uint8_t> data);

private:
  struct State;
  explicit SerialPort(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

} // namespace updclient::net::serial
```

Implement it in `src/net/serial/serial_port_posix.cpp` (termios) and `src/net/serial/serial_port_win32.cpp`
(`CreateFile`, `SetCommState`, `SetCommTimeouts`). Because CMake globs every `.cpp`, wrap each file in
`#if defined(_WIN32)` / `#if !defined(_WIN32)` so that exactly one of them compiles to something on a
given platform.

### Step 3: the transport

`src/net/serial_transport.cpp`

```cpp
#include <updclient/net/serial_transport.hpp>

#include "net/serial/serial_port.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace updclient::net {

struct SerialTransport::Impl {
  serial::SerialPort port;
  std::string device;
};

SerialTransport::SerialTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SerialTransport::~SerialTransport() = default;

Result<TransportPtr> SerialTransport::open(const Endpoint &endpoint) {
  unsigned baud = 115200;
  if (auto it = endpoint.options.find("baud"); it != endpoint.options.end()) {
    const std::string &text = it->second;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), baud);
    if (ec != std::errc{} || end != text.data() + text.size() || baud == 0) {
      return fail(ErrorCode::InvalidArgument, "invalid baud rate '" + text + "'");
    }
  }

  auto port = serial::SerialPort::open(endpoint.host, baud);
  if (!port) return unexpected<Error>(port.error());
  if (auto r = port->setReadTimeout(endpoint.timeout); !r) return unexpected<Error>(r.error());

  auto impl = std::make_unique<Impl>(Impl{std::move(*port), endpoint.host});
  return TransportPtr(new SerialTransport(std::move(impl)));
}

bool SerialTransport::isOpen() const noexcept {
  return impl_->port.isOpen();
}

void SerialTransport::close() noexcept {
  impl_->port.close();
}

std::string SerialTransport::describe() const {
  return "serial://" + impl_->device;
}

Result<void> SerialTransport::setTimeout(std::chrono::milliseconds timeout) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidArgument, "negative timeout");
  return impl_->port.setReadTimeout(timeout);
}

Result<size_t> SerialTransport::readSome(std::span<uint8_t> buffer) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  return impl_->port.read(buffer);
}

Result<size_t> SerialTransport::writeSome(std::span<const uint8_t> data) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  return impl_->port.write(data);
}

void registerSerialTransport(TransportRegistry &registry) {
  registry.registerScheme("serial", &SerialTransport::open);
}

} // namespace updclient::net
```

### Step 4: registration

`registerSerialTransport()` is a separate function and is deliberately not called by static
initialisation. An application calls it next to `registerBuiltins()`:

```cpp
updclient::registerBuiltins();
updclient::net::registerSerialTransport();

auto endpoint = updclient::net::Endpoint::parse("serial:///dev/ttyUSB0?baud=115200");
auto client = updclient::updserver::UpdServerClient::connect(*endpoint);  // runs over the serial line
```

Nothing in `UpdServerClient`, `XellClient` or `http_lite` changes: they only see an `ITransport`.

To make the CLI accept `--target serial:///dev/ttyUSB0`, add one line after `registerBuiltins();` in
`src/cli/app.cpp` (and `#include <updclient/net/serial_transport.hpp>`). The CLI rejects schemes that
are not registered, listing the ones that are. To make it a built-in for every user of the library, put
the same call inside the `std::call_once` lambda in `src/updclient.cpp` and add the header to
`include/updclient/updclient.hpp`.

### Step 5: tests

Real hardware is not needed to test argument handling and registration.
`tests/net/serial_transport_test.cpp`

```cpp
#include "support/test_harness.hpp"

#include <updclient/net/serial_transport.hpp>
#include <updclient/net/transport_registry.hpp>

using namespace updclient;

TEST(SerialTransport, RegistersItsScheme) {
  net::TransportRegistry registry;
  net::registerSerialTransport(registry);
  CHECK_EQ(registry.schemes(), std::vector<std::string>{"serial"});
}

TEST(SerialTransport, RejectsABadBaudRateBeforeOpeningTheDevice) {
  auto endpoint = net::Endpoint::parse("serial:///dev/ttyUSB0?baud=abc");
  REQUIRE_OK(endpoint);
  CHECK_ERR(net::SerialTransport::open(*endpoint), ErrorCode::InvalidArgument);
}
```

The scripted `ut::MockTransport` (`tests/support/mock_transport.hpp`) is for testing code that uses a
transport, not the transport itself. Test the platform wrapper against a pty or a loopback adapter and
mark such tests with `SKIP("no device")` when the device is absent.

## 2. Add a protocol client

A protocol client takes its byte stream by injection and never constructs a socket. This one speaks an
invented protocol: the line `STATUS\n` is answered by a big-endian 32-bit word.

`include/updclient/protocols/acme/client.hpp`

```cpp
#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/transport.hpp>

#include <cstdint>
#include <string>

namespace updclient::acme {

inline constexpr uint16_t kAcmePort = 4242;

// Hypothetical protocol: "STATUS\n" is answered with a big-endian uint32.
// The transport is injected, so any net::ITransport can carry it. Not thread-safe.
class UPDCLIENT_API AcmeClient {
public:
  explicit AcmeClient(net::TransportPtr transport);
  ~AcmeClient();

  AcmeClient(AcmeClient &&) noexcept;
  AcmeClient &operator=(AcmeClient &&) noexcept;
  AcmeClient(const AcmeClient &) = delete;
  AcmeClient &operator=(const AcmeClient &) = delete;

  // An empty scheme selects tcp; port 0 selects kAcmePort for schemes that use the protocol port.
  static Result<AcmeClient> connect(const net::Endpoint &endpoint);

  bool isConnected() const noexcept;
  void disconnect() noexcept;

  Result<uint32_t> status();

private:
  net::TransportPtr transport_;
};

} // namespace updclient::acme
```

`src/protocols/acme/client.cpp`

```cpp
#include <updclient/protocols/acme/client.hpp>

#include <updclient/net/transport_registry.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <string_view>
#include <utility>

namespace updclient::acme {

AcmeClient::AcmeClient(net::TransportPtr transport) : transport_(std::move(transport)) {}

AcmeClient::~AcmeClient() = default;
AcmeClient::AcmeClient(AcmeClient &&) noexcept = default;
AcmeClient &AcmeClient::operator=(AcmeClient &&) noexcept = default;

Result<AcmeClient> AcmeClient::connect(const net::Endpoint &endpoint) {
  net::Endpoint target = endpoint;
  if (target.scheme.empty()) target.scheme = "tcp";

  auto &registry = net::TransportRegistry::instance();
  auto transport = registry.connect(registry.withDefaultPort(target, kAcmePort));
  if (!transport) return unexpected<Error>(transport.error());
  if (!*transport) return fail(ErrorCode::ConnectFailed, "transport registry returned no transport");
  spdlog::debug("connected to acme device at {}", (*transport)->describe());
  return AcmeClient(std::move(*transport));
}

bool AcmeClient::isConnected() const noexcept {
  return transport_ && transport_->isOpen();
}

void AcmeClient::disconnect() noexcept {
  if (transport_) transport_->close();
}

Result<uint32_t> AcmeClient::status() {
  if (!isConnected()) return fail(ErrorCode::NotConnected, "not connected to an acme device");

  constexpr std::string_view kRequest = "STATUS\n";
  const auto *bytes = reinterpret_cast<const uint8_t *>(kRequest.data());
  if (auto r = transport_->writeAll({bytes, kRequest.size()}); !r) {
    transport_->close();
    return unexpected<Error>(r.error());
  }

  std::array<uint8_t, 4> reply{};
  if (auto r = transport_->readExact(reply); !r) {
    transport_->close();
    return unexpected<Error>(r.error());
  }
  return (uint32_t{reply[0]} << 24) | (uint32_t{reply[1]} << 16) | (uint32_t{reply[2]} << 8) | reply[3];
}

} // namespace updclient::acme
```

Guidelines taken from `UpdServerClient`:

- Use `writeAll` and `readExact`; check every length that comes from the peer against a limit before
  allocating (`ErrorCode::LimitExceeded`). `UpdServerClient` exposes its limits as `ClientLimits`.
- A protocol without framing cannot resynchronise after a failed exchange, so close the transport on
  failure as above. Errors raised before anything is sent (invalid arguments) should leave the
  connection usable.
- State plainly in the header which commands the device does not acknowledge.
- Keep wire constants and packed structures in `protocol.hpp`, with no I/O, as
  `include/updclient/protocols/updserver/protocol.hpp` does, and `static_assert` their sizes.
- Do not include another protocol's headers. Shared code belongs in `core/` or `net/`.
- Write downloads to a temporary file and rename on success (see `UpdServerClient::getFile`).

If the device closes the connection after every exchange, as XeLL does over HTTP, take a connector
instead of a transport and open one per request. `XellClient` does exactly this; for HTTP devices reuse
`net/http_lite.hpp`:

```cpp
#include <updclient/net/http_lite.hpp>
#include <updclient/net/transport.hpp>

#include <functional>
#include <string>

using namespace updclient;

Result<std::string> fetchText(const std::function<Result<net::TransportPtr>()> &connector,
                              const std::string &path) {
  auto transport = connector();
  if (!transport) return unexpected<Error>(transport.error());

  auto response = net::httpGet(**transport, net::HttpRequest("device", path));
  if (!response) return unexpected<Error>(response.error());
  return std::string(response->body.begin(), response->body.end());
}
```

Tests use `ut::MockScript` to script the device. `tests/protocols/acme/client_test.cpp`

```cpp
#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"

#include <updclient/protocols/acme/client.hpp>

using namespace updclient;
using ut::MockScript;

TEST(AcmeClient, StatusDecodesABigEndianWord) {
  auto script = MockScript::create();
  script->expectWrite("STATUS\n").reply(ut::be32(0x00000102));
  acme::AcmeClient client(script->transport());

  auto status = client.status();
  REQUIRE_OK(status);
  CHECK_EQ(*status, uint32_t{0x102});
  CHECK_EQ(script->problems(), std::string());
}

TEST(AcmeClient, ShortReplyClosesTheConnection) {
  auto script = MockScript::create();
  script->reply(ut::be16(1));
  acme::AcmeClient client(script->transport());

  CHECK_ERR(client.status(), ErrorCode::Disconnected);
  CHECK(!client.isConnected());
}
```

The umbrella header `include/updclient/updclient.hpp` lists every public header; adding the new ones is
optional but keeps `#include <updclient/updclient.hpp>` sufficient for library users.

## 3. Add a discovery provider

A provider implements `discovery::IDiscoveryProvider`. A datagram-based one takes the socket factory as
a constructor argument so tests can inject `ut::FakeDatagrams`. It must return as soon as it finds one
device when `stopAfterFirst` is set, and must report a failure (such as a bind error) as an `Error`, not
as an empty successful result.

`include/updclient/protocols/acme/discovery.hpp`

```cpp
#pragma once

#include <updclient/core/export.hpp>
#include <updclient/discovery/discovery.hpp>
#include <updclient/net/datagram.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace updclient::acme {

inline constexpr uint16_t kAcmeAnnouncePort = 4243;

// Listens for UDP datagrams that start with "ACME".
class UPDCLIENT_API AcmeDiscovery final : public discovery::IDiscoveryProvider {
public:
  // An empty factory selects the real UDP socket.
  explicit AcmeDiscovery(net::DatagramSocketFactory socketFactory = {},
                         uint16_t announcePort = kAcmeAnnouncePort);

  std::string name() const override;
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                            bool stopAfterFirst) override;

private:
  net::DatagramSocketFactory socketFactory_;
  uint16_t announcePort_;
};

UPDCLIENT_API void registerAcmeDiscovery(
    discovery::DiscoveryRegistry &registry = discovery::DiscoveryRegistry::instance());

} // namespace updclient::acme
```

`src/protocols/acme/discovery.cpp`

```cpp
#include <updclient/protocols/acme/discovery.hpp>

#include <updclient/net/udp_socket.hpp>
#include <updclient/protocols/acme/client.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace updclient::acme {

namespace {

constexpr std::chrono::milliseconds kPollSlice{200};
constexpr std::string_view kMagic = "ACME";

bool isAnnouncement(const std::vector<uint8_t> &data) {
  return data.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), data.begin());
}

} // namespace

AcmeDiscovery::AcmeDiscovery(net::DatagramSocketFactory socketFactory, uint16_t announcePort)
    : socketFactory_(socketFactory ? std::move(socketFactory) : net::DatagramSocketFactory(net::makeUdpSocket)),
      announcePort_(announcePort) {}

std::string AcmeDiscovery::name() const {
  return "acme";
}

Result<std::vector<discovery::DiscoveredDevice>>
AcmeDiscovery::discover(std::chrono::milliseconds timeout, bool stopAfterFirst) {
  auto socket = socketFactory_();
  if (!socket) return fail(ErrorCode::Unknown, "datagram socket factory returned no socket");
  if (auto r = socket->bind(announcePort_, true); !r) return unexpected<Error>(r.error());

  std::vector<discovery::DiscoveredDevice> devices;
  std::set<std::string> seen;
  const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, net::Endpoint::kMaxTimeout);

  for (bool first = true;; first = false) {
    const auto now = std::chrono::steady_clock::now();
    if (!first && now >= deadline) break;
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto slice = std::clamp(remaining, std::chrono::milliseconds(0), kPollSlice);

    auto packet = socket->receive(slice);
    if (!packet) {
      if (!devices.empty()) break;
      return unexpected<Error>(packet.error());
    }
    if (!*packet) continue;

    const net::Datagram &datagram = **packet;
    if (!isAnnouncement(datagram.data) || !seen.insert(datagram.senderAddress).second) continue;

    discovery::DiscoveredDevice device;
    device.protocol = "acme";
    device.address = datagram.senderAddress;
    device.info["port"] = std::to_string(kAcmePort);
    device.lastSeen = std::chrono::system_clock::now();
    devices.push_back(std::move(device));

    if (stopAfterFirst) break;
  }
  return devices;
}

void registerAcmeDiscovery(discovery::DiscoveryRegistry &registry) {
  registry.add(std::make_unique<AcmeDiscovery>());
}

} // namespace updclient::acme
```

A provider that must send first (a directed or broadcast probe, SSDP, mDNS) binds with
`socket->bindWith(options)` instead of `bind`, setting `DatagramBindOptions::broadcast`,
`multicastGroup`, a bind `address` or `family = AddressFamily::IPv6` as needed, and transmits with
`socket->sendTo(payload, "255.255.255.255", port)`; `receive` then collects the replies. A socket that
does not implement these (such as a test fake) reports `ErrorCode::Unsupported`, so a fake for such a
provider must override `bindWith` and `sendTo`.

Providers that are not datagram based (mDNS, serial probing, a fixed candidate list like
`xell::XellProbeProvider`) implement only `IDiscoveryProvider` and need no socket abstraction.

Register it with `acme::registerAcmeDiscovery()` next to `registerBuiltins()`, or add the call to
`src/updclient.cpp` to make it built in. After that `updclient discover` lists its devices with no CLI
change, because `discover` runs every registered provider and prints `protocol`, `address`, `info` and
`last_seen`. Automatic target selection is different: `Context::resolveUpdServerEndpoint` in
`src/cli/context.cpp` only uses devices whose `protocol` is `"updserver"`.

`tests/protocols/acme/discovery_test.cpp`

```cpp
#include "support/fake_datagram_socket.hpp"
#include "support/test_harness.hpp"

#include <updclient/protocols/acme/discovery.hpp>

using namespace updclient;
using namespace std::chrono_literals;

TEST(AcmeDiscovery, StopsAfterTheFirstDeviceWhenAsked) {
  auto fake = ut::FakeDatagrams::create();
  fake->push(ut::bytesOf("ACME1"), "10.0.0.7").push(ut::bytesOf("ACME1"), "10.0.0.8");
  acme::AcmeDiscovery discovery(fake->factory());

  auto found = discovery.discover(60ms, true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("10.0.0.7"));
  CHECK_EQ((*found)[0].protocol, std::string("acme"));
}

TEST(AcmeDiscovery, IgnoresOtherTraffic) {
  auto fake = ut::FakeDatagrams::create();
  fake->push(ut::bytesOf("hello"), "10.0.0.9");
  acme::AcmeDiscovery discovery(fake->factory());

  auto found = discovery.discover(60ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}
```

## 4. Add a CLI command

The CLI is the one place that depends on CLI11 and nlohmann_json. A command group is one function that
declares subcommands and binds callbacks to the shared `Context`.

1. Create `src/cli/acme.cpp` (globbed into the `updclient` target).

```cpp
#include "cli/args.hpp"
#include "cli/commands.hpp"

#include <updclient/protocols/acme/client.hpp>

#include <chrono>

namespace updclient::cli {

namespace {

Outcome<net::Endpoint> acmeEndpoint(const Context &context) {
  if (!context.hasExplicitTarget()) return usageError("acme commands need --target or --ip");
  const std::string &spec = context.options.target.empty() ? context.options.ip : context.options.target;
  auto endpoint = net::Endpoint::parse(spec);
  if (!endpoint) return usageError("invalid target '" + spec + "': " + endpoint.error().message);
  if (context.options.timeoutMs) endpoint->timeout = std::chrono::milliseconds(*context.options.timeoutMs);
  return *endpoint;
}

Outcome<void> runStatus(Context &context) {
  auto endpoint = acmeEndpoint(context);
  if (!endpoint) return unexpected<Failure>(endpoint.error());

  auto client = acme::AcmeClient::connect(*endpoint);
  if (!client) return fromError(client.error());
  auto status = client->status();
  if (!status) return fromError(status.error());

  context.output.result({{"status", *status}}, "Status: " + std::to_string(*status));
  return {};
}

} // namespace

void registerAcmeCommands(CLI::App &app, Context &context) {
  auto *group = addGroup(app, "acme", "Acme device operations (needs --target or --ip)");
  auto *status = group->add_subcommand("status", "Read the device status word");
  status->callback([&context] { context.finish(runStatus(context)); });
}

} // namespace updclient::cli
```

2. Declare the function in `src/cli/commands.hpp`:

```cpp
void registerAcmeCommands(CLI::App &app, Context &context);
```

3. Call it in `run()` in `src/cli/app.cpp`, next to the other `registerXxxCommands(app, context)` calls
   and before `app.footer(kFooter)`.

Rules for CLI commands:

- All stdout goes through `context.output.result(json, text)`; the JSON object is the contract with
  scripts. Call it at most once, and put `"acknowledged": false` in the result of any command the device
  does not acknowledge.
- Return `Outcome<void>` and report through `context.finish(...)`: it logs to stderr, writes the JSON
  error document and sets the exit code. Use `usageError` for bad arguments (exit 2), `fromError` for
  library errors (`InvalidArgument` gives exit 2, everything else 1) and `failWith` for a specific code.
- Logs and progress go to stderr through spdlog (`cli/progress.hpp`).
- A destructive command passes a non-empty action text to `withUpdServer`, which makes it ask for
  confirmation (or require `--yes`). For a new protocol, add an equivalent `withAcme` helper in
  `cli/session.hpp` that resolves the endpoint, calls `context.confirmDestructive`, connects and runs the
  body.
- `Context::explicitEndpoint(bool forXell)` is written for the two existing protocols. The example
  above parses `--target` directly; with a third protocol in regular use, generalise it (for example
  to take the default port) instead of adding another boolean.
- Document the command in the README command table.

## Known limitations

- Addresses are numeric. `platform::resolve` passes `AI_NUMERICHOST`, and TCP resolves with
  `kDefaultFamily` (`AF_INET`), so a host name or an IPv6 literal fails with `InvalidArgument`.
  `UdpSocket` can already bind and send over IPv6 (`DatagramBindOptions::family`), but nothing in the
  built-in providers uses it. `Endpoint::parse` already
  accepts `[::1]:49`.
- Discovery listens only for UpdServer UDP broadcasts on IPv4, and port 48 needs privileges on Linux.
  XeLL has no discovery; `xell::XellProbeProvider` probes endpoints you list and is not registered by
  default or used by the CLI.
- `registerBuiltins()` registers only `tcp` and UpdServer discovery. Other transports and providers need
  an explicit `registerXxx()` call.
- Timeouts apply to each connect and each read or write call, not to a whole transfer.
- Many UpdServer commands are not acknowledged; success means "sent". The protocol has no framing, so
  a failed exchange closes the connection.
- `http_lite` is HTTP/1.0 GET only: no chunked encoding, redirects, keep-alive or TLS. Responses with
  another `Transfer-Encoding` are rejected as `Unsupported`.
- Clients and transports are not thread-safe; the two registries are.
- `Context::explicitEndpoint` and `resolveUpdServerEndpoint` in the CLI know two protocols and treat
  discovered devices as `tcp` endpoints.
- The library has a C++ API only. A shared build exports `UPDCLIENT_API` symbols that use `std::string`
  and `std::map`, so the library and its users must be built with the same compiler and standard library.
- Verified here: GCC and Clang on Linux, MinGW-w64 cross build (build only). macOS and MSVC are not
  verified.
- No license has been chosen.

## Roadmap

Each item lists the files it would touch. "Add" means a new file.

### IPv6 and host names

- `src/net/platform/socket_platform.hpp`: change `kDefaultFamily` from `AF_INET` to `AF_UNSPEC`.
- `src/net/platform/socket_platform.cpp`: in `resolve`, drop `AI_NUMERICHOST` for host names and update
  the "numeric IP address required" message. `getaddrinfo` is blocking and not bounded by
  `Endpoint::timeout`, so decide how to bound DNS time.
- `src/net/tcp_transport.cpp`: already tries every resolved address and creates the socket from each
  address family; only `describe()` needs to bracket IPv6 literals.
- `src/net/udp_socket.cpp`: binding the announcement port for IPv6 needs a dual-stack or second socket.
- `tests/net/tcp_loopback_test.cpp` and `tests/support/loopback_server.hpp`: add IPv6 loopback cases (the
  support server is IPv4 only).
- `src/cli/app.cpp`: help text says "IP"; the endpoint handling in `src/cli/context.cpp` needs no change.

No public header changes: `Endpoint` already carries a string host.

### TLS

- Add `include/updclient/net/tls_transport.hpp` and `src/net/tls_transport.cpp`: an `ITransport` that
  wraps an inner `TransportPtr`, performs the handshake, and implements `readSome`/`writeSome` on the
  TLS session. Its connector, registered for `tls`, opens the inner transport through
  `TransportRegistry::instance().connect(...)` with the scheme rewritten to `tcp`, then handshakes.
  Settings such as certificate verification go in `Endpoint::options`.
- `CMakeLists.txt`: add the TLS library (a new `FetchContent_Declare` or `find_package`) and link it
  `PRIVATE` to `updclient_lib`. Globbing does not cover dependencies.
- `src/updclient.cpp` and `include/updclient/updclient.hpp` only if it becomes a built-in.
- Add `tests/net/tls_transport_test.cpp`.

`UpdServerClient`, `XellClient` and `http_lite` work over it unchanged. XeLL's HTTPD does not speak TLS, so
this matters for new devices and protocols.

### Serial and USB

- Add the files from section 1: `include/updclient/net/serial_transport.hpp`,
  `src/net/serial_transport.cpp`, `src/net/serial/serial_port.hpp`, and the per-OS implementations.
  No CMake change on POSIX or Windows (termios and the Win32 API come with the platform).
- `src/cli/app.cpp`: one registration line so `--target serial://...` is accepted.
- A USB device that enumerates as a serial port (CDC/FTDI) is covered by the same transport. Raw USB
  (bulk endpoints) would be a separate `usb_transport` with its own scheme and a libusb dependency in
  `CMakeLists.txt`.
- UpdServer over a non-IP medium also needs a decision about the announcement/discovery path, which is
  UDP-only today.

### New console protocols

- Add `include/updclient/protocols/<name>/{protocol,client,discovery}.hpp` and the matching
  `src/protocols/<name>/` sources, as in sections 2 and 3.
- Add `src/cli/<name>.cpp`, one declaration in `src/cli/commands.hpp` and one call in
  `src/cli/app.cpp`, as in section 4. `src/cli/context.cpp` and `src/cli/session.cpp` need a resolver and
  a `with<Name>` helper if the protocol is used regularly.
- Add tests under `tests/protocols/<name>/` using `MockScript` and `FakeDatagrams`.
- Add the headers to `include/updclient/updclient.hpp` (optional).
