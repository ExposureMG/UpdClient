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
| XBDM (Xbox debug monitor: devkits, RGH/JTAG/Glitch2 consoles with an XBDM plugin, emulators that offer it) | Line protocol over TCP, port 730, scheme `xbdm://`. Discovery: UDP name protocol on port 730 | Console information, drives and free space, folder listings, file attributes, download and upload with 64-bit sizes, mkdir, delete, rename, memory read (`getmem`, `getmemex`) and write, memory regions, modules and sections, screenshot (raw frame buffer), reboot, launch, shutdown, tray, clock |

Transports are looked up by URI scheme. Only `tcp` is built in; `xbdm` (TCP with port 730 as the
default) is registered by `xbdm::registerXbdmScheme()`. Adding another (serial, USB, TLS) means adding
files, see [docs/EXTENDING.md](docs/EXTENDING.md).

The XBDM client follows [docs/XBDM_PROTOCOL.md](docs/XBDM_PROTOCOL.md), a contract written from
third-party clients. It has been tested only against a mock console (`tests/support/xbdm_mock_server`).
## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. The library needs only C++20 (`<span>`,
`<concepts>`, `<bit>`). The CLI additionally uses `<format>`, which means GCC 13+, Clang 17+ or a
recent MSVC.

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
| `UPDCLIENT_BUILD_CLI` | `ON` | Build the `updclient` executable. When off, CLI11 and nlohmann_json are neither declared nor looked for. |
| `UPDCLIENT_USE_SYSTEM_DEPS` | `OFF` | Look for each dependency with `find_package(... CONFIG)` first and fetch only the ones not found. |
| `UPDCLIENT_USE_STD_EXPECTED` | `OFF` | Build `Result<T>` on `std::expected` (needs `-DCMAKE_CXX_STANDARD=23`); tl-expected is then not needed. |
| `UPDCLIENT_BUILD_TESTS` | `ON` at top level | Build `updclient_tests` from `tests/`. |
| `UPDCLIENT_WARNINGS` | `ON` at top level | `-Wall -Wextra -Wpedantic` (`/W4` on MSVC) for project targets. |
| `UPDCLIENT_INSTALL` | `ON` at top level | Generate install and export rules (`UpdClientConfig.cmake`). |
| `BUILD_SHARED_LIBS` | `OFF` | Build `updclient_lib` as a shared library. Only `UPDCLIENT_API` symbols are exported. |

When UpdClient is added with `add_subdirectory`, the tests, warnings and install rules default to off.
Library sources are every `src/**/*.cpp` except `src/main.cpp` and `src/cli/`; they are globbed, so a
new file needs no CMake edit (re-run CMake to pick it up).

### Install

```sh
cmake --install build --prefix /usr/local
```

installs the library, the public headers under `include/`, the `updclient` binary and the
CMake package files; a spdlog or tl-expected that was fetched is installed into the same prefix, an
installed one is looked up again. A consumer then uses `find_package(UpdClient)` and links
`UpdClient::updclient_lib`; the package needs `tl-expected` (and `spdlog` for a static build) to be
findable.

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

The XBDM tests also run the client against `tests/support/xbdm_mock_server.{hpp,cpp}`, an in-memory
console with fault injection, twice per test: over an in-memory pipe and over loopback TCP (a TCP variant
is skipped with a message where sockets are refused). With the CLI built, `tests/cli/` runs the
`updclient` executable against the same mock. `UPDCLIENT_XBDM_HUGE=1` adds a test that moves 4 GiB + 1
in each direction over loopback (slow; it needs no disk space beyond a sparse file).

## Using the library

Link `UpdClient::updclient_lib` and include the umbrella header. Call `registerBuiltins()` once; it
registers the `tcp` transport and the UpdServer discovery provider and is safe to call repeatedly.
XBDM needs `xbdm::registerXbdm()` as well (the `xbdm` scheme and the XBDM discovery provider), or only
`xbdm::registerXbdmScheme()`.

