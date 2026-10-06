# Architecture

UpdClient is a C++20 library (`updclient_lib`) and a thin CLI (`updclient`). The library separates
three concerns that vary independently:

- transports: how bytes move (`net::ITransport`, `net::IDatagramSocket`);
- protocols: what the bytes mean (`updserver::UpdServerClient`, `xell::XellClient`);
- discovery: how devices are found (`discovery::IDiscoveryProvider`).

Each axis is an interface plus a registry or an injection point. Adding a transport, protocol or
discovery provider means adding files; no existing header or source has to change apart from one
registration line (see [EXTENDING.md](EXTENDING.md)). For how to use the library and the CLI, see the
[README](../README.md).

## Layout

Public headers are included as `<updclient/...>`; sources mirror them under `src/`.

```
include/updclient/
  core/        export.hpp expected.hpp error.hpp hex.hpp
  net/         endpoint.hpp transport.hpp transport_registry.hpp datagram.hpp
               tcp_transport.hpp udp_socket.hpp http_lite.hpp
  discovery/   discovery.hpp
  protocols/
    updserver/ protocol.hpp client.hpp discovery.hpp
    xell/      client.hpp discovery.hpp
  updclient.hpp              umbrella header and registerBuiltins()
src/
  core/ net/ discovery/ protocols/ updclient.cpp   library sources (mirror include/)
  net/platform/socket_platform.{hpp,cpp}           PRIVATE socket layer
  cli/ main.cpp                                    CLI only (CLI11, nlohmann_json)
tests/                                             test suite, fakes in tests/support/
cmake/UpdClientConfig.cmake.in                     package config template
```

## Layers and dependency rules

```
          src/cli, src/main.cpp            CLI11, nlohmann_json
                    |
     protocols/updserver, protocols/xell   one directory per protocol
            |                  |
       discovery/       net/  endpoint, transport, registry, tcp, udp, http_lite
            |                  |
            |            net/platform      private: winsock / BSD sockets
            |                  |
            +------ core/ -----+           error, expected, hex, export
```

A layer may include only from the layers below it, following the lines. `net/platform` is reachable only
from `net/` sources.

