# Plan: a full-featured JRPC adapter

Status: J1 to J7 and J9 are implemented; J8 is not started and stays
gated on the hardware pass. Written 2026-10-08 against commit `cfb2309`. Nothing has run against a
console: the suite (about 460 JRPC tests, over a `MemoryPipe` and over loopback TCP) shows the client is
consistent with JRPC_PROTOCOL.md, not that it works on hardware. Where the as-built code differs from
the text below, the [Implementation record](#implementation-record) at the end says so; the decisions D1
to D10 and the sketches are otherwise left as they were planned.
The wire contract is [JRPC_PROTOCOL.md](JRPC_PROTOCOL.md) (section numbers below refer to it).
"JRPC" is the TCP plugin `JRPC.xex` on port 1409, not JRPC2 (`consolefeatures` inside XBDM).

## Ground rules

As in the earlier plans: the console side does not change; public API changes are additive;
header comments change with the behavior they describe; every commit builds and passes `ctest`
on its own. New code follows [ARCHITECTURE.md](ARCHITECTURE.md) (layer table, `Result<T>`, no
exceptions for expected failures, `UPDCLIENT_API`, protocol directories do not include each other)
and [EXTENDING.md](EXTENDING.md). Commits are authored as ExposureMG without a co-author trailer,
and nothing is pushed without asking.

**The one caveat that shapes everything:** JRPC_PROTOCOL.md was derived from a single server
binary plus a client for the sister plugin. Nothing has run against a console. The mock server is
written from the same document, so a green suite proves the client is consistent with the
document, not that it works on hardware. The plan therefore (a) keeps every unverified guess
behind a named option or a documented `Contract:`, and (b) ends with a hardware checklist that
decides which options become defaults.

## What "full featured" covers, and what it cannot

| Feature | Basis | Status |
|---|---|---|
| Connect, banner `JRPC2 connected\r\n`, persistent connection, `Bye` | §1.2 | solid (single source) |
| Generic call, all return tags 0-8, by address or by `module`+`ord`, title or system thread | §2, §4.1, §5 | solid; arrays capped at 8 elements (open question 1) |
| Arguments: int, uint, bool, byte, int64/uint64, float/double, string, byte/int/float arrays; 37 args; line limit | §3 | solid |
| `ResolveFunction` (9), `GetCPUKey` (10), `GetKernelVersion` (13), `GetTemperature` (15), `GetCurrentTitleId` (16), `ConsoleType` (17) | §4.2 | solid; CPU key split is ambiguous (see D8) |
| `XNotify` (12), `SetLeds` (14), `ShutDownConsole` (11) | §4.2 | solid; whether they answer is unknown (see D6) |
| `constantMemorySet` (18) | §4.2 | sendable; no documented way to cancel one |
| Memory read and write | none: not a JRPC command (§4.2 note, §9.7) | **no native support.** Read is at best 8 bytes per call and needs a console routine nobody has identified; write needs `memcpy`'s ordinal. Shipped only as a hardware-gated helper (J8). The CLI keeps `mem` XBDM-only. |
| 64-bit array return (`CallArray<ulong>`) | `type=9` is ResolveFunction (open question 2) | `Unsupported` |
| Opcode 19 | unknown purpose (open question 4) | not exposed |
| Discovery | JRPC does not announce itself | probe by address only, like XeLL |

## Design decisions

**D1. Names.** Namespace `updclient::jrpc`, scheme `jrpc://host[:1409]` (TCP; registered by
`jrpc::registerJrpcScheme()`, which is not in `registerBuiltins()`, same as `xbdm`). Port constant
`kJrpcPort = 1409`. The wire module never uses "v1" or "JRPC2" for this protocol.

**D2. Wire and client are separate files; the wire module has no I/O.** `protocol.hpp`/`protocol.cpp`
build command lines and parse replies; `client.cpp` owns the connection. The command text is the
same `consolefeatures` text JRPC2 uses, so a future JRPC2 adapter wants the same builder. The layer
table forbids protocol-to-protocol includes, so for now it stays in `protocols/jrpc/`; when JRPC2
is built, extract it to a shared `protocols/consolefeatures/` directory and amend the table in
ARCHITECTURE.md. Keeping it free of JRPC-only assumptions (no banner, no `Bye`, no reply framing in
it) makes that a move, not a rewrite. JRPC2 differs in framing (`200-` prefix, `buf_addr=` poll), so
only the request side is shared.

**D3. One command in flight, any anomaly closes the connection.** A reply has no status prefix, so
the client cannot resynchronise after an unexpected line. Same rule as `XbdmClient`: a timeout, a
reply of the wrong shape for the `type` sent, an over-long line, or a drop closes the transport;
`isConnected()` turns false; `reconnect()` is the caller's decision. A call that times out is
especially bad: the function may still be running and its reply will arrive later, so the
connection must not be reused.

**D4. Remote failures.** The only failure signal is a reply line starting `error=` (section 6 says
this is what the JRPC2 build sends; the TCP build's wording is unknown). It becomes
`ErrorCode::Io` with the text at the end of `message`, and `jrpc::remoteFault(const Error &)`
(parallel to `xbdm::consoleStatusCode`) classifies known texts (`Could not resolve function
address`, `Version mismatch`, `The paramaters were not found`) with `sysError = 1000 + fault`; unknown
text is `RemoteFault::Other`. A reply line containing `DEBUG` is `Unsupported` ("JRPC is not
installed"), per section 6. The connection stays usable after `error=`, because the reply was a
well-formed line for that command.

**D5. Typed calls over a single `CallSpec`.** One data type describes a call; typed helpers are
thin wrappers. Arguments are a closed `Arg` variant, so nothing is guessed from C++ types at
runtime. Encoding rules fixed up front, each one a `Contract:`:

- ints go as decimal through tag `1`; a `uint32_t` above `INT32_MAX` is sent as the equal negative
  `int32` (the server reads `sscanf %i` into an int); `bool` is `1/0|1\`; byte is tag `4`; 64-bit
  is tag `8` as decimal.
- floats and doubles are tag `3`, `%.9g`/`%.17g` in the C locale (never `printf` with the process
  locale), because the server `sscanf`s them; NaN and infinities are `InvalidArgument`.
- strings are tag `2`, the byte count then the hex of the bytes; embedded NUL and non-UTF-8-valid
  input are refused (it is a C string on the console). Byte, int and float arrays all go as tag `7`
  with big-endian elements.
- checked before anything is sent: at most 37 arguments, at most `maxCommandBytes` (default 8191)
  for the whole line, `as` at most `maxArrayElements` (default 8), return kind not `Uint64Array`.
  Violations are `InvalidArgument` or `LimitExceeded`, and `lastDelivery()` stays `NotSent`.
- replies are parsed by the `type` that was sent: `%X` hex unsigned (typed helpers reinterpret as
  signed where asked), `%llX`, `%f` through `from_chars` (6 decimals, so tiny floats read as 0:
  documented), `%s` as the rest of the line, arrays as `v,v,...;`.

**D6. Opcodes that probably do not answer (11, 12, 14, 18).** If the server really sends nothing,
the client must not wait for a reply, but then a stray `error=` line would be misread as the next
call's reply. Policy, chosen to be safe either way and settled on hardware: after a silent opcode
the client sends a barrier (`GetKernelVersion`, a read-only decimal answer) and treats any line
before the decimal one as that opcode's answer (`error=` fails the call, `S_OK` or hex is
accepted). `ClientOptions::silentOpBarrier` (default on) turns it off. *As built, the barrier is
`ConsoleType` (opcode 17), not `GetKernelVersion`; see the record.* `shutdown()` and the opcode
that ends the connection are exempt: the client closes after sending, like `XbdmClient::shutdown`.
The mock has a switch for both behaviors.

**D7. Delivery tracking.** `lastDelivery()` with `NotSent`, `PartlySent`, `Sent`, `Answered`, as in
XBDM. Defined again in `jrpc` (the layer rule forbids reusing the XBDM type; if a third protocol
needs it, promote it to `core/`). Every `call` is treated as changing the console; the read-only
opcodes (9, 10, 13, 15, 16, 17) are marked so the CLI reports `command_delivery` only for the rest.

**D8. Parsing that the document leaves ambiguous.**
`GetCPUKey` is two unpadded `%X`, so `A1B2C3D4E5F60718` splits correctly only when both halves are
8 digits. *As built, 16 or 32 digits are accepted; see the record.* The client returns `Result<CpuKey>` when the reply is exactly 32 hex digits and
`Protocol` (with the raw text in the message) otherwise. A padded server just works; an unpadded
one is reported honestly instead of guessed.
Void calls accept `S_OK` or any hex.

**D9. Memory is a helper, not a protocol feature.** J8 adds `MemoryAccess` over `call()`: the caller
names the console routines (module, ordinal) to use for copy and for the pointer-returning
primitive, so no unverified ordinal is hard-coded. Until a hardware pass proves a routine pair,
J8 stays unmerged and the docs say memory access needs XBDM.

**D10. Safety in the CLI.** `jrpc call`, `jrpc shutdown`, `jrpc leds`, `jrpc constmem` are destructive
(they run code or change state on the console) and go through `confirmDestructive`. `constmem`
additionally warns that a registered task cannot be removed by this client. Read-only commands
(`resolve`, `cpukey`, `kernel`, `console-type`, `title-id`, `temp`) do not ask.

## Public API sketch (`include/protocols/jrpc/client.hpp`)

```cpp
namespace updclient::jrpc {

inline constexpr uint16_t kJrpcPort = 1409;

struct ClientOptions {
  std::chrono::milliseconds bannerTimeout{5000};   // whole banner line
  std::chrono::milliseconds idleTimeout{10000};    // inside a reply
  std::chrono::milliseconds callTimeout{60000};    // a called function may run long; 0 = none
  std::chrono::milliseconds byeTimeout{500};       // Bye gets no answer; bounds the write only
  size_t maxCommandBytes = kMaxCommandBytes;       // 8191
  size_t maxReplyBytes = 64 * 1024;
  size_t maxArrayElements = 8;                     // raised only if hardware shows more is sent
  bool silentOpBarrier = true;                     // D6
  TraceHook trace;                                 // same shape as xbdm::TraceHook
};

enum class ThreadContext { Title, System };
enum class ReturnKind { Void, Int, String, Float, Byte, IntArray, FloatArray, ByteArray, Int64 };
struct ByName { std::string module; uint32_t ordinal; };
struct Arg;                                        // closed variant, factory functions: Arg::i32(), ::u32(),
                                                   // ::boolean(), ::byte(), ::i64(), ::u64(), ::f32(), ::f64(),
                                                   // ::string(), ::bytes(), ::ints(), ::floats()
struct CallSpec {
  std::variant<uint32_t, ByName> target;
  ThreadContext thread = ThreadContext::Title;
  ReturnKind returns = ReturnKind::Void;
  size_t arraySize = 0;                            // only for the array kinds
  std::vector<Arg> args;
};
using CallValue = std::variant<std::monostate, uint64_t, std::string, double,
                               std::vector<int32_t>, std::vector<double>, std::vector<uint8_t>>;
struct CallResult { CallValue value; std::string line; };   // line: the reply as sent, for diagnostics

class UPDCLIENT_API JrpcClient {
 public:
  using Connector = std::function<Result<net::TransportPtr>()>;
  static Result<JrpcClient> connect(const net::Endpoint &, ClientOptions = {});
  static Result<JrpcClient> connect(const net::Endpoint &, ClientOptions, std::stop_token);
  static Result<JrpcClient> open(Connector, ClientOptions = {});     // + stop_token overload
  static Result<JrpcClient> attach(net::TransportPtr, ClientOptions = {}, Connector = {});

  bool isConnected() const noexcept;  std::string describe() const;
  std::optional<net::Endpoint> peer() const;
  Result<void> reconnect();  void close() noexcept;  void cancel() noexcept;   // thread-safe
  std::optional<CommandDelivery> lastDelivery() const;
  const ClientOptions &options() const noexcept;  void setOptions(const ClientOptions &);

  Result<CallResult> call(const CallSpec &);                        // the generic path, any kind
  Result<void> callVoid(const CallSpec &);  Result<int32_t> callInt32(const CallSpec &);
  Result<uint32_t> callUInt32(...);  Result<uint64_t> callUInt64(...);  Result<float/double> callFloat(...);
  Result<std::string> callString(...);  Result<std::vector<uint8_t>> callBytes(...);  // + ints, floats

  Result<uint32_t> resolveFunction(const std::string &module, uint32_t ordinal);   // 9
  Result<CpuKey> cpuKey();                                                         // 10
  Result<void> shutdown();                                                         // 11
  Result<void> notify(const std::string &text, uint32_t type);                     // 12
  Result<uint32_t> kernelVersion();                                                // 13
  Result<void> setLeds(LedState topLeft, LedState topRight, LedState bottomLeft, LedState bottomRight);  // 14
  Result<uint32_t> temperature(TemperatureSensor);                                 // 15, raw SMC value
  Result<uint32_t> currentTitleId();                                               // 16
  Result<ConsoleType> consoleType();                                               // 17, enum + the text
  Result<void> constantMemorySet(uint32_t address, uint32_t value, std::optional<...> onlyIf, std::optional<uint32_t> inTitle);  // 18

  Result<RawAnswer> rawCommand(const std::string &line);  // one line as typed, returns the reply line; diagnostics
};

UPDCLIENT_API std::optional<RemoteFault> remoteFault(const Error &) noexcept;
UPDCLIENT_API Result<IdentifyResult> identify(const net::Endpoint &, ClientOptions = {});   // connect, read banner, Bye
UPDCLIENT_API void registerJrpcScheme(net::TransportRegistry & = net::TransportRegistry::instance());
}
```

`LedState` and `TemperatureSensor` are enums with the raw wire value kept reachable
(`static_cast`) because the document gives only the meaning of the temperature index
(0 CPU, 1 GPU, 2 EDRAM, 3 mainboard) and nothing about LED encodings: `setLeds` takes the raw `int`
per LED until hardware shows the mapping. The temperature unit (raw `%X`) is likewise returned as
the raw number.

## Files

```
include/protocols/jrpc/protocol.hpp   constants, Arg, CallSpec, builders, reply parsers (no I/O)
include/protocols/jrpc/client.hpp     JrpcClient, ClientOptions, identify, registerJrpcScheme
include/protocols/jrpc/discovery.hpp  JrpcProbeProvider (J5, added to the plan)
include/protocols/jrpc/memory.hpp     MemoryAccess helper (J8, hardware-gated, not written)
src/protocols/jrpc/protocol.cpp  client.cpp  discovery.cpp  endpoint.hpp (private)  memory.cpp (J8)
tests/support/jrpc_mock_server.{hpp,cpp}  tcp_server_transport.hpp (shared with the XBDM mock)
tests/protocols/jrpc/                 wire_test, mock_server_test, client_fake.hpp, client_*_test,
                                      integration_*_test, client_fuzz_test
tests/cli/jrpc_cli_test.cpp
src/cli/jrpc.{hpp,cpp}                the `jrpc` group and the JRPC halves of shared commands
```

CMake globs pick the new files up (re-run CMake once); no CMake edit is needed.
`include/updclient.hpp` gets the new headers.

## Milestones

```
J1 wire: protocol.hpp/.cpp, builders and parsers, goldens from §7        independent
J2 mock: tests/support/jrpc_mock_server, written from the document        after J1 only for shared constants
J3 client core: connect/attach/open, banner, Bye, call(), errors, cancel  after J1, J2
J4 typed helpers and the system opcodes                                   after J3
J5 scheme + identify + umbrella header                                    after J3
J6 integration and fault tests over MemoryPipe and loopback TCP            after J4, J5
J7 CLI: Service::Jrpc, withJrpc, `jrpc` group, shared commands             after J4, J5
J8 MemoryAccess helper (hardware-gated, may stay unmerged)                 after J4
J9 docs: ARCHITECTURE, README, JRPC_PROTOCOL.md contract amendments        last
```

### J1. Wire module

`kMaxCommandBytes = 8191`, `kMaxArgs = 37`, `kMaxArrayElements = 8`, `Opcode` and `ReturnTag`
enums with the §4 numbers. `buildCommand(const CallSpec &)` and `buildOpcodeCommand(Opcode, args)`
return the line without terminator; `formatLine` adds CRLF (the client sends CRLF both ways, §9).
Reply parsers, one per `ReturnKind`, plus `parseCpuKey`, `parseConsoleType`, `isErrorLine`,
`isDebugLine`. Argument encoding per D5. Tests: byte-exact goldens for all six examples in §7
(7.1 to 7.6), every tag, boundaries (36/37/38 args, 8191/8192 bytes, `as` 8/9, uint32 above
`INT32_MAX`, NaN, string with NUL, empty string and empty blob), and parser fuzz (truncated
arrays, missing `;`, signs, embedded `\r`, huge numbers).

### J2. Mock server

Modeled on `XbdmMockServer` (per connection thread, `MemoryPipe` or loopback listener, command log,
fault injection) but with its own parser written to follow §2 and the server's behavior in the
document, not the client's builder: it scans for `\n` in an 8500-byte buffer and drops an over-long
line, keys arguments on the tag's first character, hex-decodes strings and blobs, and only then
dispatches. Console model: registered functions (`registerFunction(address, lambda(args) ->
MockReturn)` and by `module`+`ord`), a small module table for `ResolveFunction`, configurable CPU
key, kernel version, console type, title id, temperatures; a recorded list of notifies, LED writes
and constant-memory tasks. Replies are formatted exactly like §6 (unpadded `%X`, `%f` with six
decimals, `v,v;` lists, at most 8 elements). Switches: send or omit the banner, wrong banner, delay,
silent opcodes (D6) both ways, padded and unpadded CPU key, `error=` for a named function, reply
with the wrong shape, garbled or truncated line, over-long line, drop after N bytes, drop on a
specific command, hang forever, 8-slot limit with a held 9th connection, and an array reply that
loops past 8. `mock_server_test` exercises it with raw bytes so the mock is trusted before the
client uses it.

### J3. Client core

Banner read (`JRPC2 connected` exact, otherwise `Protocol`; a `DEBUG` line is `Unsupported`), the
idle/total timeout split from `XbdmClient`, a bounded line reader, `call()`, `Bye` on `close()`
without waiting for an answer, `cancel()` from any thread, `reconnect()`, `lastDelivery()`, trace
hook. Unit tests against a scripted `MockTransport`: split reads at every byte boundary, CR-only or
LF-only terminators from the console (Contract: accept CRLF, tolerate bare LF), a banner that
arrives with the first reply in one segment, timeout closes the connection, `error=` keeps it,
nothing is sent after close, a second concurrent call is refused, `maxReplyBytes`, cancel during a
long call.

### J4. Typed helpers and system opcodes

Every method in the API sketch, with D6 (barrier), D7 (delivery) and D8 (CPU key) behavior, and the
sign and width reinterpretation for the `callInt32`/`callUInt64` family. Tests per method against
both the scripted fake and the mock; `silentOpBarrier` on and off; `shutdown()` closes the client.

### J5. Scheme, identify, umbrella header

`registerJrpcScheme` (`SchemeTraits{.defaultPort = 1409}`; a bare `tcp://host` gets 1409 through
`withDefaultPort`, as XBDM does with 730). `identify()` connects, checks the banner and returns
`{endpoint, banner}`, which is how the CLI and a GUI test "is JRPC installed on this console"
without sending a command. Optionally a `JrpcProbeProvider` over a caller-supplied endpoint list,
like `XellProbeProvider`; not registered by default.

### J6. Integration tests

The client against the mock, every test over a `MemoryPipe` and over loopback TCP (the pattern in
`tests/protocols/xbdm/integration_*`): all return kinds, all opcodes, 37 arguments, the largest
line, 100 calls on one connection, two clients at once, the ninth connection, a drop mid-reply,
reconnect after a drop, cancel during a call that never returns, a call that outlives
`callTimeout`.

### J7. CLI

- `Service::Jrpc` in `context.hpp`; `explicitEndpoint`: a bare host for the `jrpc` group means
  `jrpc://`, port 1409; `--port` applies. `targetsJrpc()` like `targetsXbdm()`; the UpdServer and
  XBDM resolvers refuse a `jrpc://` target with a pointer to `updclient jrpc --help`.
  `resolveJrpcEndpoint()` requires an explicit target (no discovery) with a message saying so.
- `session.cpp`: `withJrpc(context, effect, destructiveAction, body)` mirroring `withXbdm`: options
  from `--timeout-ms`, `--trace` (the option text and the footer in `app.cpp` stop saying "XBDM"),
  the `InterruptScope` that cancels the client, delivery in the failure (`command_delivery`)
  only for console-changing commands, confirmation as in D10.
- `Failure` gets `remoteFault` (string) next to `consoleStatus`; `Output::error` and the JSON
  document carry it. Existing JSON fields do not change.
- `jrpc` group: `ping` (identify), `info` (kernel, console type, title id, CPU key, temperatures; each
  field empty if its command failed, as `ConsoleInfo`), `resolve <module> <ordinal>`, `cpukey`,
  `kernel`, `console-type`, `title-id`, `temp [cpu|gpu|edram|board|all]`, `notify <text> [--type n]`,
  `leds <a> <b> <c> <d>`, `shutdown`, `constmem ...`, `call` (below), `raw <line>`.
- `jrpc call <addr|module!ordinal> [--system] [--returns void|int|str|float|byte|int64|ints|floats|bytes]
  [--count n] [--arg i32:5 --arg u32:0xFFFFFFFF --arg str:hi --arg bytes:DEADBEEF --arg f32:1.5 ...]`.
  Prefix syntax is `type:value`; numbers use the existing 0x/decimal parser in `args.cpp`; the JSON
  result is `{"type":..., "value":..., "line":...}`.
- Shared commands: `info` and `power shutdown` take the JRPC path on a `jrpc://` target. `mem`, `file`
  and `nand` refuse it with a message saying JRPC has no memory or file commands and that XBDM does.
- tests/cli/jrpc_cli_test.cpp runs the executable against the mock: text and JSON output, exit codes
  (usage 2, remote error 1), `--yes` handling, refused combinations, the Ctrl-C path where the
  existing XBDM CLI tests already cover it.

### J8. MemoryAccess (gated)

Not started until the hardware pass (below) names a working routine pair. Shape:
`MemoryAccess(JrpcClient &, MemoryRoutines)` with `write(address, bytes)` as a copy call whose
source is a blob argument (the server owns the scratch buffer, so one call per at most
~3.9 KB of data after hex doubling and the line limit), and `read(address, length)` in pieces of
at most 8 bytes through the array-return path. Tests use a mock whose registered "copy" routine
actually writes the memory map. If no pair is found, this milestone is dropped and the README
says so.

### J9. Docs

ARCHITECTURE.md (layout, the protocols list, a `jrpc` row in the layer table, the test list,
the CLI file list), README (protocol table, CLI table, library example, error codes for
`remoteFault`), EXTENDING.md only if a convention changed (it should not have), and
JRPC_PROTOCOL.md amended where this plan fixed a `Contract:` that the document leaves implicit
(sections 3 and 6 for the argument and reply rules, section 9 for D6).

## Verification

Per commit: configure and build as in the build memory, then `ctest`. Dependency rules are
checked with the `grep -r '#include' include src` check from ARCHITECTURE.md (no OS socket header
or spdlog under `include/`, no cross-protocol include). Sanitizer build (ASan/UBSan) over the
JRPC tests, since the reply parsers take untrusted text.

## Hardware checklist (decides defaults; none of it blocks J1 to J7)

1. Banner and port: connect to 1409, read `JRPC2 connected\r\n`.
2. Do opcodes 11, 12, 14 and 18 answer? What do void calls answer (`0` or `S_OK`)? Decides D6.
3. Is the CPU key padded to 8+8 digits? (D8)
4. What does the server do with `as` above 8? (open question 1) Decides `maxArrayElements`.
5. What does a failed resolve or a bad argument line answer on the TCP build? (D4 texts)
6. Does a long-running call block other connections? Timeout behavior at the 9th connection.
7. LED encoding, temperature units, and whether `type=18` tasks can be cancelled. Decides the
   `setLeds`/`temperature` types and the `constmem` warning.
8. Find a pointer-returning routine and a copy routine for J8, or drop J8.

## Risks

- The whole protocol picture rests on one binary and a sister client. Any wrong guess is isolated
  behind an option or a milestone that can be dropped; the plan's order puts the hardware-dependent
  parts (J8, defaults of D6 and `maxArrayElements`) last.
- The mock encodes the same document as the client. Mitigations: the mock's parser is written to
  the server's behavior (first-character tags, 8500-byte buffer, `sscanf` semantics), the goldens are
  the document's own examples, and the first hardware session runs the J6 call set against the
  console through `jrpc call`/`raw` before anything is trusted.
- `call` is arbitrary code execution on the console. The CLI confirms it, the library does not
  second-guess it, and `lastDelivery()` plus a closed connection on timeout tell a GUI when a call
  may still be running.

## Implementation record

What the milestones recorded as they were built: the choices the plan left open, and every place the
code differs from the plan. Nothing here is a hardware fact; each item marked "unverified" is a candidate
for the hardware checklist. The binding text of the wire rules is in JRPC_PROTOCOL.md (the `Contract:`
paragraphs of sections 3, 6 and 9, added in J9) and in the comments of `protocol.hpp` and `client.hpp`.

### Status

| Milestone | State |
|---|---|
| J1 wire module | implemented (`protocol.{hpp,cpp}`, `wire_test.cpp`) |
| J2 mock server | implemented (`tests/support/jrpc_mock_server.*`, `mock_server_test.cpp`) |
| J3 client core | implemented (`client.{hpp,cpp}`, `client_*_test.cpp`) |
| J4 typed helpers and system opcodes | implemented |
| J5 scheme, `identify`, umbrella header | implemented, plus `JrpcProbeProvider` |
| J6 integration and fault tests | implemented (`integration_*_test.cpp`, 52 tests each, each over a pipe and over loopback TCP) |
| J7 CLI | implemented (`src/cli/jrpc.*`, `tests/cli/jrpc_cli_test.cpp`) |
| J8 MemoryAccess | not started; waits for the hardware pass |
| J9 docs | implemented (this record, ARCHITECTURE, README, JRPC_PROTOCOL contract paragraphs) |

### Deviations from the plan

- **D6 barrier is `ConsoleType` (opcode 17), not `GetKernelVersion` (J4).** With a decimal barrier, an
  opcode that answers `0` or any digits-only hex looks like the barrier's reply, and the real reply is left
  in the stream. The seven console names cannot be mistaken for `S_OK`, hex or `error=`. The barrier is
  pipelined behind the opcode, so both lines may reach the server in one `recv`; that relaxes D3's
  one-command-in-flight rule for opcodes 12, 14 and 18 only. Unverified: that opcode 17 answers and that
  the server handles two lines in one `recv`.
- **CPU key length (D8, J1/J4).** The plan says exactly 32 digits; the document's own example (7.6) has 16.
  `parseCpuKey` accepts 16 or 32 and returns `CpuKey` (big-endian bytes and `hex()`). Any other length is
  `Protocol` with the raw text; when the text is hex alone the connection is kept (J4), a deliberate
  departure from D3's close-on-wrong-shape rule, so that `jrpc info` does not lose the connection on a
  console with unpadded output. Unverified: which length is real.
- **`ReturnKind::Uint64Array` exists (J1)** so a 64-bit array request can be refused as `Unsupported`; the
  plan's enum sketch omitted it.
- **uint64 above `INT64_MAX` is sent as the equal negative int64 (J1)**, mirroring the uint32 rule; the
  plan said only "tag 8 as decimal".
- **Typed helpers (J4).** A helper accepts a spec whose return kind is `Void` (unset) or its own kind, and
  puts its own kind on the wire; any other kind is `InvalidArgument` with nothing sent. `callByte` and
  `callInt64` were added so every non-array kind has a helper. `callFloat` and `callFloats` return
  `double`, because the server prints `%f` with six decimals.
- **`LedState` and `TemperatureSensor` are enums (J4).** `LedState` is Off 0, Red 0x08, Green 0x80, Orange
  0x88, taken from the JRPC2 client and unverified; any raw value is reachable with `static_cast`. The
  plan said both "enum" and "raw int"; both are true. `temperature()` refuses a sensor above 3.
- **`consoleType()` returns `Result<ConsoleType>` only** (the plan said enum and text); `consoleTypeName()`
  gives the text. `kernelVersion()` returns `uint32_t`; a negative `%d` is `Protocol`.
- **`notify()` refuses empty text; `constantMemorySet()` refuses address 0.** Both go through
  `Arg::u32`, so values above `INT32_MAX` go out negative.
- **`shutdown()` ignores `silentOpBarrier`** (as D6 intends): it sends opcode 11 and closes without
  waiting and without `Bye`.
- **`JrpcProbeProvider` and `discovery.hpp`** were added in J5 (the plan said "optionally"). Unsupported
  from the registry (an unregistered scheme) is treated as "not JRPC", like a DEBUG line.
- **`identify()`** sends nothing but `Bye`; `IdentifyResult::banner` is always `kBanner` because the client
  accepts nothing else. A private `src/protocols/jrpc/endpoint.hpp` holds the default-port helper.
- **`TcpServerTransport` moved (J2)** out of `xbdm_mock_server.cpp` into
  `tests/support/tcp_server_transport.hpp`, unchanged, so both mocks share it.
- **Trace hook (J3).** It has the XBDM shape (`event`, `text`, `bytes`) with only `Sent` and `Received`
  events and `bytes` always 0.

### Wire module decisions (J1)

- Error codes: more than 37 arguments, an `as` above the limit and an over-long line are `LimitExceeded`;
  NaN or infinity, NUL or non-UTF-8 strings, a bad module name, address 0, an `arraySize` of 0 for an
  array kind or non-zero for a scalar are `InvalidArgument`; `Uint64Array` is `Unsupported`.
- The line limit (8191) applies to the command without CRLF; `formatLine` adds 2 bytes, still under the
  server's 8500-byte buffer. Oversized payloads are refused before hex encoding.
- By-address call at address 0 is refused (0 marks by-ordinal). Module names are non-empty printable ASCII
  without space, quote or backslash.
- Empty string (`2/0\\`), empty blob and empty int or float arrays are encodable; whether the server
  tolerates them is unverified.
- Floats use `std::to_chars` general format, precision 9 (f32) or 17 (f64): locale independent, equal to
  `%.9g` and `%.17g`. Float array elements may be NaN or infinity, being raw bytes.
- Reply parsing: byte replies and byte-array elements take 1 to 8 hex digits and keep the low 8 bits;
  `parseVoidReply` accepts `S_OK` or 1 to 16 hex digits; strings reject NUL, CR, LF; floats use
  `from_chars` (so `inf` and `nan` spellings pass, `+` and spaces do not); an unknown console type name is
  `Protocol`; array replies must hold exactly `as` values and end in `;`.
- Parsers take a line without terminator (`stripTerminator`); the caller checks `isErrorLine` and
  `isDebugLine` first. Every parser failure is `Protocol`.
- Small helpers beyond the plan: `isKnownOpcode`, `isReadOnly`, `mayBeSilent`, `opcodeName`,
  `consoleTypeName`, `errorText`, `stripTerminator`, `returnTag`, `isArrayKind`, and a public
  `encodeArgument`.

### Mock decisions (J2)

- Request parser: scans for LF in an 8500-byte buffer (8500 bytes with the LF accepted, 8501 dropped),
  keys arguments on the tag's first character, accepts `/` and `\` interchangeably, reads numbers like
  `sscanf %i` (0x prefix, octal, trailing junk ignored; `Clamp` or `Wrap` for values that do not fit;
  default `Clamp`, which exposes a client that sends a uint32 above `INT32_MAX` as-is). A value with no
  digits is a malformed-params error. More than 37 arguments, tags 5, 6 and 9 on the wire, a count that
  does not match the hex length and an odd number of hex digits are refused.
- Unspecified error texts are mock choices: `error=The paramaters were not found` (typo kept) for bad
  params, a missing type or params block, an unknown type, wrong opcode arguments;
  `error=Version mismatch` for a missing or wrong `ver`; `error=Unknown command` for a line that does not
  start with `consolefeatures` (invented); `error=Could not resolve function address, params = <p>, <t>`
  for an unknown address or export. The first three and the last come from D4 and section 6 and belong
  to the JRPC2 build. Each failure kind has a policy option (error line, silent, close).
- Defaults where the document is silent: opcodes 11/12/14/18 send nothing (options for `S_OK` or `0`); a
  void call answers the `%X` of r3; an over-long line is dropped without a reply and the connection
  resynchronises at the next LF; an opcode 15 index outside 0 to 3 answers 0; opcode 19 is accepted with no
  reply and no effect.
- CPU key: two halves, each `%X`, padded to `cpuKeyDigits` (default 8, giving the document's 16 digits);
  16 gives 32 digits; `cpuKeyPadded=false` gives the bare form. `%llX` is never padded, so example 7.3's
  `0000000248173A00` comes out as `248173A00`. A byte return is `%X` by default, `%02X` as an option.
  Arrays print `min(as, 8)` values by default (`Truncate`), or `Loop`; a fault can force any count.
- Connections: numbered in arrival order; the 9th waits without a banner until a slot frees, FIFO; an
  abandoned waiting connection keeps its place and is served and dropped when its turn comes.
  `setOptions()` applies to the next line that arrives. `Bye` must be `Bye` after trimming (bare LF
  tolerated).
- Faults: drop (after N bytes or at once), delay, stall, silence, replaced reply, `error=` line, DEBUG
  line, oversized and endless lines, trickle, deterministic hostile mangling, trailer bytes, array element
  count, skipped command, greeting faults; each targetable by type, address, text, connection, skip and
  repeat counts. `silence()` stays armed until `clearFaults()`.
- Float arguments are kept as a double (the precision the server keeps is unknown). The raw-byte test
  client closes its transport in its destructor because `MemoryTransport` does not close on destruction.

### Client decisions (J3, J4)

- Banner: exactly `JRPC2 connected`, CRLF or bare LF. A DEBUG line is `Unsupported`, anything else
  `Protocol`, none `Timeout` (with a hint about the 8 connection limit).
- Terminators: CRLF accepted, a bare LF tolerated, exactly one trailing CR stripped; a lone CR is not a
  terminator and ends in `Timeout`.
- Banner and a reply in one segment: the stray bytes stay buffered and the first `call()` closes the
  connection with `Protocol` without sending anything, because accepting them as that call's answer could
  hand garbage to the caller of a console-changing call.
- A concurrent second call (other thread, or re-entered from the trace hook) is refused with
  `InvalidArgument` through an atomic busy flag, leaving the call in progress undisturbed; `reconnect()` is
  refused the same way. `close()` from another thread during a call acts as `cancel()`.
- Timeouts: `bannerTimeout` bounds the whole banner. `callTimeout` bounds the first byte and the whole
  call (0 waits for the first byte without bound; a silent console then waits forever by design);
  `idleTimeout` bounds gaps and writes once the call is under way.
- `error=` is classified before the DEBUG check, so `error=...DEBUG...` stays a remote fault. A string
  result containing `DEBUG` closes the connection with `Unsupported`; this follows section 6.
- `remoteFault()` requires code `Io`, `sysError` in 1000 to 1003 and the marker
  `console answered error=` in the message, so OS error numbers of 1000 or more are not misread.
  `RemoteFault`: Other 0, CouldNotResolve, VersionMismatch, ParametersNotFound.
- `lastDelivery()`: `beginCommand()` runs at the start of every public call, so a refused build or a call
  on a closed client reports `NotSent`. A reply line, `error=` and wrong-shape lines included, makes it
  `Answered`. A failed call that was `Sent` or `PartlySent` gets "; the command was sent but not answered,
  so the console may have carried it out" appended. `reconnect()` leaves the record alone. For opcodes
  12, 14, 18 with the barrier, `Answered` means the line that ended the wait was read; the barrier is
  never the "last command".
- `rawCommand`: printable ASCII only (no LF/CR smuggling), non-empty, at most `maxCommandBytes`, exactly one
  reply line expected; an `error=` line is an answer; a DEBUG line is `Unsupported` and closes.
- `Bye` is written even with stray unread bytes buffered; skipped when closed, never greeted, dropped,
  cancelled, or already closed. The destructor, move-assignment over a live client and `reconnect()` (for a
  healthy old connection) write it.
- Types `RemoteFault`, `Delivery`, `CommandDelivery`, `RawAnswer`, `TraceEvent`, `TraceHook` are defined
  again in `jrpc` (the layer rule forbids reusing XBDM's). The session is held by `unique_ptr`;
  `isConnected()` and `peer()` lock the transport mutex.
- Opcode error handling: an `error=` answer is `Io` with `remoteFault()` set and keeps the connection; a
  failed barrier (timeout, drop) is reported with the opcode's delivery and the "may have carried it out"
  note; `shutdown()` ends as `Sent`.
- The longest `notify` text that fits the 8191 byte line is 4066 bytes.

### Integration findings (J6)

No client or mock defect was found. Known limitation, not fixed: a reply has no sequence number and the
client does not poll the socket for unread bytes before it sends. If stray bytes from the console reach the
client only after the next command is sent (a trailing line in a separate TCP segment), they are read as
that command's answer and the real answer sits in the stream. Stray bytes in the same read as a reply are
detected. A poll before each send would cost about 1 ms per call and still could not close the
late-arrival case. `checkCleanTraffic` allows pipelining only for `notify`, `setLeds`,
`constantMemorySet` and `shutdown`, the one place D3 is relaxed.

### CLI decisions (J7)

- `--timeout-ms` sets `bannerTimeout`, `idleTimeout` and `callTimeout` together (0 for none), so a silent
  console fails in the time the user gave; a long-running `jrpc call` needs a larger value or 0.
- Destructive (confirm or `--yes`): `call`, `raw`, `shutdown`, `leds`, `constmem`. `raw` was added to D10's
  list, matching `xbdm raw`. `notify` is not confirmed but is marked as changing the console, so a failure
  reports `command_delivery`. Read-only commands do not ask and report no `command_delivery`.
- `call` validates the whole request before connecting or confirming (target, `--returns`, `--count`, every
  `--arg`, a trial `buildCommand`); a request that cannot be built is a usage error (exit 2) and sends
  nothing, except an over-long line, which is `LimitExceeded` (exit 1). `--arg` is `type:value`
  (`i32`, `u32`, `bool`, `byte`, `i64`, `u64`, `f32`, `f64`, `str`, `bytes`, `ints`, `floats`); `i32`
  accepts only the int32 range, so `0xFFFFFFFF` needs `u32`. Array returns need `--count` 1 to 8, and
  `--count` with a non-array return is a usage error. The result JSON is `{target, type, value, line}`;
  int, byte and int64 values are the unsigned register, bytes an uppercase hex string, void `null`.
- `info` asks kernel, console type, title id, CPU key and four temperatures in turn. An unanswered field
  is `null` in JSON and `(not answered)` in text, with a warning on stderr; if the connection is lost the
  rest are not asked; if nothing answered, the first error is returned; an unpadded CPU key leaves only
  that field null.
- `ping` uses `identify` and sends only `Bye`; JSON `{target, banner}`. `shutdown` JSON is `{action, target,
  acknowledged:false}`. `raw` prints an `error=` reply as an answer, exit 0, `{command, reply, is_error}`.
- `leds` takes `off|red|green|orange` or a raw number per LED; `notify --type` defaults to 0; `temp` takes
  `cpu|gpu|edram|board|all` and prints the raw value; `constmem` warns that the task cannot be removed.
- `Failure.remoteFault` is filled by `fromError` through `jrpc::remoteFault` (names `could_not_resolve`,
  `version_mismatch`, `parameters_not_found`, `other`) with `sysError` 0; `Output::error` emits
  `remote_fault` only when set. The UpdServer, XBDM and XeLL resolvers refuse a `jrpc://` target;
  `mem`, `file`, `nand`, `version`, `power reboot` and `smc-reset` are refused with a message that JRPC has
  no such commands. `TraceFile::open` takes a protocol name. The `--port` and `--trace` help texts and the
  footer now mention JRPC.

### Hardware checklist, additions

On top of the list above: that opcode 17 answers (the barrier sentinel); that the server handles two lines
in one `recv`; whether the CPU key is 16 or 32 digits; whether the byte return is masked or padded;
whether empty strings, blobs and arrays are accepted; whether the server keeps a `float` or a `double`;
the `error=` wording on the TCP build; the `LedState` values; and whether a console that stays silent on
opcodes 12, 14 and 18 makes `silentOpBarrier` worth turning off.
