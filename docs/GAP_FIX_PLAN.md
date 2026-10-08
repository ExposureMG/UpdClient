# Plan: seven gaps in the XBDM client, the TCP transport and discovery

Status: implemented as C1 to C10 (see "Suggested commits"). Line numbers refer to commit `ad83582`.

## Ground rules

- **The XBDM protocol and the console side do not change.** Every fix works with the commands and
  answers of `docs/XBDM_PROTOCOL.md` as they are; the mock server changes only where a test needs a
  fault it cannot inject yet.
- **Public API: small, additive changes only, and only on the client side** (the library API that
  the CLI and GUI consume, and the CLI itself). Each addition is listed in "API additions" with its
  consumer impact. Rules for every addition:
  - New functions and overloads, never a changed signature: no default argument is added to an
    existing function (that would change its mangled name), so binaries built against the current
    headers keep linking.
  - New types are new enums and structs; no field is added to an existing struct or class
    (`Error`, `ClientOptions`, `DiscoveryOptions`, `Endpoint`, `XbdmClient`, `XbdmDiscovery` keep their
    layout). State lives behind the existing pimpls (`detail::Session`, `FileWriter::State`) or in
    `.cpp`-defined structs (`XbdmDiscovery::Search`).
  - No virtual function is added to an existing interface (`ITransport`, `IDiscoveryProvider`).
  - Cancellation uses C++20 `std::stop_token` (the library already requires C++20,
    `CMakeLists.txt`, `target_compile_features(updclient_lib PUBLIC cxx_std_20)`), so a GUI uses one
    `std::stop_source` per operation for every cancellable call.
- **Workarounds stay only where an API is not warranted:** messages keep their human-readable notes
  (kept upload, delivery), but nothing has to parse them any more.
- **Header comments in `include/` are updated** in the same commit as the behavior they describe.
- **Behavior-compatible:** every success path that works today keeps working; the intended
  differences are gathered in the last section. Error codes do not change.
- **Shared builds:** the library hides non-exported symbols (`CXX_VISIBILITY_PRESET hidden`); every
  new public function is `UPDCLIENT_API` (members of exported classes are exported with the class).
  The CLI uses only public API.

## API additions

All additive and source-compatible: existing calls compile and link unchanged. Taking the address of a
function that gained an overload (`&XbdmClient::connect`, `&XbdmClient::open`, the three
`XbdmDiscovery` searches, `&xbdm::identify`, `&DiscoveryRegistry::discoverAll`) needs a cast to the
wanted signature since C3 and C4.