```cmake
set(UPDCLIENT_BUILD_CLI OFF CACHE BOOL "")
add_subdirectory(third_party/UpdClient)
target_link_libraries(my_app PRIVATE UpdClient::updclient_lib)
```

```cpp
#include <updclient.hpp>

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

XBDM keeps one connection per client and answers every command:

```cpp
#include <updclient.hpp>

#include <iostream>

using namespace updclient;

int main() {
  registerBuiltins();
  xbdm::registerXbdmScheme();

  auto endpoint = net::Endpoint::parse("xbdm://192.168.1.50");  // port 730
  if (!endpoint) return 2;
  auto client = xbdm::XbdmClient::connect(*endpoint);
  if (!client) {
    std::cerr << formatError(client.error()) << "\n";
    return 1;
  }
  if (auto name = client->debugName()) std::cout << "console: " << *name << "\n";

  auto listing = client->list("HDD:\\");
  if (listing) {
    for (const auto &entry : listing->entries) std::cout << entry.name << " " << entry.size << "\n";
  } else if (auto status = xbdm::consoleStatusCode(listing.error())) {
    std::cerr << "refused with " << *status << "\n";  // a 4xx: the connection is still usable
  }

  // Uploads go to a temporary name and are renamed after the console confirmed the data.
  auto sent = client->uploadFromFile("save.bin", "HDD:\\Content\\save.bin",
                                     [](uint64_t done, uint64_t total) { std::cerr << done << "/" << total << "\r"; });
  if (!sent) std::cerr << formatError(sent.error()) << "\n";
  return 0;
}
```

Things to know:

- `Result<T>` is `expected<T, Error>`. `Error` carries an `ErrorCode` (`Unknown`, `InvalidArgument`,
  `Unsupported`, `NotConnected`, `ConnectFailed`, `Timeout`, `Disconnected`, `Io`, `Protocol`,
  `LimitExceeded`, `Cancelled`, `AlreadyExists`), a message and the OS error number. `formatError`
  renders all three and `errorCodeName` gives the code as text. New codes are only ever appended.
  `ConnectFailed` means no connection could be made (refused, unreachable, timed out, resolver
  failure; a timeout carries `ETIMEDOUT`); `Timeout` means a connection that was made went quiet.
- `Endpoint` is a URI: `scheme://host[:port][?key=value&...]`. A bare `host` or `host:port` means `tcp`.
  Port 0 means "unspecified": for `tcp` the protocol client substitutes its default (UpdServer 49,
  XeLL 80); other schemes keep port 0 unless they were registered with `net::SchemeTraits` (a fixed
  `defaultPort`, or `usesProtocolPort`). The `timeout` option (milliseconds, capped at
  `Endpoint::kMaxTimeout`, 24 hours) sets `Endpoint::timeout`, which bounds the whole connect (name
  lookup and every address together) and is the initial read/write timeout. Other options are kept in `Endpoint::options` for transports that need them.
- To cancel a long read or write, call `close()` on its transport from another thread: the blocked
  call returns `ErrorCode::Cancelled` at once instead of waiting for its timeout. A TCP connect has no
  transport to close yet: pass a `std::stop_token` to `TcpTransport::connect` (or to the XBDM connect
  and discovery calls below), and `request_stop()` ends it at once with `Cancelled`; without one it is
  bounded by `Endpoint::timeout`.
- TCP hosts are IPv4 addresses or host names. A name is looked up within `Endpoint::timeout`, and a
  stop request ends the lookup too; a name that does not resolve is `InvalidArgument`. UDP sockets
  take numeric addresses only. `TcpTransport::peer()` and `XbdmClient::peer()` give the address
  actually connected as an `Endpoint` (numeric host and port), so nothing has to parse `describe()`.
- The stop-token functions are overloads: existing calls compile unchanged, but taking the address
  of one of the overloaded functions (`&XbdmClient::connect`, `&xbdm::identify`, ...) needs a cast to
  the wanted signature.
