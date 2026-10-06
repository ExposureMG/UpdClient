# UpdClient

A C++20 library and command line tool for talking to an Xbox 360 over the network.

- `updclient_lib` (CMake alias `UpdClient::updclient_lib`): protocol clients, transports and
  discovery behind small interfaces. Every fallible call returns `Result<T>`; nothing throws
  for expected failures.
- `updclient`: a CLI built on the library. It is the only part that depends on CLI11 and
  nlohmann_json.

## Supported protocols

| Protocol | Transport | What it does |
| --- | --- | --- |
| UpdServer | TCP, default port 49. Discovery: UDP broadcast on port 48 | Console info and keys, NAND dump and raw block read/write/erase, bad block list, physical and hypervisor peek/poke, 1BL and bootloader dumps, file get/send, mount/unmount/mkdir, reboot/SMC reset/shutdown |
| XeLL (Reloaded HTTPD) | HTTP/1.0 over TCP, default port 80. No discovery: you name the console | CPU key, DVD key and colours from the index page, NAND flash dump (`/FLASH`), fuse listing, keyvault (`/KV`, `/KVRAW`, `/KVRAW2`) |

DashLaunch is not supported.

Transports are looked up by URI scheme. Only `tcp` is built in; adding another (serial, USB, TLS)
means adding files, see [docs/EXTENDING.md](docs/EXTENDING.md).

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. The library needs only C++20 (`<span>`,
`<concepts>`, `<bit>`). The CLI additionally uses `<format>`, which means GCC 13+, Clang 17+ or a
recent MSVC.

Dependencies are fetched with CMake `FetchContent` on the first configure: CLI11 2.4.2, spdlog 1.14.1,
nlohmann_json 3.11.3 and TartanLlama/expected 1.1.0. `Result<T>` is built on `tl::expected` by default.
It is part of the library's ABI, so it never depends on the `-std` a consumer compiles with. Configure
with `-DUPDCLIENT_USE_STD_EXPECTED=ON` (and `-DCMAKE_CXX_STANDARD=23`) to use `std::expected` instead;
the installed target then carries `UPDCLIENT_USE_STD_EXPECTED` so consumers agree with the library.

Verified for this repository: GCC and Clang on Linux (native), and MinGW-w64 as a Linux-to-Windows
cross build (build only, the `.exe` was not run). macOS and MSVC are supported by design but were not
built or run for this documentation.

### Linux and macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/updclient --help
```

Use `-DCMAKE_CXX_COMPILER=clang++` for Clang. On macOS install a recent Xcode or Homebrew LLVM.

### Windows with MinGW-w64

Natively from an MSYS2 MinGW shell, the commands above work unchanged
(`-G "MinGW Makefiles"` or `-G Ninja`). To cross-compile from Linux, create a toolchain file:

```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
```

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

### Windows with MSVC

From a Visual Studio developer prompt:

```bat
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
build\Release\updclient.exe --help
```

Winsock is linked (`ws2_32`) and initialised on demand by the library; callers never call
`WSAStartup`.

### CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `UPDCLIENT_BUILD_CLI` | `ON` | Build the `updclient` executable. Turn off to avoid CLI11 and nlohmann_json. |
| `UPDCLIENT_BUILD_TESTS` | `ON` at top level | Build `updclient_tests` from `tests/`. |
| `UPDCLIENT_WARNINGS` | `ON` at top level | `-Wall -Wextra -Wpedantic` (`/W4` on MSVC) for project targets. |
| `UPDCLIENT_INSTALL` | `ON` at top level | Generate install and export rules (`UpdClientConfig.cmake`). |
| `BUILD_SHARED_LIBS` | `OFF` | Build `updclient_lib` as a shared library. Only `UPDCLIENT_API` symbols are exported. |

When UpdClient is added with `add_subdirectory`, the tests, warnings and install rules default to off.
Library sources are every `src/**/*.cpp` except `src/main.cpp` and `src/cli/`; they are globbed, so a
new file needs no CMake edit (re-run CMake to pick it up).

### Offline builds

The four `FetchContent_Declare` names are `cli11`, `spdlog`, `json` and `expected`. Point each at an
existing source tree to build without network access:

```sh
cmake -S . -B build \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_CLI11=/path/to/CLI11 \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=/path/to/spdlog \
  -DFETCHCONTENT_SOURCE_DIR_JSON=/path/to/json \
  -DFETCHCONTENT_SOURCE_DIR_EXPECTED=/path/to/expected