| # | Header | Addition | Gap | Consumer impact (CLI / GUI) |
|---|---|---|---|---|
| A1 | `include/net/tcp_transport.hpp` | `static Result<TransportPtr> TcpTransport::connect(const Endpoint &, std::stop_token)` | 7, 4 | A stop request ends the connect (and a name lookup) at once with `ErrorCode::Cancelled`. Optional; the old overloads behave as before. |
| A2 | `include/protocols/xbdm/client.hpp` | `static Result<XbdmClient> XbdmClient::connect(const net::Endpoint &, ClientOptions, std::stop_token)` and `static Result<XbdmClient> XbdmClient::open(Connector, ClientOptions, std::stop_token)` | 7 | GUI: create a `std::stop_source` per connect; Cancel and quit call `request_stop()`, and the worker returns at once instead of after up to ~10 s. `open` honors the token for the greeting, and for the connect only when the connector returns early itself. CLI: connects inside its Ctrl-C scope. |
| A3 | `include/protocols/xbdm/client.hpp` | `std::vector<std::string> XbdmClient::keptUploads() const` and `void XbdmClient::clearKeptUploads()`; `std::optional<std::string> FileWriter::keptPath() const noexcept` | 2 | GUI: after a failed upload, `keptUploads()` (or the writer's `keptPath()`) names the file holding the only copy; offer "rename it" or "delete it", then `clearKeptUploads()`. CLI: JSON error field `kept_upload`. |
| A4 | `include/protocols/xbdm/client.hpp` | `enum class Delivery { NotSent, PartlySent, Sent, Answered }`, `struct CommandDelivery { std::string command; Delivery delivery; }`, `std::optional<CommandDelivery> XbdmClient::lastDelivery() const` | 3 | GUI: after a failed mutating call, `NotSent` means safe to retry; `PartlySent`/`Sent` means "may have happened, refresh before retrying". CLI: JSON error field `command_delivery`. |
| A5 | `include/protocols/xbdm/discovery.hpp` | `XbdmDiscovery::discover(std::chrono::milliseconds, bool, std::stop_token)`, `findByName(std::string_view, std::chrono::milliseconds, std::stop_token)`, `probeAddress(std::string_view, std::chrono::milliseconds, std::stop_token)`; `identify(const net::Endpoint &, ClientOptions, std::stop_token)` | 5 | GUI: a "Searching..." dialog's Cancel calls `request_stop()`; the call returns within one receive slice (200 ms) with the consoles found so far. CLI: Ctrl-C during `discover` prints what was found. |
| A6 | `include/discovery/discovery.hpp` | `DiscoveryRegistry::discoverAll(std::chrono::milliseconds, bool, std::stop_token) const` | 5 | Stops between providers, and passes the token to providers that are `XbdmDiscovery` (`dynamic_cast`); other providers run to their timeout as today. Optional for consumers. |

Not added, because the existing API already suffices or the change is not warranted:
- No `cancel()` member on `XbdmDiscovery` (it would add a data member and change the class layout;
  the token overloads do the same job).
- No new `Error` field or `ErrorCode` value (the state is on the client, A3 and A4, where it belongs).
- No virtual `discover(..., std::stop_token)` on `IDiscoveryProvider` (vtable change); A6 covers the
  registry.
- No `reconnect(std::stop_token)`: `XbdmClient::cancel()` already covers a reconnect and is made to
  work during the TCP connect (gap 7).

## Order and dependencies

```
C1 transport: TcpTransport::connect with a stop token (A1) ──┬─> C2 transport: host names (gap 4)
                                                             ├─> C3 XBDM: connect/open tokens, reconnect cancel (A2, gap 7)
                                                             └─> C4 XBDM: discovery port and tokens (A5, A6, gap 5)
C5 XBDM: case-only rename (gap 6)                            independent
C6 XBDM: lastDelivery() (A4, gap 3) ──> C7 XBDM: upload replace rule (gap 1) ──> C8 XBDM: kept uploads (A3, gap 2)
C3..C8 ──> C9 CLI: Ctrl-C through stop tokens, JSON fields, discovered port
C1..C9 ──> C10 docs
```

- C1 first: C2's bounded name lookup, C3's connect and C4's name queries all wait on its stop token.
- C4 after C3: discovery's `dbgname` queries connect through A2.
- C6 before C7 and C8: `finish()` decides "may have been deleted" from the delivery state, and C8's
  kept-upload bookkeeping sits on the error paths C7 rewrites.
- C5 can land at any point.

---

## Gap 1: `FileWriter::finish()` always replaces

### Root cause

- `XbdmClient::openWrite` (`src/protocols/xbdm/client.cpp:1514-1554`) sends `sendfile` to a
  temporary name without looking at the final name, so the writer has no idea whether the caller
  meant to replace something.
- `FileWriter::finish()` (`client.cpp:963-1032`) looks the final name up only after the data was
  confirmed (`lookUp`, `client.cpp:1008`) and deletes whatever it finds there (`client.cpp:1010-1024`)
  before the rename (`client.cpp:1026-1031`). A file that appears after the caller checked for one
  (for example, after a GUI's "Replace?" prompt said nothing was there), or during the upload, is
  deleted and replaced without a word. The public contract says so
  (`include/protocols/xbdm/client.hpp:259-262`, "deleting a file of that name first").
- The window between the `lookUp` and the `rename` inside `finish()` is a second, smaller race: the
  protocol has no "rename unless the target exists", and whether the console's `rename` refuses or
  replaces an existing target is **[none]** (`docs/XBDM_PROTOCOL.md` 3.10). The protocol cannot
  change, so that window can only be narrowed.

### Fix

1. `FileWriter::State` (`client.cpp:874-906`) gains `bool finalExistedAtOpen`.
2. `openWrite` calls `lookUp(session, *canonical, false)` after the argument checks and before the
   `sendfile` line (between `client.cpp:1532` and `1533`). A transport failure fails `openWrite` with
   nothing created; a refusal or "not found" means "did not exist" (`lookUp` already maps both,
   `client.cpp:680-701`); a folder of that name fails right away with the `InvalidArgument` that
   `finish()` gives today (`client.cpp:1011-1013`), so a doomed upload never sends its data.
3. `finish()` keeps its structure but decides with the snapshot:
   - lookup now finds nothing: rename directly (unchanged).
   - lookup finds a file and `finalExistedAtOpen`: delete, then rename (unchanged). Decided: this
     holds even if the file was replaced by another one during the upload; only existence is compared.
   - lookup finds a file and `!finalExistedAtOpen`: do not delete. Remove the temporary file on the
     same connection (`removeTemporary`, as for every failure where nothing was deleted) and fail with
     `InvalidArgument`, message `"sendfile <path>: <path> appeared during the upload and was not
     replaced; the upload was removed"`. Same code family as `rename()`'s "already exists"
     (`client.cpp:1466`).
   - the final rename is refused (410 or any other 4xx) while `!finalExistedAtOpen`: the target
     appeared inside the last window; same handling as the previous case.
4. Residual, documented: a file that appears between the `lookUp` in `finish()` and the `rename`, on a
   console whose `rename` silently replaces, cannot be detected. The window shrinks from "the whole
   upload" to one round trip. `docs/XBDM_PROTOCOL.md` section 6 gains the hardware question "does
   `rename` onto an existing name answer 410, or replace?".
5. Header comment `client.hpp:259-271`: "deleting a file of that name first" becomes "replacing a
   file of that name only if it existed when openWrite() ran; a file that appeared meanwhile fails
   finish() with InvalidArgument and is left alone".

### Files

`src/protocols/xbdm/client.cpp`, `include/protocols/xbdm/client.hpp` (comment only); tests in
`tests/protocols/xbdm/client_transfer_test.cpp` and `tests/protocols/xbdm/integration_transfer_test.cpp`.

### API compatibility

No API change: only `FileWriter::State` (defined in the `.cpp`) and function bodies.

### Behavior change

- One extra command (`getfileattributes`, or `dirlist` of the parent on a console that answers 407)
  before every `sendfile`.
- A file that did not exist at `openWrite` is never deleted by `finish()`; the call fails with
  `InvalidArgument` instead of replacing it. GUI impact: a "file appeared" error after a successful
  transfer; the GUI may ask again and re-upload.
- A folder at the final name fails in `openWrite` instead of after the data.

### Risks

- Scripted tests assert exact command sequences (`client_transfer_test.cpp:330-335`, `357-358`):
  they gain the lookup as their first command; `UploadReplacesAnExistingFile` expects
  `commands().size() == 5` and the delete at index 3.
- The `dirlist` fallback on consoles without `getfileattributes` makes `openWrite` cost a folder
  listing, bounded by `maxBodyBytes` like every listing.

### Tests

- `client_transfer_test.cpp`: new `AFileThatAppearsDuringTheUploadIsNotReplaced` (the `Files` fake
  creates the final name from inside the `sendfile` data handler; `finish()` is `InvalidArgument`, the
  other file is intact, the temporary file is deleted, the connection stays usable, `pendingCleanup()`
  and `keptUploads()` are empty).
- Same file: `AFileThatAppearsBeforeTheRenameIsNotReplaced` (the fake answers the lookup with 402,
  then creates the file and answers the rename with 410).
- Same file: `AFileReplacedDuringTheUploadIsStillReplaced` (exists at open, replaced by another
  during the upload: deleted and replaced, as decided).
- Same file: `UploadOntoAFolderFailsBeforeTheData` replaces the post-data expectations of
  `UploadOntoAFolderFailsAndRemovesTheTemporaryFile` (no `sendfile` is sent).
- Update the sequence assertions of the existing upload tests; `UploadReplacesAnExistingFile` and
  `AFailedRenameAfterTheOldFileWasDeletedKeepsTheUpload` must still pass otherwise.
- `UploadFindsTheFileToReplaceInTheListingWithoutGetfileattributes`: the listing is now read in
  `openWrite`, and again in `finish()`.
- `integration_transfer_test.cpp`: against the mock, a second client creates the final name while the
  first is uploading; the first upload fails and the second client's file survives.

---

## Gap 2: a kept upload is reported only in error text

### Root cause

`finish()`'s `giveUp` (`client.cpp:997-1006`) appends `"; the upload is kept as <temp>, since <path>
was deleted"` (or "may have been deleted") to `Error::message` and nothing else. `Error`
(`include/core/error.hpp:27-31`) carries the code and `sysError` of the delete or rename that failed,
so it looks like any other failure. After the failure `FileWriter::temporaryPath()` returns the name
whether the file was kept, deleted or queued for deletion, and `pendingCleanup()`
(`client.hpp:357`) does not list kept files by design, so nothing public says "your data is on the
console under this name". `uploadFromFile` (`client.cpp:1585-1613`) hides the writer altogether. The
CLI (`src/cli/xbdm.cpp:287-302`, `xbdmSend`) and its JSON error document (`src/cli/output.cpp`,
`Output::error`) show only the message.

### Fix (API additions A3)

1. `include/protocols/xbdm/client.hpp`:
   - `FileWriter::keptPath() const noexcept -> std::optional<std::string>`: the temporary path when
     `finish()` failed and kept it (the only copy of either version); `nullopt` otherwise, including
     before `finish()`.
   - `XbdmClient::keptUploads() const -> std::vector<std::string>`: every upload kept this way on
     this client, oldest first, including those of `uploadFromFile`; mirrors `pendingCleanup()`, but
     these files are never deleted by the client.
   - `XbdmClient::clearKeptUploads()`: forgets them, after the caller handled them.
2. `FileWriter::State` gains `std::optional<std::string> kept`; `detail::Session` gains
   `std::vector<std::string> keptUploads`, capped at 64 entries (oldest dropped with a debug log), so
   a long-lived GUI client cannot grow it without bound.
3. `giveUp` (`client.cpp:997-1006`) sets both when `finalRemoved` is non-empty. The message text is
   unchanged, so existing tests (`client_transfer_test.cpp:379`, `409`) and logs read the same.
4. Header comment `client.hpp:263-266` names `keptPath()` and `keptUploads()` instead of "names it in
   the error".
5. CLI (C9): `xbdmSend` reads `client.keptUploads()` after a failed upload; `Failure`
   (`src/cli/context.hpp:308-315`) gains `std::optional<std::string> keptUpload`; `Output::error`
   writes `"kept_upload": "<console path>"` into the JSON error object (additive key); text mode logs
   `The upload is kept on the console as <temp>; rename it with 'xbdm mv'`.

### Files

`include/protocols/xbdm/client.hpp`, `src/protocols/xbdm/client.cpp`, `src/cli/xbdm.cpp`,
`src/cli/context.hpp`, `src/cli/context.cpp`, `src/cli/output.hpp`, `src/cli/output.cpp`.

### API compatibility

Three new member functions of exported classes; no layout change (state is behind the pimpls).
Existing callers are unaffected. The CLI JSON gains an optional key.

### Risks

- A kept file listed in `keptUploads()` may later be renamed or deleted by the user through another
  client; the list is "what this client kept", not live state. Documented in the comment.

### Tests

- `client_transfer_test.cpp`: the existing kept-upload cases also check `keptPath() == temp` and
  `client.keptUploads() == {temp}`; every other failing case (refusal before the delete, appeared
  file, drop during data) checks that both are empty; `clearKeptUploads()` empties the list.
- Same file: `uploadFromFile` with a refused rename after the delete: `keptUploads()` names it.
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

### Fix (API addition A4)

1. `include/protocols/xbdm/client.hpp`:
   ```cpp
   // How far the last command line got. NotSent: no byte left the client, so the
   // console cannot have run it. PartlySent: part of the line was written (some
   // consoles end a line at CR, section 1.9), Sent: all of it; in both cases the
   // console may have carried it out. Answered: a status line was read.
   enum class Delivery { NotSent, PartlySent, Sent, Answered };
   struct CommandDelivery {
     std::string command; // the command name ("delete", "rename", ...), never its arguments
     Delivery delivery = Delivery::NotSent;
   };
   // The last command this client tried, including internal ones (the lookups and
   // deletes of an upload); nullopt before the first.
   std::optional<CommandDelivery> lastDelivery() const;
   ```
   Placed next to `lastStatus()` (`client.hpp:353-354`), which it complements.
2. `detail::Session` gains `std::optional<CommandDelivery> delivery`. `request()` and the other
   senders (`power`, `rawCommand`, `openWrite`, `screenshot`) set it to `NotSent` with the command
   name before `checkUsable`, to `PartlySent`/`Sent` as the line goes out, and to `Answered` in
   `readStatus`.
3. To know the partial count, `sendLine` writes the command line with its own `writeSome` loop (same
   `arm()` per iteration, same idle timeout and command deadline as `sendBytes`) instead of
   `writeAll`. Binary upload data keeps `sendBytes`.
4. Messages also gain a short note for humans on non-refusal failures of mutating commands:
   `"; the command was not sent"` or `"; the command was sent but not answered, so the console may
   have carried it out"`. Codes, `sysError` and `consoleStatusCode()` are unchanged (the note never
   contains `"console answered 4"`, `client.cpp:779-788`).
5. `setMemory` (`client.cpp:1793-1806`) adds to its message how many bytes are known written before
   the failing piece; `lastDelivery()` describes that piece.
6. `finish()` derives "may have been deleted" from `delivery` instead of `!isRefusal`
   (`client.cpp:1017-1020`), so a delete that was never sent no longer keeps the upload needlessly.
7. CLI (C9): `fromError` gets the client's `lastDelivery()` for XBDM failures (passed through
   `withXbdm`); `Failure` gains `std::optional<std::string> delivery`; the JSON error gets
   `"command_delivery": "not_sent" | "unknown"` (`PartlySent` and `Sent` map to `unknown`); text mode
   adds "The console may have carried the command out; check before repeating it" for `unknown`.

### Files

`include/protocols/xbdm/client.hpp`, `src/protocols/xbdm/client.cpp`, `src/cli/session.cpp`,
`src/cli/context.hpp`, `src/cli/context.cpp`, `src/cli/output.hpp`, `src/cli/output.cpp`.

### API compatibility

One enum, one struct, one const member function, all new. `Session` is private.

### Risks

- The `writeSome` loop must keep the idle-timeout and command-deadline semantics of `sendBytes`.
- `lastDelivery()` reports the last command, which for a composite call (`finish()`, the fallback
  rename of gap 6) is its last step; the comment says so.

### Tests

- `tests/protocols/xbdm/client_commands_test.cpp` with `FakeConsole`: (a) connection closed before
  the call: `removeFile` fails, `lastDelivery() == {"delete", NotSent}`; (b) `hangUp()` when the
  `delete` line arrives: `Disconnected`, `Sent`; (c) silence after the line: `Timeout`, `Sent`;
  (d) a refusal: `Answered`, `consoleStatusCode` unchanged, no note; (e) success: `Answered`.
- Same file: `setMemory` of 200 bytes, the third piece answered by a hang-up: the message names the
  128 bytes known written, `lastDelivery()` is `{"setmem", Sent}`.
- `client_transfer_test.cpp`: a delete in `finish()` that fails before its line is sent no longer
  keeps the upload; the existing "may have been deleted" cases are unchanged.
- A `MemoryPipe` (capacity 4) test with a cancel mid-line gives `PartlySent`.
- `tests/cli/xbdm_cli_test.cpp`: `xbdm rm` against a mock told to drop the connection on `delete`;
  `--json` has `command_delivery: "unknown"`.

---

## Gap 4: host names fail because resolution is numeric-only

### Root cause

- `platform::resolve` sets `AI_NUMERICHOST` unconditionally (`src/net/platform/socket_platform.cpp:373`)
  and says "numeric IP address required" on failure (`socket_platform.cpp:393`).
- Every TCP connect resolves through it (`src/net/tcp_transport.cpp:124`), so `xbdm://devkit`,
  `tcp://console.lan:49` and `--target devkit` fail with `InvalidArgument`. The public comment says
  so (`include/net/tcp_transport.hpp:15-17`) and `docs/EXTENDING.md:736-740` lists it.
- `getaddrinfo` without `AI_NUMERICHOST` blocks for as long as DNS takes and is neither bounded by
  `Endpoint::timeout` nor cancellable (`docs/EXTENDING.md:780-782` already flags this).

### Fix

1. `platform::resolve` (`socket_platform.hpp:126-128`, private) gains a trailing parameter
   `bool allowNames = false`; every existing caller keeps the numeric behavior. With `allowNames`:
   try numeric first (no DNS for addresses); on `EAI_NONAME` retry without `AI_NUMERICHOST`, with
   `AI_ADDRCONFIG`. Family stays `kDefaultFamily` (`AF_INET`, `socket_platform.hpp:46`): XBDM is
   IPv4-only (spec 1.1) and IPv6 is a separate roadmap item.
2. Bounded and cancellable lookup: a private `resolveBounded(host, port, timeout, const WakeSignal
   *wake)` runs `getaddrinfo` on a short-lived thread that owns copies of its inputs and a
   `shared_ptr` to the result slot, and waits for the result, the timeout (`Endpoint::timeout`, the
   same bound as the connect) or the wake-up signal that C1's stop token raises. On timeout or stop
   it returns `Timeout` / `Cancelled` and detaches the thread, which frees its `addrinfo` itself.
   Numeric hosts skip the thread.
3. `TcpTransport::connect` (all overloads, `tcp_transport.cpp:115-161`) passes `allowNames = true`.
   `UdpSocket` (`src/net/udp_socket.cpp:41`, `47`, `85`) stays numeric: `include/net/datagram.hpp:26-28`
   and `:59` promise numeric addresses, and discovery's broadcast and probe addresses are addresses.
4. Error codes: `EAI_NONAME` stays `InvalidArgument` (as today for a bad literal, so the CLI's exit
   code 2 for a typo is unchanged); `EAI_AGAIN`/`EAI_FAIL` become `ConnectFailed`; the message says
   `cannot resolve '<host>'` without the "numeric" hint, and keeps it for UDP.
5. Header comment `include/net/tcp_transport.hpp:15-17`: "IPv4 addresses and host names (resolved
   to IPv4 within Endpoint::timeout)". CLI help (`src/cli/app.cpp:20-21`, `:39`): "IP address or host
   name".

