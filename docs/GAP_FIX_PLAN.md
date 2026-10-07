# Plan: seven gaps in the XBDM client, the TCP transport and discovery

Status: plan only; nothing here is implemented yet. Line numbers refer to commit `ad83582`.

## Ground rules

- **No public API change.** Nothing under `include/` changes signature, type, layout or exported
  symbol: no new public functions, overloads, fields, enum values or `friend` declarations. All new
  code lives in `src/` (private headers next to `net/deadline.hpp` and `net/platform/`, the
  `detail::Session` and `FileWriter::State` structs that are defined in `client.cpp`, and the
  `XbdmDiscovery::Search` struct defined in `discovery.cpp`). The private members of
  `XbdmDiscovery` declared in `include/protocols/xbdm/discovery.hpp:79-86` (`Search`, `run`,
  `resolveNames`, the three data members) keep their declarations.
- **Shared builds.** `updclient_lib` builds with `CXX_VISIBILITY_PRESET hidden`
  (`CMakeLists.txt`), so the CLI can only call `UPDCLIENT_API` symbols. Anything the CLI shares with
  the library goes into a header-only private header under `src/` (the CLI target already has `src/`
  on its include path); nothing new is exported.
- **Behavior-compatible.** Every success path that works today keeps working. The intended
  differences are listed per gap under "Behavior change" and gathered in the last section. Error
  codes stay as they are; error messages may gain suffixes (messages are not a contract, tests match
  substrings).
- **Header comments.** Some comments in public headers describe behavior that changes (listed per
  gap). The default here is to leave `include/` byte-for-byte unchanged and document the new
  behavior in `README.md`, `docs/EXTENDING.md` and `docs/XBDM_PROTOCOL.md`; a separate, droppable
  commit refreshes the comments if comment-only edits are acceptable (open question 1).

Two gaps cannot be closed fully without an API change. That is said explicitly where it applies
(gaps 2, 5 and 7), together with the closest compatible option and the API that would close it.

## Order and dependencies

```
C1 transport: cancellable TCP connect (internal) ──┬─> C2 transport: host names (gap 4)
                                                   └─> C3 XBDM: reconnect cancel (gap 7)
C4 XBDM: discovery port and cancellation (gap 5)        independent
C5 XBDM: case-only rename (gap 6)                       independent
C6 XBDM: delivery of a failed command (gap 3) ──> C7 XBDM: upload replace rule (gap 1)
                                                   └─> C8 XBDM: kept upload note (gap 2)
C3..C8 ──> C9 CLI: JSON fields, Ctrl-C for discovery and cleanup, discovered port
C1..C9 ──> C10 docs
(C11 docs: public header comments, only if open question 1 is answered yes)
```

- C1 before C2 and C3: both wait on the connect's wake-up signal that C1 introduces.
- C6 before C7 and C8: `FileWriter::finish()` reports its delete and rename failures through the
  delivery notes of C6, and C8 formats the kept-upload note with the same private helper.
- C4 and C5 touch separate code and can land at any point.
- C9 only consumes what C3 to C8 add; C10 documents the end state.

---

## Gap 1: `FileWriter::finish()` always replaces

### Root cause

- `XbdmClient::openWrite` (`src/protocols/xbdm/client.cpp:1514-1554`) sends `sendfile` to a
  temporary name without looking at the final name, so the writer has no idea whether the caller
  meant to replace something.