- Protocol clients never create sockets. `UpdServerClient` takes a `net::TransportPtr` (or connects
  one through `TransportRegistry`); `XellClient` takes a connector returning a transport. Anything
  that implements `net::ITransport` can carry either protocol.
- Many UpdServer commands (reboot, poke, writeBlock, eraseBlock, sendFile, ...) are not acknowledged
  by the console, so a successful `Result<void>` only means the bytes were sent. See the comments in
  `include/protocols/updserver/client.hpp` for the full list and for the rule that a failure
  mid-exchange closes the connection (`isConnected()` turns false; reconnect).
- Sizes announced by a peer are bounded by `updserver::ClientLimits`; exceeding one gives
  `ErrorCode::LimitExceeded`.
- Downloads (`getFile`, `dumpFlash`, `XellClient::dumpFlash`) go to `<path>.part` or a temporary file
  next to the destination and are renamed only when complete.
- Local files are `std::filesystem::path` everywhere. Text that is UTF-8 (command line, JSON, messages)
  converts with `pathFromUtf8` / `pathToUtf8` from `core/path.hpp`, because on Windows a
  `std::string` path is read in the ANSI code page.
- Discovery: `discovery::DiscoveryRegistry::instance().discoverAll(std::chrono::seconds(3))` returns
  the `DiscoveredDevice` list from every registered provider.
- XBDM (`include/protocols/xbdm/client.hpp`): a 4xx answer is an error that carries the status
  (`xbdm::consoleStatusCode(error)`) and leaves the connection usable; any other failure (timeout,
  malformed answer, drop, `cancel()`) closes it, and the client never reconnects on its own: call
  `reconnect()`, which also deletes temporary files left by interrupted uploads. While a `FileReader` or
  `FileWriter` is open it owns the connection; open a second client for parallel work (the console
  limits connections and refuses with 401). `cancel()`, `FileReader::cancel()` and
  `FileWriter::cancel()` may be called from any thread. Console paths are `HDD:\dir\file`;
  `xbdm::toConsolePath` converts `/HDD/dir/file`. `ClientOptions::trace` receives every command and
  text line, and the size of binary data, never its bytes. An upload replaces only a file that existed
  when `openWrite()` ran (`FileWriter::replacesExisting()`): it deletes it right before the rename,
  while a file that appeared during the upload fails `finish()` with `AlreadyExists` and is left alone
  (a folder there is `InvalidArgument`). `openWrite()` checks every command line the upload needs
  before it sends anything. If the rename fails after the delete, the data stays under the temporary
  name, which `FileWriter::keptPath()` and `XbdmClient::keptUploads()` report, and is never deleted.
  After a failed call that changes something, `lastDelivery()` says how far the call got: `NotSent`
  means nothing that can change the console left the client, so it is safe to repeat; `Sent` means it
  may have happened, also when an earlier step of the call took effect. `rename()` onto an existing
  name fails with `AlreadyExists`. A rename that only changes the case of a name is allowed, and when
  its fallback does not finish the error says where the file is. `XbdmClient::connect`
  and `open`, `XbdmDiscovery::discover`, `findByName`, `probeAddress`, `xbdm::identify` and
  `DiscoveryRegistry::discoverAll` have `std::stop_token` overloads, so a GUI can cancel them with one
  `std::stop_source` per operation; a stopped search returns what it found. `cancel()` also ends a
  `reconnect()` stuck in its TCP connect. Every answer is bounded as a whole as well as between
  bytes (the greeting by `greetingTimeout`, the answer to `bye` by `byeTimeout`, everything else but
  file data by `commandTimeout`), so a console that trickles bytes cannot hold a call indefinitely.
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
| `-p, --port <n>` | UpdServer TCP port, default 49; for XBDM targets the TCP port, default 730. Rejected for `xell` commands. |
| `--xell-port <n>` | XeLL HTTP port, default 80. Rejected for UpdServer commands. |
| `--timeout-ms <n>` | Connect and I/O timeout, 0 for none, default 5000. |
| `--discovery-timeout-ms <n>` | How long discovery listens, default 3000. |
| `-j, --json` | Print results and errors as one JSON document. |
| `-v, --verbose` | Debug logging on stderr. |
| `--yes` | Skip the confirmation required by destructive commands. |
| `--trace <file>` | Append every XBDM command line and every line received to the file, with timings; file contents are never written, only their size. See [docs/HARDWARE_TEST_PLAN.md](docs/HARDWARE_TEST_PLAN.md). |
| `--version`, `-h, --help` | Version and help (`updclient <command> --help` for a command). |