### Files

`src/net/platform/socket_platform.hpp`, `src/net/platform/socket_platform.cpp`,
`src/net/tcp_transport.cpp`, `include/net/tcp_transport.hpp` (comment), `src/cli/app.cpp`.

### API compatibility

No API change. The platform header is private (`socket_platform.hpp:3`); `Endpoint` already carries
a string host.

### Behavior change

Hosts that failed with `InvalidArgument` now connect when they resolve; one that does not resolve
still fails with `InvalidArgument`. `describe()` keeps showing the numeric peer.

### Risks

- A detached resolver thread can outlive its caller by the DNS timeout; it touches only its own
  state, and at process exit a still-running lookup is abandoned. Alternatives to evaluate:
  `getaddrinfo_a` (glibc) and `GetAddrInfoExW` with a timeout (Windows).
- `AI_ADDRCONFIG` hides `localhost` on hosts with no non-loopback IPv4 address on some libcs; drop it
  if the loopback test fails there.
- Host names that resolve only to IPv6 fail with "no usable address" until IPv6 is enabled.

### Tests

- `tests/net/tcp_loopback_test.cpp`: `ConnectsByHostName` (`localhost` to the loopback server; SKIP
  when it does not resolve), `UnknownHostNameIsInvalidArgument` (`no-such-host.invalid`, RFC 6761,
  fails within the endpoint timeout), numeric tests unchanged.
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
  needs a connector that ignores the endpoint's port). The CLI ignores the device's port too:
  `Context::resolveXbdmEndpoint` uses `options.port.value_or(xbdm::kXbdmPort)`
  (`src/cli/context.cpp:196`), unlike the UpdServer path that reads `info["port"]`
  (`context.cpp:148-154`).