```

The dependencies are skipped when the targets `spdlog`, `tl::expected`, `CLI11::CLI11` or
`nlohmann_json::nlohmann_json` already exist, so a parent project can supply its own copies.

### Install

```sh
cmake --install build --prefix /usr/local
```

installs the library, the public headers under `include/updclient/`, the `updclient` binary and the
CMake package files; the bundled spdlog and tl-expected are installed into the same prefix. A consumer
then uses `find_package(UpdClient)` and links `UpdClient::updclient_lib`; the package needs `tl-expected`
(and `spdlog` for a static build) to be findable.

## Tests

```sh
cmake --build build --target updclient_tests
ctest --test-dir build --output-on-failure
```

`ctest` runs the whole binary as one entry. Run it directly for finer control:

```sh
./build/tests/updclient_tests --list
./build/tests/updclient_tests --filter UpdServerClient
./build/tests/updclient_tests --verbose
```

The suite uses a small built-in harness (no third-party framework). Protocol and discovery tests run
against scripted fakes (`tests/support/mock_transport.hpp`, `tests/support/fake_datagram_socket.hpp`),
so they need no console and no network; a few tests use a loopback TCP server on `127.0.0.1`.

## Using the library

Link `UpdClient::updclient_lib` and include the umbrella header. Call `registerBuiltins()` once; it
registers the `tcp` transport and the UpdServer discovery provider and is safe to call repeatedly.

```cmake
set(UPDCLIENT_BUILD_CLI OFF CACHE BOOL "")
add_subdirectory(third_party/UpdClient)
target_link_libraries(my_app PRIVATE UpdClient::updclient_lib)
```

```cpp
#include <updclient/updclient.hpp>

#include <iostream>
#include <span>

using namespace updclient;

int main() {
  registerBuiltins();

  auto endpoint = net::Endpoint::parse("tcp://192.168.1.5:49?timeout=3000");
  if (!endpoint) {
    std::cerr << formatError(endpoint.error()) << "\n";
    return 2;
  }

  auto client = updserver::UpdServerClient::connect(*endpoint);
  if (!client) {
    std::cerr << formatError(client.error()) << "\n";
    return 1;
  }

  auto info = client->getInfo();
  if (!info) {
    std::cerr << formatError(info.error()) << "\n";
    return 1;
  }
  std::cout << "CPU key: " << formatHex(std::span<const uint8_t>(info->cpuKey)) << "\n";

  auto bad = client->getBadBlockList();
  if (bad) {
    std::cout << bad->size() << " bad blocks\n";
  } else if (bad.error().code == ErrorCode::Timeout) {
    std::cerr << "console stopped answering\n";
  }

  // XeLL opens a fresh connection per request, so one client object is reusable.
  auto xellEndpoint = net::Endpoint::parse("192.168.1.5");  // bare host means tcp, port 80 for XeLL
  if (!xellEndpoint) return 2;
  xell::XellClient xellClient = xell::XellClient::forEndpoint(*xellEndpoint);
  if (auto page = xellClient.getInfo(); page) {
    for (const auto &field : page->missingFields()) std::cerr << "missing: " << field << "\n";
  }
  return 0;
}
```

Things to know:

- `Result<T>` is `expected<T, Error>`. `Error` carries an `ErrorCode` (`Unknown`, `InvalidArgument`,
  `Unsupported`, `NotConnected`, `ConnectFailed`, `Timeout`, `Disconnected`, `Io`, `Protocol`,
  `LimitExceeded`, `Cancelled`), a message and the OS error number. `formatError` renders all three and
  `errorCodeName` gives the code as text.
- `Endpoint` is a URI: `scheme://host[:port][?key=value&...]`. A bare `host` or `host:port` means `tcp`.
  Port 0 means "unspecified": for `tcp` the protocol client substitutes its default (UpdServer 49,
  XeLL 80); other schemes keep port 0 unless they were registered with `net::SchemeTraits` (a fixed
  `defaultPort`, or `usesProtocolPort`). The `timeout` option (milliseconds, capped at
  `Endpoint::kMaxTimeout`, 24 hours) sets `Endpoint::timeout`, which bounds the connect and the initial
  read/write timeout. Other options are kept in `Endpoint::options` for transports that need them.