- `FileWriter::finish()` (`client.cpp:963-1032`) looks the final name up only after the data was
  confirmed (`lookUp`, `client.cpp:1008`) and deletes whatever it finds there (`client.cpp:1010-1024`)
  before the rename (`client.cpp:1026-1031`). A file that appears after the caller checked for one
  (for example, after an app's "Replace?" prompt said nothing was there), or during the upload, is
  deleted and replaced without a word. The public contract says so (`include/protocols/xbdm/client.hpp:259-262`,
  "deleting a file of that name first"), which is exactly the gap.
- The window between the `lookUp` and the `rename` inside `finish()` is a second, smaller race: the
  protocol has no "rename unless the target exists", and whether the console's `rename` refuses or
  replaces an existing target is **[none]** (`docs/XBDM_PROTOCOL.md` 3.10).

### Fix

1. `FileWriter::State` (`client.cpp:874-906`) gains `bool finalExistedAtOpen` (and, for open
   question 3, the `std::optional<FileAttributes>` seen then).
2. `openWrite` calls `lookUp(session, *canonical, false)` after the argument checks and before the
   `sendfile` line (between `client.cpp:1532` and `1533`). A transport failure fails `openWrite` with
   nothing created; a refusal or "not found" means "did not exist" (`lookUp` already maps both,
   `client.cpp:680-701`); a folder of that name fails right away with the `InvalidArgument` that
   `finish()` gives today (`client.cpp:1011-1013`), so a doomed upload never sends its data.
3. `finish()` keeps its structure but decides with the snapshot:
   - lookup now finds nothing: rename directly (unchanged).
   - lookup finds a file and `finalExistedAtOpen`: delete, then rename (unchanged; this is the
     replace the caller asked for).
   - lookup finds a file and `!finalExistedAtOpen`: do not delete. Remove the temporary file on the
     same connection (`removeTemporary`, as for every failure where nothing was deleted) and fail with
     `InvalidArgument`, message `"sendfile <path>: <path> appeared during the upload and was not
     replaced; the upload was removed"`. Same code family as `rename()`'s "already exists"
     (`client.cpp:1466`).
   - the final rename is refused with 410 (or any refusal) while `!finalExistedAtOpen`: the target
     appeared inside the last window; same handling as the previous case (temporary file removed,
     final name untouched).
4. The documented residual: a file that appears between the `lookUp` in `finish()` and the `rename`
   on a console whose `rename` silently replaces cannot be detected. The window shrinks from "the
   whole upload" to one round trip. Add this to `docs/XBDM_PROTOCOL.md` section 6 as a hardware
   question ("does `rename` onto an existing name answer 410, or replace?").

### Files

`src/protocols/xbdm/client.cpp`; tests in `tests/protocols/xbdm/client_transfer_test.cpp` and
`tests/protocols/xbdm/integration_transfer_test.cpp`; docs in C10.

### API compatibility

Only `FileWriter::State` (defined in the `.cpp`) and function bodies change. `openWrite`,
`finish`, `uploadFromFile` keep their signatures. The public comment at `client.hpp:259-262` stays
true for every file that existed when the upload began; the narrowing is documented in C10 (and in
C11 if allowed).

### Behavior change

- One extra command (`getfileattributes`, or `dirlist` of the parent on a console that answers 407)
  before every `sendfile`.
- A file that did not exist at `openWrite` is never deleted by `finish()`; the call fails with
  `InvalidArgument` instead of replacing it.
- A folder at the final name fails in `openWrite` instead of after the data.

### Risks

- Scripted tests assert exact command sequences (`client_transfer_test.cpp:330-335`, `357-358`):
  they gain the lookup as their first command; `UploadReplacesAnExistingFile` expects
  `commands().size() == 5` and the delete at index 3.
- The `dirlist` fallback on consoles without `getfileattributes` makes `openWrite` cost a folder
  listing; bounded by `maxBodyBytes` like every listing.
- Policy for a file that existed at open but was replaced by another one during the upload: the
  default compares existence only (it is replaced). Comparing `createdFileTime` is open question 3.

### Tests

- `client_transfer_test.cpp`: new `AFileThatAppearsDuringTheUploadIsNotReplaced` (the `Files` fake
  creates the final name from inside the `sendfile` data handler; `finish()` is `InvalidArgument`, the
  other file is intact, the temporary file is deleted, the connection stays usable,
  `pendingCleanup()` is empty).
- Same file: `AFileThatAppearsBeforeTheRenameIsNotReplaced` (the fake answers the lookup with 402,
  then creates the file and answers the rename with 410).
- Same file: `UploadOntoAFolderFailsBeforeTheData` replaces the post-data expectations of
  `UploadOntoAFolderFailsAndRemovesTheTemporaryFile` (no `sendfile` is sent).
- Update the sequence assertions of the existing upload tests; `UploadReplacesAnExistingFile` and
  `AFailedRenameAfterTheOldFileWasDeletedKeepsTheUpload` must still pass unchanged otherwise.
- `UploadFindsTheFileToReplaceInTheListingWithoutGetfileattributes`: the listing is now read in
  `openWrite`, and again in `finish()`.
- `integration_transfer_test.cpp`: against the mock, a second client creates the final name while
  the first is uploading; the first upload fails and the second client's file survives.

---

## Gap 2: a kept upload is reported only in error text

### Root cause

`finish()`'s `giveUp` (`client.cpp:997-1006`) appends `"; the upload is kept as <temp>, since <path>
was deleted"` (or "may have been deleted") to `Error::message` and nothing else. `Error`
(`include/core/error.hpp:27-31`) has only `code`, `message` and `sysError`; the code and `sysError`
are those of the delete or rename that failed, so they look like any other failure. After the
failure `FileWriter::temporaryPath()` returns the name whether the file was kept, deleted, or queued
for deletion, and `pendingCleanup()` (`client.hpp:357`) does not list kept files by design, so no
public observable tells a caller "your data is on the console under this name". The CLI
(`src/cli/xbdm.cpp:287-302`, `xbdmSend`) and its JSON error document (`src/cli/output.cpp`,
`Output::error`) show only the message.

### Can it be fixed without an API change?

Not as a true library-level flag. Every compatible carrier is either already used (`code`,
`sysError`), or would change the meaning of an existing accessor. The options:

| Option | API change? | Verdict |
|---|---|---|
| A. New field in `Error`, new `ErrorCode`, or a new `keptUpload(const Error&)` like `consoleStatusCode` | yes | Out of scope; the clean fix if the rule is relaxed (open question 2) |
| B. `FileWriter::temporaryPath()` returns empty after `finish()` unless the file was kept | no signature change, but a semantic change of an accessor callers may already use to name an aborted upload | Not by default; offered as an alternative |
| C. A stable, documented marker in `Error::message`, produced and parsed by one private header-only helper, and an explicit field in the CLI's JSON | no | **Chosen** |

### Fix (option C)

1. Add `src/protocols/xbdm/error_notes.hpp` (private, header-only, `namespace
   updclient::xbdm::detail`):
   - `void noteKeptUpload(Error &, std::string_view temp, std::string_view final, bool deleted)`:
     appends exactly today's text, so existing messages and the tests at
     `client_transfer_test.cpp:379` and `409` stay valid.
   - `std::optional<std::string> keptUpload(const Error &)`: finds the last
     `"; the upload is kept as "` and takes the text up to the `", since "` that is followed by the
     final path and `" was deleted"` / `" may have been deleted"` at the end of the message (anchored
     at the end, so commas in console names do not confuse it).
2. `giveUp` (`client.cpp:997-1006`) calls `noteKeptUpload` instead of composing the text inline.
3. CLI (C9): `Failure` (`src/cli/context.hpp:308-315`) gains `std::optional<std::string>
   keptUpload`; `fromError` (`src/cli/context.cpp`, `fromError`) fills it through `keptUpload()`;
   `Output::error` writes `"kept_upload": "<console path>"` into the JSON error object (additive),
   and text mode logs one warning: `The upload is kept on the console as <temp>; rename it with
   'xbdm mv'`.
4. Document the marker in `README.md` (XBDM section) as the supported way for library users to
   detect a kept upload until an API can carry it.

### Files

`src/protocols/xbdm/error_notes.hpp` (new), `src/protocols/xbdm/client.cpp`, `src/cli/context.hpp`,
`src/cli/context.cpp`, `src/cli/output.hpp`, `src/cli/output.cpp`; tests below; docs in C10.

### API compatibility

No public symbol, type or signature changes; the helper is header-only and private. The message text
is unchanged. The CLI JSON gains an optional key, which scripts that ignore unknown keys accept.

### Risks

- Library users still parse a message; the marker becomes a de facto contract and must not be
  reworded without updating the helper (the helper is the only producer).
- `Output::error`'s signature is CLI-internal; changing it touches every caller in `src/cli/`.

### Tests

- `client_transfer_test.cpp`: the existing kept-upload cases also assert that a message built by
  `noteKeptUpload` is parsed back to the same temporary name, including a final name containing
  `", since "`. This needs `src/` on the test target's include path
  (`tests/CMakeLists.txt`, `target_include_directories(updclient_tests PRIVATE ... ${PROJECT_SOURCE_DIR}/src)`);
  header-only, so it works with shared builds too. If that include is unwanted, cover it through the
  CLI test only.
- `tests/cli/xbdm_cli_test.cpp`: `xbdm`-group upload over an existing file with the mock told to
  refuse the rename (`XbdmFault` on `rename`); `--json` gives `error.kept_upload` equal to the
  temporary name, and the mock still holds it.

---

## Gap 3: no way to tell whether a mutating command reached the console

### Root cause

- Every command goes through `Session::request` (`client.cpp:396-402`): `checkUsable`, then
  `sendLine` (`client.cpp:296-301`, which calls `sendBytes`/`writeAll`, `client.cpp:282-286`), then
  `readStatus`. A failure in any of the three ends in `transportFailure`/`drop`
  (`client.cpp:206-217`, `254-266`) with `Disconnected`, `Timeout`, `Cancelled` or `Io`, whether it
  happened before the first byte left or after the whole line was sent and only the answer was lost.
- `ITransport::writeAll` (`include/net/transport.hpp:577`) does not report how much of a failed
  write went out, so even the session cannot tell "nothing sent" from "half a line sent".
- Only two places reason about it today, by hand: `openWrite` queues the temporary name when the
  answer to `sendfile` is lost (`client.cpp:1537-1541`) and `finish()` notes "may have been deleted"
  (`client.cpp:1017-1020`). `mkdir`, `delete`, `rename`, `setmem`, `setsystime`, `dvdeject` and
  `rawCommand` (`client.cpp:1421-1472`, `1640-1646`, `1716-1727`, `1793-1806`, `1872-1908`) say
  nothing.

### Can it be fixed without an API change?

A typed flag cannot (same reasons as gap 2: `Error` has no room, and remapping `ErrorCode`s, for
example `NotConnected` for "not sent", would change codes callers already branch on). The
compatible route is the same as gap 2: a stable note in the message, produced and parsed by the
private helper, plus an explicit field in the CLI JSON.

### Fix

1. `Session` gains `enum class Delivery { None, NotSent, Partial, Sent, Answered }` and a member
   `delivery`, reset to `None` by `checkUsable` and set:
   - `NotSent` when `checkUsable` fails or the write fails before any byte of the line was accepted;
   - `Partial` when the write fails after some bytes (some consoles end a line at CR, spec 1.9, so a
     partial line is treated like a sent one);
   - `Sent` once the whole line was written;
   - `Answered` once a status line was read.
2. To know the partial count, `sendLine` writes the command line with its own `writeSome` loop
   (`transport->writeSome`, same `arm()` and timeouts) instead of `writeAll`. Binary upload data keeps
   `sendBytes`.
3. `error_notes.hpp` gains `noteDelivery(Error &, Delivery)`, appending
   `"; the command was not sent"` or `"; the command was sent but not answered, so the console may have
   carried it out"`, and `std::optional<Delivery> deliveryOf(const Error &)`.
4. A helper `mutating(Session &, Result<...>)` in `client.cpp` applies the note when the result is
   an error that is not a refusal (`isRefusal`, `client.cpp:614-616`). Used by `makeDirectory`,
   `removeFile`, `removeDirectory`, `rename`, `ejectTray`, `setSystemTimeRaw`, `setMemory` and
   `rawCommand`, and by `finish()`'s delete and rename (which then derives its "may have been
   deleted" from `Delivery` instead of `!isRefusal`).
5. `setMemory` (`client.cpp:1793-1806`) also says which part is known written: `"; 0x.. bytes from
   0x........ were written before this piece"`.
6. `power()` (`client.cpp:1194-1215`) is unchanged: a closed connection after the line is already
   reported as `PowerResult::ConnectionClosed`.
7. CLI (C9): `Failure` gains `std::optional<std::string> delivery`, set by `fromError` from
   `deliveryOf()`; JSON error gets `"command_delivery": "not_sent" | "unknown"` (`Partial` and
   `Sent` both map to `unknown`). Text mode adds one line: "The console may have carried the command
   out; check before repeating it" for `unknown`.

### Files

`src/protocols/xbdm/client.cpp`, `src/protocols/xbdm/error_notes.hpp`, CLI files as in gap 2; tests
below; docs in C10.

### API compatibility

`detail::Session` is defined in the `.cpp` and owned through `shared_ptr`; its layout is private.
Codes, `sysError` and `consoleStatusCode()` are unchanged (the note never contains
`"console answered 4"`, which `consoleStatusCode` looks for, `client.cpp:779-788`).

### Risks

- Message suffixes change text seen by users and by the trace-free logs; tests matching whole
  messages need updating (none found that compare a full message for these commands).
- The `writeSome` loop must keep the idle-timeout and command-deadline semantics of `sendBytes`;
  reuse `arm()` per iteration exactly as `fill()` does.

### Tests

- `tests/protocols/xbdm/client_commands_test.cpp`: with `FakeConsole`, (a) the connection closed
  before the call: `removeFile` fails and `deliveryOf == NotSent`; (b) `hangUp()` when the
  `delete` line arrives: `Disconnected` with `Sent`; (c) silence after the line: `Timeout` with
  `Sent`; (d) a refusal: no delivery note, `consoleStatusCode` unchanged.
- Same file: `setMemory` of 200 bytes where the third piece's line is answered by a hang-up: the
  message names the 128 bytes known written.
- `tests/protocols/xbdm/client_transfer_test.cpp`: the existing "may have been deleted" cases keep
  their text.
- `tests/net/transport_close_test.cpp` style test for the `writeSome` loop with `MemoryPipe`
  capacity 4: a cancel mid-line gives `Partial`.
- `tests/cli/xbdm_cli_test.cpp`: `xbdm rm` against a mock told to drop the connection on `delete`
  (`XbdmFault`), `--json` has `command_delivery: "unknown"`.

---

## Gap 4: host names fail because resolution is numeric-only

### Root cause

- `platform::resolve` sets `AI_NUMERICHOST` unconditionally (`src/net/platform/socket_platform.cpp:373`)
  and says "numeric IP address required" on failure (`socket_platform.cpp:393`).
- Every TCP connect resolves through it (`src/net/tcp_transport.cpp:124`), so `xbdm://devkit`,
  `tcp://console.lan:49` and `--target devkit` fail with `InvalidArgument`. The public comment says
  so (`include/net/tcp_transport.hpp:15-17`) and `docs/EXTENDING.md:736-740` lists it.
- `getaddrinfo` without `AI_NUMERICHOST` blocks for as long as DNS takes and is not bounded by
  `Endpoint::timeout`, nor cancellable (`docs/EXTENDING.md:780-782` already flags this).

### Fix

1. `platform::resolve` (`socket_platform.hpp:126-128`, private) gains a trailing parameter
   `bool allowNames = false`; every existing caller keeps the numeric behavior. With `allowNames`:
   try numeric first (no DNS for addresses); on `EAI_NONAME` retry without `AI_NUMERICHOST`, with
   `AI_ADDRCONFIG`. Family stays `kDefaultFamily` (`AF_INET`, `socket_platform.hpp:46`): XBDM is
   IPv4-only (spec 1.1) and IPv6 is a separate roadmap item.
2. Bound and cancel the name lookup: a private `resolveBounded(host, port, timeout, const WakeSignal
   *wake)` in `socket_platform.cpp` runs `getaddrinfo` on a short-lived thread that owns copies of
   its inputs and a `shared_ptr` to the result slot, and waits on a condition variable for the
   result, the timeout (`Endpoint::timeout`, the same bound as the connect) or the wake-up signal of
   C1. On timeout or cancel it returns `Timeout` / `Cancelled` and detaches the thread, which frees
   its `addrinfo` itself. Numeric hosts skip the thread.
3. `TcpTransport::connect` (`tcp_transport.cpp:115-161`) passes `allowNames = true`.
   `UdpSocket` (`src/net/udp_socket.cpp:41`, `47`, `85`) stays numeric:
   `include/net/datagram.hpp:26-28` and `:59` promise numeric addresses, and discovery's broadcast
   and probe addresses are addresses.
4. Error codes: `EAI_NONAME` stays `InvalidArgument` (as today for a bad literal, so the CLI's exit
   code 2 for a typo is unchanged); `EAI_AGAIN`/`EAI_FAIL` become `ConnectFailed`; the message says
   `cannot resolve '<host>'` without the "numeric" hint, and keeps it for UDP.
5. CLI help (`src/cli/app.cpp:20-21`, `:39`): "IP address or host name".

### Files

`src/net/platform/socket_platform.hpp`, `src/net/platform/socket_platform.cpp`,
`src/net/tcp_transport.cpp`, `src/cli/app.cpp`; tests below; docs in C10 (`README.md`,
`docs/EXTENDING.md` known limitations and the "IPv6 and host names" roadmap item).

### API compatibility

The platform header is private (`socket_platform.hpp:3`). `Endpoint` already carries a string host
(`docs/EXTENDING.md:790`). `TcpTransport::connect` keeps both signatures.

### Behavior change

Hosts that failed with `InvalidArgument` now connect when they resolve. A host that does not resolve
still fails with `InvalidArgument`. `describe()` keeps showing the numeric peer.

### Risks

- A detached resolver thread can outlive its caller by the DNS timeout; it touches only its own
  state. At process exit a still-running lookup is abandoned, which glibc and Winsock tolerate.
  Alternative to evaluate: `getaddrinfo_a` (glibc) and `GetAddrInfoExW` with a timeout (Windows).
- `AI_ADDRCONFIG` hides `localhost` on hosts with no non-loopback IPv4 address on some libcs; drop it
  if the loopback test fails there.
- Hostnames that resolve only to IPv6 fail with "no usable address" until IPv6 is enabled.

### Tests

- `tests/net/tcp_loopback_test.cpp`: `ConnectsByHostName` (`localhost` to the loopback server; SKIP
  when the resolver cannot resolve it), `UnknownHostNameIsInvalidArgument`
  (`no-such-host.invalid`, RFC 6761, must fail within the endpoint timeout), and the existing numeric
  tests unchanged.
- `tests/net/udp_socket_test.cpp`: a host name is still refused by `sendTo`.
- `tests/cli/xbdm_cli_test.cpp`: `--target xbdm://localhost:<mock port> xbdm info` succeeds.

---

## Gap 5: discovery always reports port 730 and cannot be cancelled

### Root cause

- Port: `XbdmDiscovery::run` builds every device with `kXbdmPort`
  (`src/protocols/xbdm/discovery.cpp:129`), ignoring `DiscoveryOptions::port`
  (`include/protocols/xbdm/discovery.hpp:25`), and `resolveNames` connects to `kXbdmPort`
  (`discovery.cpp:165`). With `port` set to anything else the device says 730 and the `dbgname`
  query goes to the wrong port (which is why `tests/protocols/xbdm/integration_discovery_test.cpp:127-129`
  needs a connector that ignores the endpoint's port). The CLI ignores the device's port as well:
  `Context::resolveXbdmEndpoint` uses `options.port.value_or(xbdm::kXbdmPort)`
  (`src/cli/context.cpp:196`), unlike the UpdServer path that reads `info["port"]`.
- Cancellation: `IDiscoveryProvider` (`include/discovery/discovery.hpp`) and `XbdmDiscovery` have no
  cancel. A search lasts its timeout plus up to `nameQueryBudget` (10 s,
  `discovery.hpp:38`) of `dbgname` queries, each a blocking `XbdmClient::connect`
  (`discovery.cpp:168-174`). `run` treats a failing `receive()` as "stop early" only when it has
  devices (`discovery.cpp:103-110`), and `resolveNames` moves on to the next console after any
  connector failure (`discovery.cpp:175-178`), a `Cancelled` one included. The CLI runs discovery
  without an `InterruptScope` (`src/cli/discover.cpp:34-35`, `src/cli/context.cpp:182-183`), so
  Ctrl-C kills the process and prints nothing.

### Can it be fixed without an API change?

The port, fully. A `cancel()` on `XbdmDiscovery` or `IDiscoveryProvider` would be new API. The
compatible route uses what the class already injects: the `DatagramSocketFactory` and the
`Connector` (`discovery.hpp:61-66`). A caller cancels by making its socket's `receive()` or its
connector return `ErrorCode::Cancelled`; the fix makes discovery honor that promptly. The CLI uses
exactly this.

### Fix

1. Port: `run` passes `options_.port` to `deviceFor` (`discovery.cpp:129`), and `resolveNames`
   connects to the device's `info["port"]` (parsed back, falling back to `options_.port`)
   (`discovery.cpp:165`). `options_.port` is the port the console answered on; XBDM uses the same
   number for UDP and TCP (spec 1.1, 2.1). Using `Datagram::senderPort` instead is open question 4.
2. Cancellation, without touching the private member declarations:
   - `XbdmDiscovery::Search` (defined at `discovery.cpp:42-50`) gains `mutable bool cancelled`.
   - In `run`, a `receive()` or `sendTo()` failure with `ErrorCode::Cancelled` sets it and stops:
     with devices, return them (as today's early stop); without, return the `Cancelled` error.
   - `discover`, `findByName` and `probeAddress` (`discovery.cpp:188-228`) skip `resolveNames` when
     `search.cancelled`.
   - `resolveNames` stops at the first connector or `debugName()` failure whose code is `Cancelled`;
     the remaining consoles keep their UDP names (as for an exhausted budget, `discovery.cpp:146-151`).
3. The default connector path (`XbdmClient::connect`, `discovery.cpp:174`) stays uncancellable
   (gap 7); callers who need cancellation inject a connector.
4. CLI (C9): a private `src/cli/cancellable.hpp`/`.cpp` with
   - a datagram socket decorator over `net::makeUdpSocket()` that checks an atomic flag before each
     `receive()` (discovery already polls in slices of at most 200 ms, `discovery.cpp:21`, `101-103`,
     so the latency is bounded without closing a socket across threads, which `UdpSocket` does not
     support);
   - a connector decorator that refuses once the flag is set and wraps each transport in a
     forwarding `ITransport` registered in a mutex-guarded set, so cancel can `close()` it
     (`ITransport::close` is thread-safe) and the greeting or `dbgname` wait ends at once.
   `runDiscover` (`src/cli/discover.cpp:18-65`) and `resolveXbdmEndpoint` (`src/cli/context.cpp:175-203`)
   build `XbdmDiscovery` with both decorators inside an `InterruptScope`. On Ctrl-C, `discover`
   prints what it found and exits 1 with code `Cancelled`; target resolution fails with `Cancelled`.
5. CLI: `resolveXbdmEndpoint` takes the port from `info["port"]` (as `resolveUpdServerEndpoint`
   does, `context.cpp:148-154`), `--port` still wins.

### Files

`src/protocols/xbdm/discovery.cpp`, `src/cli/discover.cpp`, `src/cli/context.cpp`,
`src/cli/cancellable.hpp` and `.cpp` (new); tests below; docs in C10.

### API compatibility

`Search` is defined in the `.cpp`; `run`'s and `resolveNames`' declarations do not change; nothing
public is added. The CLI files are not installed.

### Behavior change

`info["port"]` and the `dbgname` port follow `DiscoveryOptions::port` (identical with the default).
A `Cancelled` from an injected socket or connector now ends the search instead of being retried or
skipped.

### Risks

- A connector that fails with `Cancelled` for reasons of its own now stops the name queries early;
  acceptable, since `Cancelled` means "closed locally" (`include/core/error.hpp:22-24`).
- The 200 ms receive slice is the cancel latency in the UDP phase; the name phase is immediate.

### Tests

- `tests/protocols/xbdm/client_discovery_test.cpp`: `ReportsAndQueriesTheConfiguredPort`
  (`options.port = 7300`: `info["port"] == "7300"`, the connector sees endpoint port 7300);
  `ACancelledReceiveEndsTheSearch` (the fake socket returns one reply, then `Cancelled`: one device,
  no connector call); `ACancelledNameQueryStopsTheRest` (three replies, the first connect returns
  `Cancelled`: one connector call, all three devices with UDP names); `CancelledWithNothingFound`
  is the `Cancelled` error.
- `integration_discovery_test.cpp:115-137`: unchanged, plus a case where the mock's UDP and TCP
  listeners share one port number, if `XbdmMockServer` can be told to, with no custom connector.
- `tests/cli/xbdm_cli_test.cpp`: `discover --protocol xbdm` with a long `--discovery-timeout-ms`,
  SIGINT after 300 ms: exits within a second with code 1 (the existing Ctrl-C tests at
  `xbdm_cli_test.cpp:281-320` show the pattern).

---

## Gap 6: `rename` may fail on case-only renames

### Root cause

`XbdmClient::rename` (`client.cpp:1452-1472`) refuses when `attributes(target)` succeeds
(`client.cpp:1465-1467`). FATX and the console compare names case-insensitively (the client itself
does, `sameName`, `client.cpp:580-584`), so for `HDD:\a.txt` to `HDD:\A.txt` the "target" is the
source, the check finds it, and the call fails with `InvalidArgument "already exists"` before
anything is sent. Whether the console accepts a case-only `rename` at all is **[none]**: the mock
does (`tests/support/xbdm_mock_server.cpp:943-972`, `to.node != from.found.node`), a real console may
answer 410 like for an existing name.

### Fix

1. In `rename`, when `sameName(*source, *target)` (whole canonical paths, case-insensitive):
   - byte-for-byte equal: keep today's behavior (the existence check finds it, `InvalidArgument`);
   - differ only in case: skip the existence check and send `rename` directly.
2. If that direct `rename` is refused with 410 (or 400/414, open question 5), fall back to two steps
   on the same connection: rename to an intermediate name in the same folder (built like
   `temporaryName`, `client.cpp:607-612`, with a `.ren` suffix, at most `kMaxFileNameBytes`), then to
   the target. If the second step fails, rename back to the source; if that also fails, the error
   names where the file is now (`"; the file is now named <intermediate>"`, via `error_notes.hpp`).
   Delivery notes (gap 3) apply to each step.
3. Folders take the same path (`rename` works on both).

### Files

`src/protocols/xbdm/client.cpp`, `src/protocols/xbdm/error_notes.hpp`; tests below; docs in C10
(`docs/XBDM_PROTOCOL.md` 3.10 contract and a section 6 question).

### API compatibility

Body of `rename` only. The public comment (`client.hpp:395-397`) stays true: the new name does not
exist as a different entry.

### Behavior change

Case-only renames succeed instead of failing with `InvalidArgument`.

### Risks

- The two-step fallback is three commands and not atomic; a drop between them leaves the file under
  the intermediate name, which the error reports. Only used after a refusal.
- If a console's `rename` silently ignores a case-only change and answers 200, the call reports
  success without effect. Optionally verify with one `dirlist` of the parent and fall back when the
  listed name still has the old case (open question 5).

### Tests

- `tests/protocols/xbdm/client_commands_test.cpp`: `RenameThatChangesOnlyCase` (scripted console:
  no `getfileattributes`, one `rename`); `CaseOnlyRenameFallsBackToTwoSteps` (first `rename`
  answered 410: sequence intermediate, target); `ACaseOnlyRenameThatCannotFinishGoesBack` (second
  step refused: the third `rename` restores the source); `RenameOntoItselfIsStillRefused`.
- `tests/protocols/xbdm/integration_commands_test.cpp:269-280`: `rename("HDD:\\default.xex",
  "HDD:\\DEFAULT.XEX")` against the mock, then a listing shows the new case.

---

## Gap 7: connect cannot be cancelled

### Root cause

- `platform::connectWithTimeout` (`src/net/platform/socket_platform.cpp:557-599`) waits in
  `waitSocket(..., WaitFor::Writable, timeout)` without a wake-up signal (`socket_platform.cpp:577`),
  and `TcpTransport::connect` (`src/net/tcp_transport.cpp:115-161`) creates its `WakeSignal`
  (`tcp_transport.cpp:127-131`) but only uses it after the connect. No transport exists to `close()`
  while connecting (`include/net/tcp_transport.hpp:18-20`).
- `XbdmClient::connect`/`open` (`client.cpp:1064-1086`) connect (up to `Endpoint::timeout`, 5 s) and
  read the greeting (up to `greetingTimeout`, 5 s) before any `XbdmClient` exists, so nothing can be
  cancelled: an app that quits while a worker is in `connect()` waits up to about 10 s.
- `XbdmClient::reconnect` (`client.cpp:1127-1154`) promises that `cancel()` ends "a reconnect() that
  is still connecting" (`client.hpp:366-368`), but `Session::cancel` (`client.cpp:175-180`) can only
  bump `cancelEpoch`; the connector call (`client.cpp:1134`) runs to its own timeout, and only then
  does `install` (`client.cpp:184-203`, `1137`) notice and return `Cancelled`. The existing test
  (`tests/protocols/xbdm/client_cancel_test.cpp:343-376`) uses a connector that sleeps 300 ms, so it
  passes either way.
- The CLI connects before installing its `InterruptScope` (`src/cli/session.cpp:61`, `66`) and runs
  the cleanup `reconnect()` after it (`session.cpp:70-79`). Ctrl-C there kills the process outright
  (no wait, but also no clean "Cancelled" and no warning about a left-over upload).

### Can it be fixed without an API change?

- `reconnect()`: yes, fully, for clients made by `XbdmClient::connect`.
- The first `XbdmClient::connect()` / `open()`: **no**. The caller holds no object until the call
  returns, and every public carrier that could pass a cancellation handle in (`ClientOptions`,
  `Endpoint`, a new overload) would be a type or signature change. Closest compatible options:
  (a) bound the wait with the existing knobs (`Endpoint::timeout`, `ClientOptions::greetingTimeout`);
  (b) supply a cancellable transport through `XbdmClient::open(connector)` or `attach()`: the greeting
  phase is then cancellable by closing that transport from another thread, and the TCP phase is as
  cancellable as the caller's own transport. The API that would close it is an additive
  `XbdmClient::connect(endpoint, options, std::stop_token)` (open question 2).

### Fix

1. C1, internal TCP connect cancellation:
   - `platform::connectWithTimeout` gains `const WakeSignal *wake = nullptr` (private header,
     `socket_platform.hpp:143-145`); `Woken` returns `ErrorCode::Cancelled` ("connect cancelled").
   - New private header `src/net/connect_cancel.hpp`: `class ConnectCancel` (mutex, `cancelled`
     flag, a lazily created `WakeSignal`; `cancel()` is thread-safe and sticky) and
     `Result<TransportPtr> connectTcp(const Endpoint &, const std::shared_ptr<ConnectCancel> &)`.
   - `TcpTransport`'s constructor is private and `tcp_transport.hpp` may not gain a `friend`, so
     `connectTcp` sets a file-local `thread_local` pointer to the canceller (RAII guard) and calls
     the public static `TcpTransport::connect(endpoint)`, which reads it: wait on its wake in
     `connectWithTimeout` and in the bounded resolve of gap 4, and check the flag between addresses.
     Called without the guard, `TcpTransport::connect` behaves exactly as today.
2. C3, `reconnect()` cancellable while connecting:
   - `XbdmClient::connect` (`client.cpp:1064-1079`) creates a `std::shared_ptr<detail::ConnectSlot>`
     (holds the current `ConnectCancel` under a mutex) and captures it in the `tcp`/`xbdm` connector
     lambda (`client.cpp:1073`), which calls `connectTcp` with the slot's current canceller. It then
     calls an internal `openSession(connector, options, slot)` (a static function in `client.cpp`
     doing what `open` does) so the `Session` holds the same slot.
   - `reconnect` (`client.cpp:1132-1139`) arms a fresh `ConnectCancel` in the slot before calling the
     connector, unless `cancelEpoch` already moved (then `Cancelled` at once), and disarms it after.
   - `Session::cancel` (`client.cpp:175-180`) also calls `slot->cancel()` under `transportMutex`.
   - The greeting after the connect is already cancellable (`cancel()` closes the installed
     transport).
   - Connectors given to `open()` and registry schemes other than `xbdm`/bare host keep today's
     behavior: cancel takes effect when the connector returns.
3. CLI (C9): the `InterruptScope` in `withXbdm` also covers the cleanup `reconnect()`
   (`session.cpp:70-79`), so Ctrl-C there cancels it promptly and prints the warning for the
   left-over temporary file instead of killing the process. The first connect stays outside the scope
   (killing the process there is immediate and loses nothing); it moves inside only if open question
   2 adds the API.

### Files

`src/net/platform/socket_platform.hpp`, `src/net/platform/socket_platform.cpp`,
`src/net/connect_cancel.hpp` (new), `src/net/tcp_transport.cpp`, `src/protocols/xbdm/client.cpp`,
`src/cli/session.cpp`; tests below; docs in C10.

### API compatibility

No public header changes: `TcpTransport`'s declarations, `XbdmClient::connect`/`open`/`attach`/
`reconnect`/`cancel` keep their signatures. The `thread_local` handoff is invisible outside
`tcp_transport.cpp`. The behavior now matches what `client.hpp:366-368` already documents.

### Risks

- The `thread_local` handoff is unusual; keep it in one RAII guard with a comment, and reset it on
  every path (including exceptions from `std::function`).
- A `ConnectCancel` must not be reused across reconnects (its `WakeSignal` stays signalled once
  signalled); the slot always installs a fresh one.
- Winsock's `WakeSignal` is a UDP socket; `waitSocket` already handles it for reads and writes, the
  connect path reuses the same code.

### Tests

- `tests/protocols/xbdm/client_cancel_test.cpp`: `CancelInterruptsAReconnectStuckInTheTcpConnect`:
  a raw loopback listener (as in `tests/net/tcp_loopback_test.cpp:165-193`) accepts the first
  connection and greets, then its backlog is filled; `reconnect()` with `Endpoint::timeout` 5 s is
  cancelled after 100 ms and must return `Cancelled` in well under 1 s (SKIP when the kernel still
  accepts, as the existing test does). Linux/macOS only, like that test.
- Same file: `CancelInterruptsAReconnectWaitingForTheGreeting` (a listener that accepts and stays
  silent).
- `CancelDuringReconnectIsNotLost` (`client_cancel_test.cpp:343`) unchanged.
- `tests/net/tcp_loopback_test.cpp`: the existing connect tests unchanged (public path without a
  canceller).
- `tests/cli/xbdm_cli_test.cpp`: an upload interrupted with Ctrl-C while the mock delays new
  connections: the cleanup reconnect ends at the second Ctrl-C-free cancel and the warning names the
  temporary file (extends `CtrlCCancelsAnUploadAndDeletesTheTemporaryFile`, `xbdm_cli_test.cpp:303`).

---

## Suggested commits

Each commit builds and passes `ctest` on its own and carries its tests, as the history does
(`00811f1`, `4e3129f`, `1fbbf50`). Subjects follow the repository's style: a lower-case area prefix
for library layers (`transport:`, `tests:`, `docs:`, `build:`), `XBDM:` and `CLI:` in capitals, a
lower-case sentence without a final period, at most about 72 characters, and a wrapped prose body.

| # | Subject | Gaps |
|---|---|---|
| C1 | `transport: a TCP connect that a wake-up signal can interrupt` | 7 (groundwork) |
| C2 | `transport: host names for TCP endpoints, resolved within the timeout` | 4 |
| C3 | `XBDM: cancel() interrupts a reconnect that is still connecting` | 7 |
| C4 | `XBDM: discovery reports the port it searched and stops when cancelled` | 5 |
| C5 | `XBDM: rename that changes only the case of a name` | 6 |
| C6 | `XBDM: say whether a failed command reached the console` | 3 |
| C7 | `XBDM: an upload replaces only a file that existed when it began` | 1 |
| C8 | `XBDM: name a kept upload in a form the CLI can read back` | 2 |
| C9 | `CLI: kept uploads and command delivery in errors, Ctrl-C for discovery` | 2, 3, 5, 7 |
| C10 | `docs: host names, cancellation, upload replacement and rename case` | all |
| C11 | `docs: refresh the public header comments for the gap fixes` (optional) | all |

C10 updates: `README.md:234-236` (connect now cancellable inside `reconnect()`, host names),
`README.md:252-260` (kept-upload marker, delivery note, replace rule), `README.md:294-300` (Ctrl-C in
discovery and cleanup), `README.md:408-410` (discovered port); `docs/EXTENDING.md:736-740` and `:764`
(remove the numeric and connect-cancel limitations, keep IPv6), `:777-790` (roadmap item now only
IPv6); `docs/XBDM_PROTOCOL.md` 3.6 contract (replace rule), 3.10 contract (case-only rename), section
6 (rename onto an existing name, case-only rename).

C11, only with open question 1 answered yes: comment-only edits to
`include/protocols/xbdm/client.hpp:259-271` (replace rule, kept marker), `:366-368` (which
connectors `cancel()` interrupts), `:395-397` (case-only rename), `include/net/tcp_transport.hpp:15-20`
(host names) and `include/protocols/xbdm/discovery.hpp:49-58` (port, cancellation through the
injected socket and connector).

## Intended behavior changes, all together

1. `openWrite` sends one lookup before `sendfile`; a folder at the final name fails there.
2. `finish()` does not delete a file that did not exist at `openWrite`; `InvalidArgument` instead.
3. Error messages of mutating XBDM commands gain a delivery note; kept uploads keep their text.
4. TCP endpoints accept host names; DNS is bounded by `Endpoint::timeout`.
5. XBDM discovery reports and queries `DiscoveryOptions::port`; a `Cancelled` from the injected
   socket or connector ends the search.
6. Case-only `rename` succeeds.
7. `XbdmClient::cancel()` ends a `reconnect()` during its TCP connect (built-in connector).
8. CLI JSON errors may carry `kept_upload` and `command_delivery`; Ctrl-C works during discovery and
   the cleanup reconnect; an auto-discovered XBDM target uses the discovered port.

## Verification

```
cmake -S . -B build -DUPDCLIENT_WARNINGS=ON && cmake --build build -j
ctest --test-dir build --output-on-failure
build/tests/updclient_tests --filter Xbdm      # the XBDM suites
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON && cmake --build build-shared -j
ctest --test-dir build-shared                   # private helpers must not need exports
```

Also check that `include/` is unchanged (`git diff --stat ad83582 -- include/` empty, unless C11 is
taken) and that the MinGW cross build still compiles the platform changes.

## Open questions

1. May public header *comments* be edited (C11), or must `include/` stay byte-for-byte identical?
2. Would an additive API be acceptable later? It is the only full fix for gap 2 (a typed kept-upload
   flag), gap 3 (a typed delivery state), gap 5 (`XbdmDiscovery::cancel()`) and the first connect of
   gap 7 (`XbdmClient::connect(..., std::stop_token)`).
3. Gap 1: should a file that existed at `openWrite` but was replaced during the upload (different
   `createdFileTime`) still be replaced? Default: yes, existence only.
4. Gap 5: report `DiscoveryOptions::port` (default here) or the reply's `Datagram::senderPort`?
5. Gap 6: which refusals trigger the two-step fallback (410 only, or also 400/414), and should a
   case-only rename be verified with a listing?
6. `README.md:414` and `docs/EXTENDING.md:46`, `:747` link to `docs/ARCHITECTURE.md` and
   `docs/HARDWARE_TEST_PLAN.md`, which are not in the tree at `ad83582`. Restore them, or drop the
   links in C10?