- Cancellation: neither `IDiscoveryProvider` (`include/discovery/discovery.hpp`) nor `XbdmDiscovery`
  can be cancelled. A search lasts its timeout plus up to `nameQueryBudget` (10 s,
  `discovery.hpp:38`) of `dbgname` queries, each a blocking `XbdmClient::connect`
  (`discovery.cpp:168-174`); `resolveNames` moves on to the next console after any failure
  (`discovery.cpp:175-178`). The CLI runs discovery without an `InterruptScope`
  (`src/cli/discover.cpp:34-35`, `src/cli/context.cpp:182-183`), so Ctrl-C kills the process and
  prints nothing.

### Fix (API additions A5, A6)

1. Port (no API change): `run` passes `options_.port` to `deviceFor` (`discovery.cpp:129`), and
   `resolveNames` connects to `options_.port` (`discovery.cpp:165`). Decided: the configured port is
   reported, not the reply's `Datagram::senderPort`; XBDM uses one number for UDP and TCP (spec 1.1,
   2.1). Header comment `discovery.hpp:49-52`: "info ... "port" (DiscoveryOptions::port)".
2. Cancellation:
   - `XbdmDiscovery` gets the three `std::stop_token` overloads of A5; the existing overloads call
     them with an empty token. The private `run` and `resolveNames` keep their declarations; the token
     travels in `XbdmDiscovery::Search` (defined at `discovery.cpp:42-50`, gains `std::stop_token stop`
     and `bool stopped`), and `resolveNames` gets it through a private file-local helper that the
     public entry points call (the declared `resolveNames(std::vector&)` becomes a thin wrapper with
     an empty token).
   - `run` checks the token before every send and every `receive()` slice (at most 200 ms,
     `discovery.cpp:21`, `101-103`): on a stop it returns the devices found so far, or an empty list,
     and sets `stopped`; no name queries follow.
   - `resolveNames` checks the token before each console, connects with `XbdmClient::connect(...,
     stop)` (A2), and registers a `std::stop_callback` that calls `client.cancel()` while the greeting
     or `dbgname` is in progress. With an injected `Connector` the connect itself runs to its own
     timeout (the connector type stays as it is); the greeting and query are still cancelled.
     Consoles not asked keep their UDP names, as for an exhausted budget (`discovery.cpp:146-151`).
   - A stopped search is a success with the partial list, not an error: a GUI's Cancel shows what was
     found. `identify(..., stop)` returns `Cancelled`, since it has nothing partial to give.
   - `DiscoveryRegistry::discoverAll(timeout, stopAfterFirst, stop)` (A6) checks the token between
     providers and hands it to `XbdmDiscovery` providers (`dynamic_cast`); other providers keep
     running to their timeout.
