# Architecture

UpdClient is a C++20 library (`updclient_lib`) and a thin CLI (`updclient`). The library separates
three concerns that vary independently:

- transports: how bytes move (`net::ITransport`, `net::IDatagramSocket`);
- protocols: what the bytes mean (`updserver::UpdServerClient`, `xell::XellClient`, `xbdm::XbdmClient`);
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
    xbdm/      protocol.hpp path.hpp client.hpp discovery.hpp
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
  protocols/updserver, xell, xbdm         one directory per protocol
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
- `protocols/updserver`, `protocols/xell` and `protocols/xbdm` do not include each other. They share
  only `core/`, `net/` and `discovery/`.
- Nothing outside `src/cli/` and `src/main.cpp` includes CLI11 or nlohmann_json. Only the `updclient`
  target links them.
- spdlog is linked `PRIVATE` to `updclient_lib` and appears only in `.cpp` files and CLI sources.

### Private platform layer

`src/net/platform/socket_platform.{hpp,cpp}` is the only place that knows about Winsock versus BSD
sockets: handle and length types, `closesocket` versus `close`, `WSAStartup`, `poll` versus `select`,
non-blocking connect, error classification, `getaddrinfo`. It exposes `platform::Socket`
(move-only RAII handle), `platform::RuntimeGuard` (refcounted `WSAStartup`/`WSACleanup`; a no-op
elsewhere), `platform::WakeSignal` (wakes a wait on another thread: a pipe on POSIX, a UDP socket
connected to itself on 127.0.0.1 under Winsock, whose `select` takes only sockets), `resolve`,
`connectWithTimeout`, `waitReadable`, `waitFor` (readable or writable, or woken), `shutdownBoth`,
`wouldBlock`, `sendSome`, `recvSome`, `recvFrom`, `bindSocket` and friends. The CMake `PRIVATE` include directory `src/` is what lets
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
  (returns `unexpected<Error>`), `errorCodeName`, `formatError`. New codes are appended, so existing
  values never change; `Cancelled` is the newest.
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
  (`LimitExceeded` past the cap), so every transport gets identical framing helpers. Cancellation is
  part of the contract, see [Cancellation](#cancellation).
- `TransportRegistry` (`net/transport_registry.hpp`): thread-safe map from scheme (case-insensitive) to
  `Connector = std::function<Result<TransportPtr>(const Endpoint &)>`. `instance()` is process-wide;
  independent instances can be constructed, which is how tests isolate themselves. An unknown scheme is
  `ErrorCode::Unsupported`. `registerScheme` takes optional `SchemeTraits` and `withDefaultPort(endpoint,
  protocolPort)` applies them: a non-zero port is kept, else the scheme's `defaultPort`, else the
  protocol's own port if the scheme `usesProtocolPort` (`tcp` does), else port 0 stays. Protocol clients
  use it instead of writing 49 or 80 into every endpoint.
- `TcpTransport` (`net/tcp_transport.hpp`): the built-in `tcp` transport, registered by
  `registerBuiltins()`. It resolves through `getaddrinfo` and tries each returned address. After the
  connect the socket is non-blocking: `readSome`/`writeSome` try the call and, when it would block,
  wait in `platform::waitFor` on the socket and the transport's `WakeSignal`, up to the timeout.
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
- `xbdm::XbdmClient`: the Xbox debug monitor on TCP port 730, specified in
  [XBDM_PROTOCOL.md](XBDM_PROTOCOL.md). One client is one connection with one command in flight, never
  pipelined. `connect(endpoint)` takes `xbdm://host[:port]` (TCP directly) or any registered scheme;
  `open(connector)` and `attach(transport, ...)` take the transport by injection, and `reconnect()` uses
  the connector again. A 4xx answer is an error carrying the status (`consoleStatusCode`) and keeps the
  connection; every other failure closes it, and the client never reconnects on its own. `FileReader`
  (`getfile`) and `FileWriter` (`sendfile`) stream transfers and own the connection while open; an
  upload goes to `<name>.<8 hex>.part` and is renamed after the console's 200, and temporary names left
  by a drop are deleted by the next `reconnect()`; once the old file of that name was deleted (or may
  have been), the temporary file is never deleted, because it may hold the only copy. `protocol.hpp`
  holds the wire constants and parsers (no I/O), `path.hpp` the console path rules. `ClientOptions`
  bounds every length the console sends and carries the timeouts and an optional trace hook. Every
  wait is idle-based and also bounded as a whole (greeting, `bye`, command; file data excepted), and a
  202 body counts each line as at least 64 bytes against `maxBodyBytes`, because multi-line answers are
  parsed line by line into only the fields a command keeps. `getfileattributes` falls back to the
  parent's `dirlist` when the console does not know it (407), and for `downloadToFile` also when the
  answer has no size. `rawCommand` sends a line as typed, for diagnostics.
- The clients use `core/hex.hpp` and `net/` only.

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
- `xbdm::XbdmDiscovery`: broadcasts the XBDM name query (type 3) to UDP port 730 through an injectable
  `DatagramSocketFactory`, de-duplicates replies by address (at most `maxDevices`, 256) and asks each
  console its `dbgname` over TCP through an injectable connector, all queries together within
  `nameQueryBudget` (10 s). `findByName` (type 1) and `probeAddress` (one address) narrow
  it; `xbdm::identify(endpoint)` connects by address alone. `registerXbdmDiscovery()` adds it; it is
  not in `registerBuiltins()`.

## Registration

`updclient::registerBuiltins()` (`updclient.hpp`, `src/updclient.cpp`) registers the `tcp` scheme and the
UpdServer discovery provider in the process-wide registries, guarded by `std::call_once`. XBDM ships
`xbdm::registerXbdmScheme()` (the `xbdm` scheme: TCP, port 730 by default), `registerXbdmDiscovery()`
and `registerXbdm()` for both. Nothing is
registered by static initialisation, so linking a static library never changes behaviour behind the
caller's back. A new transport or provider ships its own `registerXxx(registry = instance())` function in
its own header; applications call it next to `registerBuiltins()`. Promoting it to a built-in is a
one-line edit in `src/updclient.cpp`.

## CLI

`src/cli/` is the only code that depends on CLI11 and nlohmann_json.

- `main.cpp` calls `cli::run`; `app.cpp` sets the spdlog default logger to stderr, calls
  `registerBuiltins()` and `xbdm::registerXbdmScheme()`, declares the global options and one
  `registerXxxCommands` function per command group (declared in `commands.hpp`).
- `context.{hpp,cpp}`: `Context` holds the global options, the `Output` writer and the exit code. It turns
  `--target`/`--ip`/`--port`/`--xell-port`/`--timeout-ms` into a `net::Endpoint` for a `Service`
  (UpdServer, XeLL or XBDM, which decides the default port), runs UpdServer auto-discovery through
  `DiscoveryRegistry` and XBDM auto-discovery through `XbdmDiscovery`, and implements the
  destructive-command confirmation. `targetsXbdm()` is true for an `xbdm://` target: the shared
  commands then take the XBDM path, and `resolveUpdServerEndpoint` refuses the target.
- `session.{hpp,cpp}`: `withUpdServer` (resolve, confirm, connect, run), `withXell` and `withXbdm`, which
  also opens the `--trace` file, installs the Ctrl-C handler (`interrupt.{hpp,cpp}`: the handler sets a
  flag, a watcher thread calls `XbdmClient::cancel()`) and deletes an interrupted upload's temporary
  file afterwards.
- `xbdm.{hpp,cpp}`: the XBDM halves of `file get/send/mkdir`, `mem peek/poke`, `power reboot/shutdown`
  and `info`, and the `xbdm` group. `trace.{hpp,cpp}` writes `--trace` files from
  `xbdm::ClientOptions::trace`.
- `output.{hpp,cpp}`: everything on stdout goes through `Output`, which enforces the one-JSON-document
  contract; `terminalText` escapes console-supplied text for text mode.
- `args.{hpp,cpp}`, `fileio.{hpp,cpp}`, `progress.hpp`, `version.hpp`: number parsing, atomic file writes,
  throttled progress on stderr, version string.
- One file per command group: `discover.cpp`, `info.cpp`, `power.cpp`, `nand.cpp`, `mem.cpp`, `file.cpp`,
  `xell.cpp`, `xbdm.cpp`.

The CLI maps `ErrorCode::InvalidArgument` to exit code 2 and every other error to 1; discovery failures
use 3. An XBDM refusal is reported with its status (`console_status` in JSON). See the README for the
table.

## Build system

- `updclient_lib` globs every `src/**/*.cpp` except `src/main.cpp` and `src/cli/` (with
  `CONFIGURE_DEPENDS`), so a new file needs no CMake edit. `updclient` is `src/main.cpp` plus
  `src/cli/*.cpp`. `updclient_tests` globs `tests/**/*.cpp`.
- Public include directory: `include/` (build interface) and the install include directory. Private:
  `src/`.
- Link: `tl::expected` public (unless `UPDCLIENT_USE_STD_EXPECTED`, which adds a public compile definition instead); `spdlog::spdlog` private; `ws2_32` private on Windows. The CLI links CLI11,
  nlohmann_json, spdlog and the library.
- Dependencies go through one macro, `updclient_dependency(name package version target)`: an existing
  `target` wins, then (with `UPDCLIENT_USE_SYSTEM_DEPS`) `find_package(package version CONFIG)`, then
  `FetchContent_MakeAvailable(name)`. The `FetchContent` names `cli11`, `spdlog`, `json`, `expected`
  are fixed so `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides work. CLI11 and nlohmann_json are declared and
  resolved only when `UPDCLIENT_BUILD_CLI` is on, and tl-expected only without
  `UPDCLIENT_USE_STD_EXPECTED`. Under `FETCHCONTENT_FULLY_DISCONNECTED` a dependency with no source
  is a configure error naming the variable to set. See the README for the commands.
- Under Clang, spdlog's bundled fmt is built with `FMT_CONSTEVAL=` to avoid consteval format-string
  checks failing on recent Clang releases. An installed spdlog is used as it is.

## Threading

`TransportRegistry` and `DiscoveryRegistry` are thread-safe. `registerBuiltins()` is thread-safe and
idempotent. Protocol clients and transports are not thread-safe: use one at a time, or one per thread.
The one exception is `ITransport::close()` (and `isOpen()`), which any thread may call at any time.

### Cancellation

`close()` on another thread is how a long transfer is cancelled. The contract, stated in
`net/transport.hpp`, holds for every transport, including the test transports:

- `close()` is idempotent, `noexcept` and returns promptly; it never waits for a timeout.
- A `readSome` or `writeSome` in progress when `close()` runs returns `ErrorCode::Cancelled` promptly.
  So do `readExact`, `writeAll` and `readUntilEof`, even when the close falls between two of their
  primitive calls (the helpers turn a later `NotConnected` into `Cancelled`). A local close is never
  reported as end of stream, so `readUntilEof` cannot return truncated data as success.
- A call that starts after `close()` fails with `ErrorCode::NotConnected`. Code that cancels treats
  both codes as "closed locally".
- The transport object must outlive every call on it; destroying it is not a way to cancel.

`TcpTransport` does this with a mutex-guarded count of calls in progress. `close()` marks the
transport closed, calls `shutdown` on the socket (the peer sees end of stream at once), signals the
`WakeSignal` that every wait also watches, and releases the descriptors only when no call is using
them, so a descriptor can never be closed and reused under a blocked thread. After a wait or a
failed or empty `recv`/`send`, the closed flag decides: `Cancelled`.

A TCP connect cannot be cancelled: `TcpTransport::connect` is a factory and there is no transport to
close until it returns. It is bounded by `Endpoint::timeout`.
`XellClient` is stateless apart from its connector, so concurrent calls are only as safe as the
connector.

`XbdmClient` is not thread-safe either, except `cancel()` (and `FileReader::cancel()`,
`FileWriter::cancel()`), which close the connection from any thread; the call in progress then fails
with `Cancelled`. Parallel work needs one client per thread, each with its own connection.

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
tests/net/                         endpoint, transport helpers, registry, http_lite, tcp loopback, udp,
                                   close() from another thread (tcp and MemoryPipe)
tests/discovery/                   discovery registry
tests/protocols/updserver/         client, discovery
tests/protocols/xell/              client, discovery
tests/protocols/xbdm/              client_*: the client against its own scripted fake (client_fake.hpp);
                                   mock_server_test: the mock on its own, with raw bytes;
                                   integration_*: the client against the mock, each test over an
                                   in-memory pipe and over loopback TCP
tests/cli/                         the updclient executable against the XBDM mock (when the CLI is built)
tests/support/mock_transport.hpp   MockScript / MockTransport: scripted reads, expected writes, fault injection
tests/support/memory_transport.hpp MemoryPipe: two connected blocking in-memory transports (cancellation,
                                   a mock server on another thread, no sockets)
tests/support/fake_datagram_socket.hpp   FakeDatagrams: scripted UDP source
tests/support/loopback_server.hpp  throwaway TCP server on 127.0.0.1
tests/support/xbdm_mock_server.*   XbdmMockServer: an XBDM console (section 5.1 of XBDM_PROTOCOL.md)
                                   with drives, files, memory, fault injection and a UDP name responder,
                                   over MemoryPipe or a loopback TCP listener
```

Because protocol clients and discovery take their I/O by injection, nearly every protocol path is tested
without a socket. A new transport, client or provider adds a matching `tests/` file; the glob picks it up.