Numbers accept decimal (`4096`) or `0x`-prefixed hex (`0x1000`). UpdServer commands use the `--target`
or `--ip` console, or discover one when neither is given. `xell` commands never auto-discover.

The protocol follows the target: with `--target xbdm://host[:port]` the commands marked "UpdServer or
XBDM" below speak XBDM, and UpdServer-only commands refuse the target (exit 2). The `xbdm` group always
speaks XBDM; there a bare host (`--target 192.168.1.50` or `--ip`) means `xbdm://`, and without a target
the first console found by XBDM discovery is used. Console paths are `HDD:\dir\file` or
`/HDD/dir/file`. During an XBDM command Ctrl-C cancels what is in progress, the connect and the greeting
included (exit 1, `Cancelled`); an interrupted upload's temporary file is deleted over a new connection,
or named in a warning, also when Ctrl-C arrives during that cleanup. During `discover`, or while the
`xbdm` group looks for a console, Ctrl-C ends the search: `discover` prints what it found and exits 1.
A failed XBDM command's `--json` error may carry `kept_upload` (an upload kept on the console as the only
copy; rename it with `xbdm mv`) and `command_delivery` (`not_sent`, or `unknown` when the console may have
carried the command out). Text the console sends
(names, `info`, `xbdm raw` answers) is printed with control characters and bytes above 0x7E as `\xNN`;
`--json` output carries it unchanged, JSON-escaped.