3. CLI (C9): `runDiscover` (`src/cli/discover.cpp:18-65`) and `resolveXbdmEndpoint`
   (`src/cli/context.cpp:175-203`) run inside an `InterruptScope` whose callback calls
   `request_stop()` on a `std::stop_source`. On Ctrl-C `discover` prints what it found and exits 1
   with code `Cancelled`; target resolution fails with `Cancelled`. `resolveXbdmEndpoint` takes the
   port from `info["port"]` (parsed as on the UpdServer path); `--port` still wins.

### Files

`include/protocols/xbdm/discovery.hpp`, `src/protocols/xbdm/discovery.cpp`,
`include/discovery/discovery.hpp`, `src/discovery/discovery.cpp`, `src/cli/discover.cpp`,
`src/cli/context.cpp`.

### API compatibility

New overloads only; `XbdmDiscovery`'s data members and the declarations of its private members are
unchanged, and `IDiscoveryProvider`'s vtable is untouched.

### Behavior change

`info["port"]` and the `dbgname` port follow `DiscoveryOptions::port` (identical with the default).

### Risks

- The `dynamic_cast` in `discoverAll` couples the registry to one provider type; acceptable for one
  built-in provider, and a virtual on `IDiscoveryProvider` is the later, ABI-breaking alternative.