- To cancel a long read or write, call `close()` on its transport from another thread: the blocked
  call returns `ErrorCode::Cancelled` at once instead of waiting for its timeout. A TCP connect cannot
  be cancelled this way; it is bounded by `Endpoint::timeout`.
- Protocol clients never create sockets. `UpdServerClient` takes a `net::TransportPtr` (or connects
  one through `TransportRegistry`); `XellClient` takes a connector returning a transport. Anything
  that implements `net::ITransport` can carry either protocol.
- Many UpdServer commands (reboot, poke, writeBlock, eraseBlock, sendFile, ...) are not acknowledged
  by the console, so a successful `Result<void>` only means the bytes were sent. See the comments in
  `include/updclient/protocols/updserver/client.hpp` for the full list and for the rule that a failure
  mid-exchange closes the connection (`isConnected()` turns false; reconnect).
- Sizes announced by a peer are bounded by `updserver::ClientLimits`; exceeding one gives
  `ErrorCode::LimitExceeded`.
- Downloads (`getFile`, `dumpFlash`, `XellClient::dumpFlash`) go to `<path>.part` or a temporary file
  next to the destination and are renamed only when complete.
- Local files are `std::filesystem::path` everywhere. Text that is UTF-8 (command line, JSON, messages)
  converts with `pathFromUtf8` / `pathToUtf8` from `updclient/core/path.hpp`, because on Windows a
  `std::string` path is read in the ANSI code page.
- Discovery: `discovery::DiscoveryRegistry::instance().discoverAll(std::chrono::seconds(3))` returns
  the `DiscoveredDevice` list from every registered provider.
- The library logs through spdlog, which stays out of the public headers. A library user can set
  their own default logger; the CLI sends it to stderr.

## Command line

```
updclient [global options] <command> [<action>] [arguments]
```

Global options may appear before or after the command.

| Option | Meaning |
| --- | --- |
| `-t, --target <uri>` | Target such as `tcp://192.168.1.5:49` or a bare IP. |
| `-i, --ip <addr>` | Shortcut for a tcp target; mutually exclusive with `--target`. |
| `-p, --port <n>` | UpdServer TCP port, default 49. Rejected for `xell` commands. |
| `--xell-port <n>` | XeLL HTTP port, default 80. Rejected for UpdServer commands. |
| `--timeout-ms <n>` | Connect and I/O timeout, 0 for none, default 5000. |
| `--discovery-timeout-ms <n>` | How long discovery listens, default 3000. |
| `-j, --json` | Print results and errors as one JSON document. |
| `-v, --verbose` | Debug logging on stderr. |
| `--yes` | Skip the confirmation required by destructive commands. |
| `--version`, `-h, --help` | Version and help (`updclient <command> --help` for a command). |

Numbers accept decimal (`4096`) or `0x`-prefixed hex (`0x1000`). UpdServer commands use the `--target`
or `--ip` console, or discover one when neither is given. `xell` commands never auto-discover.

| Command | Protocol | What it does | Confirms |
| --- | --- | --- | --- |
| `discover` | UpdServer | List consoles that announce themselves | |
| `info` | UpdServer | Kernel version, NAND geometry, pairing data, CPU and DVD keys | |
| `version` | UpdServer | UpdServer version on the console | |
| `power reboot` / `smc-reset` / `shutdown` | UpdServer | Reboot, SMC reset or shut down the console | yes |
| `nand dump [-o file]` | UpdServer | Dump the full NAND (default `nanddump.bin`) | |
| `nand badblocks` | UpdServer | List bad blocks | |
| `nand read-block <block> [count] [-o file]` | UpdServer | Hex dump to stdout, or raw bytes to a file | |
| `nand erase-block <block> [count]` | UpdServer | Erase blocks | yes |
| `nand write-block <block> <file>` | UpdServer | Write one block from a file | yes |
| `mem peek <addr> [len]` / `mem hvpeek <addr> [len]` | UpdServer | Read physical or hypervisor memory (default length 16) | |
| `mem poke <addr> <value>` / `mem hvpoke <addr> <value>` | UpdServer | Write a 32-bit or 64-bit value | yes |
| `mem get1bl [-o file]` | UpdServer | Dump the 1BL ROM (default `1bl.bin`) | |
| `file get <remote> <local>` / `file send <local> <remote>` | UpdServer | Download or upload a file | |
| `file mount <mount-point> <device>` / `file unmount <mount-point>` / `file mkdir <path>` | UpdServer | Storage management | |
| `xell info` | XeLL | CPU key, DVD key, page colours | |
| `xell flash-dump [-o file]` | XeLL | Download the NAND image (default `xell_flash.bin`) | |
| `xell fuses` | XeLL | Fuse listing | |
| `xell kv [-o file] [-r \| --raw-block]` | XeLL | Keyvault: decrypted, `--raw` (`/KVRAW`) or `--raw-block` (`/KVRAW2`) (default `kv.bin`) | |