| Command | Protocol | What it does | Confirms |
| --- | --- | --- | --- |
| `discover [--protocol all\|updserver\|xbdm]` | UpdServer and XBDM | List consoles that announce themselves (UpdServer) or answer the XBDM name query | |
| `info` | UpdServer or XBDM | UpdServer: kernel version, NAND geometry, pairing data, CPU and DVD keys. XBDM: debug name, console type and id, running title, execution state, title address | |
| `version` | UpdServer | UpdServer version on the console | |
| `power reboot` / `shutdown` | UpdServer or XBDM | Reboot (XBDM: warm `magicboot`) or shut down the console | yes |
| `power smc-reset` | UpdServer | SMC reset | yes |
| `nand dump [-o file]` | UpdServer | Dump the full NAND (default `nanddump.bin`) | |
| `nand badblocks` | UpdServer | List bad blocks | |
| `nand read-block <block> [count] [-o file]` | UpdServer | Hex dump to stdout, or raw bytes to a file | |
| `nand erase-block <block> [count]` | UpdServer | Erase blocks | yes |
| `nand write-block <block> <file>` | UpdServer | Write one block from a file | yes |
| `mem peek <addr> [len]` | UpdServer or XBDM | Read memory (default length 16); XBDM shows unreadable bytes as `??` | |
| `mem poke <addr> <value>` | UpdServer or XBDM | Write a 32-bit value (XBDM: big-endian, as console memory is) | yes |
| `mem hvpeek <addr> [len]` / `mem hvpoke <addr> <value>` | UpdServer | Read or write hypervisor memory | `hvpoke` |
| `mem get1bl [-o file]` | UpdServer | Dump the 1BL ROM (default `1bl.bin`) | |
| `file get <remote> <local>` / `file send <local> <remote>` | UpdServer or XBDM | Download or upload a file, with progress | |
| `file mkdir <path>` | UpdServer or XBDM | Create a folder (XBDM: one level) | |
| `file mount <mount-point> <device>` / `file unmount <mount-point>` | UpdServer | Storage management | |
| `xell info` | XeLL | CPU key, DVD key, page colours | |
| `xell flash-dump [-o file]` | XeLL | Download the NAND image (default `xell_flash.bin`) | |
| `xell fuses` | XeLL | Fuse listing | |
| `xell kv [-o file] [-r \| --raw-block]` | XeLL | Keyvault: decrypted, `--raw` (`/KVRAW`) or `--raw-block` (`/KVRAW2`) (default `kv.bin`) | |
| `xbdm ls <folder>` / `xbdm stat <path>` | XBDM | List a folder (entries whose name no command could use back are skipped and counted); size, type and times of one path | |
| `xbdm drives` | XBDM | Drives with total and free bytes (at most 64 drives; drives still unasked after one command timeout, 60 s, are listed without sizes) | |
| `xbdm rm [--dir] <path>` | XBDM | Delete a file, or an empty folder | yes |
| `xbdm mv <from> <to>` | XBDM | Rename or move within one drive; the new name must not exist | |
| `xbdm screenshot [-o file]` | XBDM | Save the raw, still tiled frame buffer (default `screenshot.raw`) and print its geometry | |
| `xbdm launch <xex>` | XBDM | Start an executable (`magicboot title=`) | yes |
| `xbdm reboot [--cold]` | XBDM | Warm or cold `magicboot` | yes |
| `xbdm modules` / `xbdm regions` | XBDM | Loaded modules; committed memory regions (`walkmem`) | |
| `xbdm eject` | XBDM | Open the disc tray | |
| `xbdm raw '<line>'` | XBDM | Send one command line as typed and print the answer, for diagnostics (binary answers are not read) | yes |

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
updclient --target xbdm://192.168.1.50 info
updclient --target xbdm://192.168.1.50 file get 'HDD:\Content\save.bin' save.bin
updclient --target xbdm://192.168.1.50 file send game.xex /HDD/Games/Test/default.xex
updclient --target xbdm://192.168.1.50 --yes xbdm launch /HDD/Games/Test/default.xex
updclient --ip 192.168.1.50 xbdm ls /HDD --json
updclient --target xbdm://192.168.1.50 --trace xbdm-trace.txt xbdm drives
```

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (also `--help` and `--version`) |
| 1 | Runtime or transport error, including a declined confirmation, an XBDM refusal (4xx), a name already in use (`AlreadyExists`, for example `xbdm mv` onto an existing name) and a cancelled transfer |
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
  one of `Usage`, `DiscoveryFailed`, `NoDevices`, `Aborted`. When an XBDM console refused the command,
  `"console_status": 402` (the 4xx code) appears instead of `os_error`, and the message ends with the
  console's line.
- Commands the console does not acknowledge (`power *`, `nand erase-block`, `nand write-block`,
  `mem poke`, `mem hvpoke`, `file send`, `file mount`, `file unmount`, `file mkdir`) include
  `"acknowledged": false`. It means "sent", not "done"; verify with a follow-up read. XBDM answers
  every command, so over XBDM these report `"acknowledged": true`, except a reboot or shutdown the
  console answered by closing the connection.
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

XBDM discovery needs no privileges: it broadcasts a name query to UDP port 730 from an ephemeral port
and collects the replies, then asks each console its name over TCP. Devices are reported with the
port that was searched, and an auto-discovered target uses it (`--port` still wins). Many networks drop
the broadcast; name the console with `--target xbdm://<address>` then.

## Project layout

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layering and dependency rules and
[docs/EXTENDING.md](docs/EXTENDING.md) for how to add a transport, a protocol client, a discovery
provider or a CLI command, plus known limitations and the roadmap.

```
include/   public headers, included as <...>
src/                 library sources mirroring include/, plus the private socket layer
src/cli/, src/main.cpp   the command line tool
tests/               test suite and fakes
cmake/               package config template
```

## License

No license has been chosen for this project yet.