- 200 ms is the cancel latency in the UDP phase; the name phase is immediate.

### Tests

- `tests/protocols/xbdm/client_discovery_test.cpp`: `ReportsAndQueriesTheConfiguredPort`
  (`options.port = 7300`: `info["port"] == "7300"`, the connector sees endpoint port 7300);
  `AStopDuringTheUdpPhaseReturnsWhatWasFound` (fake socket gives one reply, the stop arrives during
  the next slice: one device, no connector call, returns within 300 ms of the stop);
  `AStopDuringNameQueriesKeepsUdpNames` (three replies, stop requested from the first connector call:
  one connector call, three devices with UDP names); `AStopBeforeTheSearchIsAnEmptyResult`;
  `IdentifyStoppedIsCancelled`.
- `tests/discovery/discovery_registry_test.cpp`: `discoverAll` with a stopped token skips the
  remaining providers.
- `integration_discovery_test.cpp:115-137`: unchanged, plus a stop from another thread during a
  real loopback search.
- `tests/cli/xbdm_cli_test.cpp`: `discover --protocol xbdm` with a long `--discovery-timeout-ms`,
  SIGINT after 300 ms: exits within a second with code 1 (pattern of `xbdm_cli_test.cpp:281-320`).

---

## Gap 6: `rename` may fail on case-only renames

### Root cause

`XbdmClient::rename` (`client.cpp:1452-1472`) refuses when `attributes(target)` succeeds
(`client.cpp:1465-1467`). FATX and the console compare names case-insensitively (the client itself
does, `sameName`, `client.cpp:580-584`), so for `HDD:\a.txt` to `HDD:\A.txt` the "target" is the
source, the check finds it, and the call fails with `InvalidArgument "already exists"` before
anything is sent. Whether the console accepts a case-only `rename` is **[none]**: the mock does
(`tests/support/xbdm_mock_server.cpp:943-972`, `to.node != from.found.node`); a real console may
answer 410 as for an existing name.

### Fix

1. In `rename`, when `sameName(*source, *target)` (whole canonical paths, case-insensitive):
   - byte-for-byte equal: keep today's behavior (`InvalidArgument`);
   - differ only in case: skip the existence check and send `rename` directly.
2. If that direct `rename` is refused with 410 or 400 (the answers a console gives for an existing
   name, spec 3.8 and 3.10), fall back to two steps on the same connection: rename to an intermediate
   name in the same folder (built like `temporaryName`, `client.cpp:607-612`, with a `.ren` suffix, at
   most `kMaxFileNameBytes`), then to the target. Other refusals (414 and the rest) are returned as
   they are. If the second step fails, rename back to the source; if that also fails, the error
   says `"; the file is now named <intermediate>"`.
3. No verification listing after a 200 (one more command per rename is not warranted);
   `docs/XBDM_PROTOCOL.md` section 6 gains "does a case-only `rename` change the stored case?".
4. Folders take the same path. `lastDelivery()` (gap 3) describes the last step.
5. Header comment `client.hpp:395-397`: "A new name that differs only in case is allowed."

### Files

`src/protocols/xbdm/client.cpp`, `include/protocols/xbdm/client.hpp` (comment).

### API compatibility

No API change: body of `rename` only.

### Behavior change

Case-only renames succeed instead of failing with `InvalidArgument`.

### Risks

- The two-step fallback is three commands and not atomic; a drop between them leaves the file under
  the intermediate name, which the error reports. Only used after a refusal.
- A console that answers 200 but keeps the old case is not detected (hardware question above).

### Tests

- `tests/protocols/xbdm/client_commands_test.cpp`: `RenameThatChangesOnlyCase` (scripted console:
  no `getfileattributes`, one `rename`); `CaseOnlyRenameFallsBackToTwoSteps` (first `rename`
  answered 410: intermediate, then target); same with 400; `ACaseOnlyRenameRefusedWith414IsReturned`
  (no fallback); `ACaseOnlyRenameThatCannotFinishGoesBack` (second step refused: the third `rename`
  restores the source); `RenameOntoItselfIsStillRefused`.
- `tests/protocols/xbdm/integration_commands_test.cpp:269-280`: `rename("HDD:\\default.xex",
  "HDD:\\DEFAULT.XEX")` against the mock, then a listing shows the new case.

---

## Gap 7: connect cannot be cancelled

### Root cause