Destructive commands print a warning and require typing `yes` on an interactive terminal. Without a
terminal they fail with a usage error unless `--yes` is given.

Examples:

```sh
updclient discover
updclient info --ip 192.168.1.5
updclient --target tcp://192.168.1.5:49 nand dump -o nand.bin
updclient nand read-block 0x10 2 -o blocks.bin --ip 192.168.1.5
updclient xell info --ip 192.168.1.5
updclient xell flash-dump --ip 192.168.1.5 --xell-port 8080 -o xell.bin
updclient power reboot --ip 192.168.1.5 --yes
```

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (also `--help` and `--version`) |
| 1 | Runtime or transport error, including a declined confirmation |
| 2 | Usage error: bad arguments, `--target` with `--ip`, a destructive command without `--yes` and without a terminal, `xell` without a target, a scheme with no registered transport, or any library `InvalidArgument` error |
| 3 | Discovery found nothing, or discovery is unavailable (for example the UDP port cannot be bound) |

### Output and `--json`

Results go to stdout. Logs, progress, prompts and warnings go to stderr, so stdout can be piped.

With `--json`, stdout carries exactly one JSON document and nothing else (a second document, if a
command somehow produced one, is dropped):

- Success: an object specific to the command, for example `info` prints `kernel_version`,
  `struct_version`, `dump_size`, `block_size`, `pairing`, `cpu_key`, `dvd_key`; `nand read-block`
  prints `block`, `count`, `bytes` and `data` (hex) or `output`; `xell info` prints `cpu_key`,
  `dvd_key`, `bg_color`, `fg_color` (each `null` when not found) and `missing_fields`.
- Failure: `{"error": {"code": "...", "message": "...", "os_error": 111}}`. `os_error` appears only
  when there is one. `code` is an `ErrorCode` name (`ConnectFailed`, `Timeout`, `Protocol`, ...) or
  one of `Usage`, `DiscoveryFailed`, `NoDevices`, `Aborted`.
- Commands the console does not acknowledge (`power *`, `nand erase-block`, `nand write-block`,
  `mem poke`, `mem hvpoke`, `file send`, `file mount`, `file unmount`, `file mkdir`) include
  `"acknowledged": false`. It means "sent", not "done"; verify with a follow-up read.
- `discover` prints `{"devices": [...]}`. When the list is empty it still prints it, and exits 3.

The exit code is the same with and without `--json`.

### Discovery and the privileged port

UpdServer consoles announce themselves with a UDP broadcast to port 48. Listening on a port below 1024
needs privileges on Linux, so `discover`, and any UpdServer command run without `--target` or `--ip`,
fail with exit code 3 and an explanatory message otherwise. Any one of these fixes it:

```sh
sudo ./build/updclient discover
sudo setcap cap_net_bind_service=+ep ./build/updclient
sudo sysctl net.ipv4.ip_unprivileged_port_start=0
```

The port must also be free (the socket is opened with address reuse) and not blocked by a firewall.
The console must be on the same broadcast domain. Passing `--target` or `--ip` skips discovery
entirely and needs no privileges.

## Project layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layering and dependency rules and
[docs/EXTENDING.md](docs/EXTENDING.md) for how to add a transport, a protocol client, a discovery
provider or a CLI command, plus known limitations and the roadmap.

```
include/updclient/   public headers, included as <updclient/...>
src/                 library sources mirroring include/, plus the private socket layer
src/cli/, src/main.cpp   the command line tool
tests/               test suite and fakes
cmake/               package config template
```

## License

No license has been chosen for this project yet.