| Code in | May include | Must not include |
| --- | --- | --- |
| `include/updclient/core/` | `core/` and the standard library (and the expected shim's backend) | everything else |
| `include/updclient/net/` | `core/`, `net/` | `discovery/`, `protocols/`, `src/` headers, any OS socket header, spdlog |
| `include/updclient/discovery/` | `core/` | `net/`, `protocols/` |
| `include/updclient/protocols/<p>/` | `core/`, `net/`, `discovery/`, its own protocol directory | another protocol's directory, OS socket headers, spdlog |
| `src/net/*.cpp` | public `core/` and `net/`; `net/platform/socket_platform.hpp` (only `tcp_transport.cpp` and `udp_socket.cpp` do) | `protocols/`, `discovery/` |
| `src/net/platform/` | `core/error.hpp`, OS headers | everything else; it is included by nothing under `include/` |
| `src/protocols/<p>/*.cpp` | public headers, spdlog | `net/platform/`, another protocol's directory |
| `src/cli/`, `src/main.cpp` | `<updclient/updclient.hpp>`, CLI11, nlohmann_json, spdlog | `net/platform/` |
| `tests/` | public headers, `tests/support/` | `net/platform/` (the loopback server has its own tiny socket layer) |

These rules hold in the tree today and are checked by `grep -r '#include' include src`:

- No file under `include/` includes `<winsock2.h>`, an OS socket header, `spdlog`, CLI11 or
  nlohmann_json, and none includes a header under `src/`.
- `net/platform/socket_platform.hpp` is included only by `src/net/platform/socket_platform.cpp`,
  `src/net/tcp_transport.cpp` and `src/net/udp_socket.cpp`.
- `protocols/updserver` and `protocols/xell` do not include each other. They share only `core/`,
  `net/` and `discovery/`.
- Nothing outside `src/cli/` and `src/main.cpp` includes CLI11 or nlohmann_json. Only the `updclient`
  target links them.
- spdlog is linked `PRIVATE` to `updclient_lib` and appears only in `.cpp` files and CLI sources.

### Private platform layer

`src/net/platform/socket_platform.{hpp,cpp}` is the only place that knows about Winsock versus BSD
sockets: handle and length types, `closesocket` versus `close`, `WSAStartup`, `poll` versus `select`,
non-blocking connect, error classification, `getaddrinfo`. It exposes `platform::Socket`
(move-only RAII handle), `platform::RuntimeGuard` (refcounted `WSAStartup`/`WSACleanup`; a no-op
elsewhere), `resolve`, `connectWithTimeout`, `waitReadable`, `sendSome`, `recvSome`, `recvFrom`,
`bindSocket` and friends. The CMake `PRIVATE` include directory `src/` is what lets
`src/net/tcp_transport.cpp` write `#include "net/platform/socket_platform.hpp"`; consumers of the
library never see that directory. Windows defines (`_WIN32_WINNT=0x0600`, `WIN32_LEAN_AND_MEAN`,
`NOMINMAX`) and `ws2_32` are also `PRIVATE`.

## Core

- `core/export.hpp`: `UPDCLIENT_API` (static build: empty; Windows DLL: `dllexport`/`dllimport`; GCC and
  Clang: default visibility). The library is built with hidden visibility, so only `UPDCLIENT_API`
  declarations are exported from a shared build.
- `core/expected.hpp`: `updclient::expected`, `unexpected`, `unexpect`. `tl::expected` (a `PUBLIC`
  dependency) unless `UPDCLIENT_USE_STD_EXPECTED` is defined, which CMake does publicly and only when
  the library was configured with `-DUPDCLIENT_USE_STD_EXPECTED=ON`. The choice is never made from the
  consumer's `-std`: `Result<T>` is in every exported signature, so a prebuilt library and its consumers
  must see the same type.
- `core/error.hpp`: `ErrorCode`, `Error { code, message, sysError }`, `Result<T>`, `makeError`, `fail`
  (returns `unexpected<Error>`), `errorCodeName`, `formatError`.
- `core/hex.hpp`: `formatHex` and `parseHex`.
- `core/path.hpp`: `pathFromUtf8` and `pathToUtf8`, header only. UTF-8 text and `std::filesystem::path`
  convert through these (a `std::string` path is ANSI on Windows). Local file arguments of the clients are
  `std::filesystem::path`.

Failures are values. Every fallible call returns `Result<T>`; the library does not throw for expected
failures (I/O errors, bad input, protocol violations).

## Net

- `Endpoint` (`net/endpoint.hpp`): the address of a peer as `scheme://host[:port][?key=value&...]`.
  A bare host means `tcp`. Port 0 means "not specified". The `timeout` option fills
  `Endpoint::timeout`, capped at `Endpoint::kMaxTimeout` (24 h) so deadline arithmetic cannot overflow; every other key lands in `Endpoint::options`, so a new transport can read its own
  settings (`baud`, `verify`, ...). Everything between `://` and `?` other than a trailing `:port` is the
  host, so device names such as `serial:///dev/ttyUSB0` parse. `[::1]:49` bracket syntax is parsed and
  printed.
- `ITransport` (`net/transport.hpp`): a reliable ordered byte stream. A transport implements six
  members: `isOpen`, `close`, `describe`, `setTimeout`, `readSome`, `writeSome`. `readSome` returning 0
  means orderly end of stream; a timeout is an `ErrorCode::Timeout` error. The base class supplies
  `writeAll`, `readExact` (EOF before the buffer is full is `Disconnected`) and `readUntilEof(maxBytes)`
  (`LimitExceeded` past the cap), so every transport gets identical framing helpers.
- `TransportRegistry` (`net/transport_registry.hpp`): thread-safe map from scheme (case-insensitive) to
  `Connector = std::function<Result<TransportPtr>(const Endpoint &)>`. `instance()` is process-wide;
  independent instances can be constructed, which is how tests isolate themselves. An unknown scheme is
  `ErrorCode::Unsupported`. `registerScheme` takes optional `SchemeTraits` and `withDefaultPort(endpoint,
  protocolPort)` applies them: a non-zero port is kept, else the scheme's `defaultPort`, else the
  protocol's own port if the scheme `usesProtocolPort` (`tcp` does), else port 0 stays. Protocol clients
  use it instead of writing 49 or 80 into every endpoint.
- `TcpTransport` (`net/tcp_transport.hpp`): the built-in `tcp` transport, registered by
  `registerBuiltins()`. It resolves through `getaddrinfo` and tries each returned address.
- `IDatagramSocket` and `UdpSocket` (`net/datagram.hpp`, `net/udp_socket.hpp`): UDP for discovery.
  `bind(port, reuse)` and `receive(timeout)` suit passive listeners. `bindWith(DatagramBindOptions)`
  (bind address, IPv4/IPv6, broadcast, multicast group and interface) and `sendTo(data, address, port)`
  serve providers that send probes; their defaults report `Unsupported`, so a minimal implementation
  only needs the three pure members. `DatagramSocketFactory` lets a caller inject a fake socket.
- `net/http_lite.hpp`: a minimal HTTP/1.0 GET client over any `ITransport` (`parseHttpHead`,
  `HttpExchange`, `httpGet`, `httpGetStream`, `httpGetToFile`). XeLL is built on it.

## Protocols

A protocol client takes its byte stream by injection and never constructs a socket.

- `updserver::UpdServerClient(TransportPtr, ClientLimits)`: a long-lived connection. The wire format has
  no framing, so after a failed exchange the client closes the transport and `isConnected()` turns
  false. `static connect(const Endpoint &)` resolves the transport through `TransportRegistry` and fills
  in port 49 when the endpoint has none. `protocol.hpp` holds wire constants and structures only: no
  I/O. `ClientLimits` bounds every size that comes from the peer.
- `xell::XellClient(Connector, hostHeader)`: XeLL answers with `Connection: close`, so every request
  asks the connector for a fresh transport. The object holds no connection and is reusable.
  `forEndpoint(const Endpoint &)` builds one whose connector calls `TransportRegistry::connect`, with
  port 80 as default. `parseXellInfo` is a pure function over the index page HTML.
- Both clients use `core/hex.hpp` and `net/` only.

## Discovery

- `discovery::IDiscoveryProvider` (`name()`, `discover(timeout, stopAfterFirst)`) and
  `discovery::DiscoveredDevice { protocol, address, info, lastSeen }`.
- `discovery::DiscoveryRegistry`: thread-safe; `add` replaces a provider of the same name;
  `discoverAll` runs providers one after another, skips failing providers, and returns an error only if
  every provider failed. With `stopAfterFirst` it stops at the first provider that finds a device.
- `updserver::UpdServerDiscovery`: binds UDP port 48 through an injectable `DatagramSocketFactory`,
  validates the 8-byte `NSvr` announcement, and returns early on the first hit when `stopAfterFirst` is
  set. It is registered by `registerBuiltins()`.
- `xell::XellProbeProvider`: probes a caller-supplied list of endpoints over HTTP and recognises XeLL by
  content. It is not registered by `registerBuiltins()`, because XeLL does not announce itself and the
  library never scans subnets on its own.

## Registration

`updclient::registerBuiltins()` (`updclient.hpp`, `src/updclient.cpp`) registers the `tcp` scheme and the
UpdServer discovery provider in the process-wide registries, guarded by `std::call_once`. Nothing is
registered by static initialisation, so linking a static library never changes behaviour behind the
caller's back. A new transport or provider ships its own `registerXxx(registry = instance())` function in
its own header; applications call it next to `registerBuiltins()`. Promoting it to a built-in is a
one-line edit in `src/updclient.cpp`.

## CLI

`src/cli/` is the only code that depends on CLI11 and nlohmann_json.

- `main.cpp` calls `cli::run`; `app.cpp` sets the spdlog default logger to stderr, calls
  `registerBuiltins()`, declares the global options and one `registerXxxCommands` function per command
  group (declared in `commands.hpp`).
- `context.{hpp,cpp}`: `Context` holds the global options, the `Output` writer and the exit code. It turns
  `--target`/`--ip`/`--port`/`--xell-port`/`--timeout-ms` into a `net::Endpoint`, runs UpdServer
  auto-discovery through `DiscoveryRegistry`, and implements the destructive-command confirmation.
- `session.{hpp,cpp}`: `withUpdServer` (resolve, confirm, connect, run) and `withXell`.
- `output.{hpp,cpp}`: everything on stdout goes through `Output`, which enforces the one-JSON-document
  contract.
- `args.{hpp,cpp}`, `fileio.{hpp,cpp}`, `progress.hpp`, `version.hpp`: number parsing, atomic file writes,
  throttled progress on stderr, version string.
- One file per command group: `discover.cpp`, `info.cpp`, `power.cpp`, `nand.cpp`, `mem.cpp`, `file.cpp`,
  `xell.cpp`.

The CLI maps `ErrorCode::InvalidArgument` to exit code 2 and every other error to 1; discovery failures
use 3. See the README for the table.

## Build system

- `updclient_lib` globs every `src/**/*.cpp` except `src/main.cpp` and `src/cli/` (with
  `CONFIGURE_DEPENDS`), so a new file needs no CMake edit. `updclient` is `src/main.cpp` plus
  `src/cli/*.cpp`. `updclient_tests` globs `tests/**/*.cpp`.
- Public include directory: `include/` (build interface) and the install include directory. Private:
  `src/`.
- Link: `tl::expected` public (unless `UPDCLIENT_USE_STD_EXPECTED`, which adds a public compile definition instead); `spdlog::spdlog` private; `ws2_32` private on Windows. The CLI links CLI11,
  nlohmann_json, spdlog and the library.
- `FetchContent` names `cli11`, `spdlog`, `json`, `expected` are fixed so
  `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides work. A dependency whose target already exists is not
  fetched.
- Under Clang, spdlog's bundled fmt is built with `FMT_CONSTEVAL=` to avoid consteval format-string
  checks failing on recent Clang releases.

## Threading

`TransportRegistry` and `DiscoveryRegistry` are thread-safe. `registerBuiltins()` is thread-safe and
idempotent. Protocol clients and transports are not thread-safe: use one at a time, or one per thread.
`XellClient` is stateless apart from its connector, so concurrent calls are only as safe as the
connector.

## Logging

The library logs through the spdlog default logger, only inside `.cpp` files. The CLI installs a stderr
logger, so stdout carries only results. A host application can install its own default logger before
calling the library.

## Tests

`tests/` is built into one binary, `updclient_tests`, by a small self-contained harness
(`tests/support/test_harness.hpp`: `TEST`, `XFAIL_TEST`, `CHECK*`, `REQUIRE*`, `SKIP`).

```
tests/main.cpp                     runner: --list, --filter <text>, --verbose
tests/core/                        error, expected shim, hex, path
tests/net/                         endpoint, transport helpers, registry, http_lite, tcp loopback, udp
tests/discovery/                   discovery registry
tests/protocols/updserver/         client, discovery
tests/protocols/xell/              client, discovery
tests/support/mock_transport.hpp   MockScript / MockTransport: scripted reads, expected writes, fault injection
tests/support/fake_datagram_socket.hpp   FakeDatagrams: scripted UDP source
tests/support/loopback_server.hpp  throwaway TCP server on 127.0.0.1
```

Because protocol clients and discovery take their I/O by injection, nearly every protocol path is tested
without a socket. A new transport, client or provider adds a matching `tests/` file; the glob picks it up.