- `platform::connectWithTimeout` (`src/net/platform/socket_platform.cpp:557-599`) waits in
  `waitSocket(..., WaitFor::Writable, timeout)` without a wake-up signal (`socket_platform.cpp:577`),
  and `TcpTransport::connect` (`src/net/tcp_transport.cpp:115-161`) creates its `WakeSignal`
  (`tcp_transport.cpp:127-131`) but uses it only after the connect. No transport exists to `close()`
  while connecting (`include/net/tcp_transport.hpp:18-20`).
- `XbdmClient::connect`/`open` (`client.cpp:1064-1086`) connect (up to `Endpoint::timeout`, 5 s) and
  read the greeting (up to `greetingTimeout`, 5 s) before any `XbdmClient` exists, so nothing can be
  cancelled: a GUI that quits or cancels while a worker is in `connect()` waits up to about 10 s.
- `XbdmClient::reconnect` (`client.cpp:1127-1154`) promises that `cancel()` ends "a reconnect() that
  is still connecting" (`client.hpp:366-368`), but `Session::cancel` (`client.cpp:175-180`) can only
  bump `cancelEpoch`; the connector call (`client.cpp:1134`) runs to its own timeout, and only then
  does `install` (`client.cpp:184-203`, `1137`) notice. The existing test
  (`tests/protocols/xbdm/client_cancel_test.cpp:343-376`) uses a connector that sleeps 300 ms, so it
  passes either way.
- The CLI connects before installing its `InterruptScope` (`src/cli/session.cpp:61`, `66`) and runs
  the cleanup `reconnect()` after it (`session.cpp:70-79`); Ctrl-C there kills the process outright,
  without a clean "Cancelled" or the warning about a left-over upload.

### Fix (API additions A1, A2)

1. C1, `TcpTransport::connect(const Endpoint &, std::stop_token)` (A1):
   - `platform::connectWithTimeout` gains `const WakeSignal *wake = nullptr` (private header,
     `socket_platform.hpp:143-145`); `Woken` returns `ErrorCode::Cancelled` ("connect cancelled").
   - The new overload creates the `WakeSignal` first (it already exists per transport,
     `tcp_transport.cpp:127-131`), registers a `std::stop_callback` that signals it, and passes it to
     the name lookup of gap 4 and to `connectWithTimeout` for every address; it checks the token
     between addresses. The existing overloads call it with an empty token, so they behave as today.
   - Header comment `tcp_transport.hpp:18-20`: "A connect can be cancelled through the stop_token
     overload."
2. C3, `XbdmClient::connect(endpoint, options, stop)` and `open(connector, options, stop)` (A2):
   - `connect` builds its `tcp`/`xbdm` connector (`client.cpp:1073`) around A1 with the token; for
     registry schemes the token is checked before and after the registry connector.
   - Both create the `Session` before the greeting and register a `std::stop_callback` that calls
     `session->cancel()` until the greeting is read, so a stop during the greeting returns
     `Cancelled` at once. `open`'s connector runs to its own end (its type is unchanged); the token is
     checked right after it returns.
   - The existing overloads forward with an empty token.
3. C3, `reconnect()` interrupted by `cancel()` during its TCP connect (no API change; makes
   `client.hpp:366-368` true):
   - The `Session` keeps a `std::stop_source` for the connect in progress. The connector that
     `connect` builds takes its token from a small shared slot (`std::shared_ptr<detail::ConnectSlot>`,
     a mutex and the current `std::stop_token`), which `reconnect` (`client.cpp:1132-1139`) arms with
     a fresh source before calling the connector, unless `cancelEpoch` already moved (then
     `Cancelled` at once).
   - `Session::cancel` (`client.cpp:175-180`) also calls `request_stop()` on that source under
     `transportMutex`.
   - Connectors given to `open()` keep today's behavior: `cancel()` takes effect when the connector
     returns. Documented in the comment of `cancel()`.
4. CLI (C9): `withXbdm` (`src/cli/session.cpp:32-80`) installs one `InterruptScope` before the
   connect whose callback calls `request_stop()` on a `std::stop_source` and, once a client exists,
   `client->cancel()`; the connect uses A2 with that token, and the scope also covers the cleanup
   `reconnect()`. Ctrl-C while connecting exits 1 with `Cancelled`; during the cleanup it prints the
   warning for the left-over temporary file.

### Files

`include/net/tcp_transport.hpp`, `src/net/tcp_transport.cpp`, `src/net/platform/socket_platform.hpp`,
`src/net/platform/socket_platform.cpp`, `include/protocols/xbdm/client.hpp`,
`src/protocols/xbdm/client.cpp`, `src/cli/session.cpp`.

### API compatibility

Three new static overloads; existing signatures untouched. `TcpTransport` and `XbdmClient` keep their
layout (state in `Impl` and `Session`).

### GUI impact

A GUI keeps one `std::stop_source` per connect (and per discovery). Cancel and quit call
`request_stop()`; for a connected client, `cancel()` as before. Quitting no longer waits for the
connect and greeting timeouts. A GUI that does not adopt the overloads behaves exactly as today.

### Risks

- A `stop_callback` runs on the thread that calls `request_stop()`; it must only signal (never
  block or take the session's I/O path). `Session::cancel` already satisfies that.
- Winsock's `WakeSignal` is a UDP socket; `waitSocket` already handles it for reads and writes, and
  the connect path reuses the same code.
- The CLI's `InterruptScope` now spans the connect; its "one scope at a time" rule
  (`src/cli/interrupt.hpp:10-12`) holds because discovery's scope (gap 5) ends before `withXbdm`
  opens its own.

### Tests

- `tests/net/tcp_loopback_test.cpp`: `AStopEndsAConnectStuckOnAFullBacklog` (listener with a full
  backlog as in `tcp_loopback_test.cpp:165-193`, `Endpoint::timeout` 5 s, stop after 100 ms: returns
  `Cancelled` well under 1 s; SKIP when the kernel still accepts); `AStoppedTokenFailsBeforeConnecting`;
  existing connect tests unchanged.
- `tests/protocols/xbdm/client_cancel_test.cpp`: `AStopEndsTheGreetingWait` (listener accepts and
  stays silent; `XbdmClient::connect(..., stop)` returns `Cancelled` promptly);
  `CancelInterruptsAReconnectStuckInTheTcpConnect` (first connection greeted, then backlog filled;
  `cancel()` during `reconnect()` returns `Cancelled` well under 1 s); `CancelDuringReconnectIsNotLost`
  (`client_cancel_test.cpp:343`) unchanged.
- `tests/cli/xbdm_cli_test.cpp`: Ctrl-C while connecting to a silent listener exits 1 with `Cancelled`
  within a second; Ctrl-C during the cleanup reconnect (extends
  `CtrlCCancelsAnUploadAndDeletesTheTemporaryFile`, `xbdm_cli_test.cpp:303`) prints the warning.

---

## Suggested commits

Each commit builds and passes `ctest` on its own and carries its tests and the header comments it
makes true, as the history does (`00811f1`, `4e3129f`, `1fbbf50`). Subjects follow the repository's
style: a lower-case area prefix for library layers (`transport:`, `tests:`, `docs:`, `build:`),
`XBDM:` and `CLI:` in capitals, a lower-case sentence without a final period, at most about 72
characters, and a wrapped prose body.

| # | Subject | Gaps | API |
|---|---|---|---|
| C1 | `transport: a TCP connect that a stop token cancels` | 7 | A1 |
| C2 | `transport: host names for TCP endpoints, resolved within the timeout` | 4 | none |
| C3 | `XBDM: stop tokens for connect and open, and cancel() during reconnect` | 7 | A2 |
| C4 | `XBDM: discovery reports the port it searched and takes a stop token` | 5 | A5, A6 |
| C5 | `XBDM: rename that changes only the case of a name` | 6 | none |
| C6 | `XBDM: lastDelivery() says whether a failed command reached the console` | 3 | A4 |
| C7 | `XBDM: an upload replaces only a file that existed when it began` | 1 | none |
| C8 | `XBDM: keptUploads() and FileWriter::keptPath() name a kept upload` | 2 | A3 |
| C9 | `CLI: Ctrl-C while connecting and discovering, kept uploads in errors` | 2, 3, 5, 7 | none |
| C10 | `docs: host names, stop tokens, upload replacement and rename case` | all | none |

C10 updates `README.md:234-236` (connect cancellation, host names), `README.md:252-260` (kept
uploads, `lastDelivery()`, the replace rule, stop tokens), `README.md:294-300` (Ctrl-C while
connecting, discovering and cleaning up), `README.md:408-410` (discovered port);
`docs/EXTENDING.md:736-740` and `:764` (remove the numeric and connect-cancel limitations, keep
IPv6), `:777-790` (the roadmap item becomes IPv6 only); `docs/XBDM_PROTOCOL.md` 3.6 contract (replace
rule), 3.10 contract (case-only rename), section 6 (rename onto an existing name, case of a case-only
rename). It leaves the links to `ARCHITECTURE.md` and `HARDWARE_TEST_PLAN.md` as they are.

## Intended behavior changes, all together

1. `openWrite` sends one lookup before `sendfile`; a folder at the final name fails there.
2. `finish()` does not delete a file that did not exist at `openWrite`; `InvalidArgument` instead.
3. Error messages of mutating XBDM commands gain a delivery note; kept-upload messages are unchanged.
4. TCP endpoints accept host names; the lookup is bounded by `Endpoint::timeout`.
5. XBDM discovery reports and queries `DiscoveryOptions::port`.
6. Case-only `rename` succeeds.
7. `XbdmClient::cancel()` ends a `reconnect()` during its TCP connect (built-in connector).
8. CLI: Ctrl-C works while connecting, discovering and cleaning up; JSON errors may carry
   `kept_upload` and `command_delivery`; an auto-discovered XBDM target uses the discovered port.
9. A delete in `finish()` that never left the client no longer keeps the upload: the temporary file is
   removed or queued for `reconnect()` instead (C6).
10. A final rename that the console refuses, when nothing was deleted and nothing existed at
    `openWrite()`, is followed by one more lookup of the final name; only a file found there turns the
    refusal into "appeared" (C7, instead of "any 4xx means it appeared").

Everything else is opt-in through the API additions A1 to A6.

## Verification

```
cmake -S . -B build -DUPDCLIENT_WARNINGS=ON && cmake --build build -j
ctest --test-dir build --output-on-failure
build/tests/updclient_tests --filter Xbdm      # the XBDM suites
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON && cmake --build build-shared -j
ctest --test-dir build-shared                   # new API exported, nothing private needed
```

Also: `git diff ad83582 -- include/` shows only the additions A1 to A6 and comment edits (no changed
or removed declaration, no new data member); an application built against the `ad83582` headers
links against the new shared library (build `tests/` of `ad83582` against it as a smoke test); the
MinGW cross build compiles the platform changes.

## Open questions

None outstanding. Decided: header comments may change; additive client-side API (A1 to A6); a file
replaced during an upload is still replaced; discovery reports the configured port; case-only renames
fall back on 410 and 400 without a verification listing; the `ARCHITECTURE.md` and
`HARDWARE_TEST_PLAN.md` links are left alone.
