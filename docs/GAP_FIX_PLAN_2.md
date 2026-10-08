# Plan: the second round of gaps (review of C1 to C10)

Status: implemented as D1 to D9 (see "Suggested commits"), with the open question decided: `rename()`
onto an existing name is `AlreadyExists` too. Line numbers refer to commit `183c89b`.

## Ground rules

The rules of [GAP_FIX_PLAN.md](GAP_FIX_PLAN.md) still hold: the XBDM protocol and the console side
do not change, public API changes are additive and client-side only (new functions, new enumerators
appended at the end, no changed signature, no new data member, no new virtual), header comments
change with the behavior they describe, and every commit builds and passes `ctest` on its own. One
correction to that plan's wording is part of this one (gap 12): "existing callers compile
unchanged" holds for calls, not for taking the address of a function that gained an overload.

"Minimal API changes" here means: fix behavior and comments first; add API only where the app has
to parse text today.

## API additions

| # | Header | Addition | Gap | Consumer impact |
|---|---|---|---|---|
| B1 | `include/net/tcp_transport.hpp` | `net::Endpoint TcpTransport::peer() const` | 6 | The connected address as data: scheme `tcp`, numeric host, port. Replaces parsing `describe()`. |
| B2 | `include/protocols/xbdm/client.hpp` | `std::optional<net::Endpoint> XbdmClient::peer() const` | 6 | The same for a client: nullopt when not connected or not over `TcpTransport` (an injected transport). GUI: show and remember the numeric address. |
| B3 | `include/core/error.hpp` | `ErrorCode::AlreadyExists`, appended after `Cancelled`; `errorCodeName` gives `"AlreadyExists"` | 8 | `finish()` fails with it when a file appeared at the final name during the upload, and `rename()` when the new name exists. A folder at an upload's final name stays `InvalidArgument`. CLI: exit code 1, JSON `code: "AlreadyExists"`. An exhaustive `switch` on `ErrorCode` gains a case. |
| B4 | `include/protocols/xbdm/client.hpp` | `bool FileWriter::replacesExisting() const noexcept` | 9 | Whether a file of the final name existed when `openWrite()` ran, so `finish()` will replace it. GUI: confirm "Replace?" against what the writer saw, not against an earlier listing. |

Changed meaning, no new API:
- `lastDelivery()` (gap 1): `NotSent` is reported only when nothing the call sent can have changed
  the console.
- `ConnectFailed` (gap 7): every failure to make the TCP connection, a connect timeout included, is
  `ConnectFailed`; `Timeout` is left for a connection that was made and then went quiet.

Not added:
- No new `Delivery` value: raising `NotSent` to `Sent` for a call whose earlier step took effect
  keeps the documented reading ("may have happened") true without a new state to handle.
- No `ErrorCode::Unreachable`: `ConnectFailed` already means it, once timeouts are folded in (gap 7).
- No renamed stop-token functions: they shipped in C3 and C4 and the app already uses them. The
  address-of exception is documented instead (gap 12).

## Order and dependencies

```
D1 transport: the lookup thread keeps the socket runtime (gap 2)           independent
D2 transport: one deadline per connect, ConnectFailed for timeouts (5, 7)  ──> D3
D3 transport, XBDM: peer() (B1, B2, gap 6)
D4 XBDM: lastDelivery() describes the whole call (gap 1)                   ──> D5
D5 XBDM: case-only rename says where the file is; drives without case (4, 14)
D6 XBDM: upload lines checked in openWrite, AlreadyExists, replacesExisting (3, 8, 9; B3, B4)
D7 CLI: command_delivery only for commands that change the console (gap 10)   after D4
D8 tests: registry race, mock name responder on any port (11, 15)          independent
D9 docs: the address-of exception, the behavior list, the new API (12, 13)  last
```

- D4 before D5: the rename messages use the call-level delivery to know whether a step went out.
- D4 before D7: the CLI reports what `lastDelivery()` says.
- D6 is independent of D4 but touches `finish()`; landing it after D4 avoids a conflict in
  `giveUp`.

---

## Gap 1: `lastDelivery()` can say NotSent after the call changed something (important)

### Root cause

`Session::delivery` (`src/protocols/xbdm/client.cpp`, set by `beginCommand`, `client.cpp:340`)
describes the last command only. In a call made of several commands an earlier step can already
have taken effect:
- case-only rename (`renameCase`, `client.cpp:1487`): first `rename` refused with 410, the move to
  the intermediate name answered 200 (`client.cpp:1508`), then the connection fails before a byte of
  the last step leaves (`client.cpp:1511`). `lastDelivery()` is `{"rename", NotSent}`, and
  `withDeliveryNote` (`client.cpp:693`) adds "the command was not sent". The file is under the
  intermediate name; repeating `rename(a, A)` fails because `a` no longer exists.
- `finish()`: the old file deleted (200), the rename never sent: `{"rename", NotSent}`. The kept
  upload is reported, but the delivery contradicts it.
- `setMemory`: pieces 1 and 2 written, piece 3 never sent: `{"setmem", NotSent}`. Repeating is
  harmless here, but the rule should not depend on that.

The header (`include/protocols/xbdm/client.hpp:387`) promises "NotSent means it is safe to repeat".

### Fix

1. The session tracks the call, not only the command. `detail::Session` gains `bool callChanged`.
   - Reset at the start of every public call: in `sessionOf()` (`client.cpp:1376`, which every
     `XbdmClient` method calls first), and at the start of `FileWriter::finish()`.
   - Set when a command that can change the console reaches `PartlySent` or `Sent` without an
     answer, or is answered with a success status. A refusal does not set it.
   - Which commands can change the console: every command except a fixed read-only list next to
     `beginCommand` (`dbgname` without arguments, `consoletype`, `getconsoleid`, `xbeinfo`,
     `getexecstate`, `altaddr`, `drivelist`, `drivefreespace`, `dirlist`, `getfileattributes`,
     `getfile`, `getmem`, `getmemex`, `walkmem`, `modules`, `modsections`, `screenshot`). An unknown
     name (a raw command) counts as changing.
2. `lastDelivery()` returns the last command's delivery, except that `NotSent` becomes `Sent` when
   `callChanged` is set. `command` still names the last command.
3. `withDeliveryNote` uses the same rule. When the last command was not sent but an earlier step
   took effect, the note is "; an earlier step was carried out" instead of "; the command was not
   sent". Rename and upload messages say what is left (gaps 2 and 4 of the first plan, gap 4 here).
4. Internal decisions keep the per-command state: `finish()`'s "may have been deleted"
   (`client.cpp:1137`) reads the delete's own delivery, not the call's.
5. Header comment at `client.hpp:381-391`: "For a call made of several commands, `NotSent` means no
   command of the call that can change the console left the client, so the call is safe to repeat;
   once an earlier step took effect, the delivery is at least `Sent` even if the last command was not
   sent. `command` names the last command."

### API compatibility

No API change; the meaning of `NotSent` becomes stricter, as the comment already promised.

### Tests

- `client_commands_test.cpp`: `ACaseOnlyRenameThatStopsAfterTheIntermediateIsNotNotSent`: 410, 200,
  then `dropAfterWritten(written())` so the last step is not sent; `lastDelivery()` is
  `{"rename", Sent}`, the message has no "not sent" and names the intermediate (gap 4).
- Same file: `setMemory` of 200 bytes with the third piece never sent: `{"setmem", Sent}`; with the
  first piece never sent: `{"setmem", NotSent}`.
- Same file: `rename` whose `getfileattributes` check fails stays `NotSent` (the check is
  read-only); the existing `ARenameWhoseCheckFailsWasNotSent` keeps passing.
- `client_transfer_test.cpp`: delete answered, rename not sent: `lastDelivery()` is Sent, and
  `keptPath()` names the upload.
- Every existing delivery test keeps passing (single-command calls are unchanged).

---

## Gap 2: Windows: the lookup thread can outlive the socket runtime

### Root cause

`platform::resolveBounded` (`src/net/platform/socket_platform.cpp:442-480`) detaches a thread
(`socket_platform.cpp:456`) that holds the `LookupSlot` and its `WakeSignal` (a UDP socket pair on
Winsock) but no `RuntimeGuard`. When the connect gives up (timeout or stop) and the caller drops the
last guard, `WSACleanup` can run while the thread is still in `getaddrinfo`. Its later
`slot->done.signal()` then writes through a socket handle that Winsock has invalidated and may have
handed out again after a new `WSAStartup`.

### Fix

1. `resolveBounded` acquires a `RuntimeGuard` (`socket_platform.hpp:58`) and moves it into the
   thread's lambda, next to the slot. The runtime then stays up until the thread has signalled and
   released the slot; `WSACleanup` runs, at the latest, when the last lookup ends.
2. The thread drops the slot (closing the `WakeSignal`) before the guard, by declaration order in
   the lambda's captures, with a comment saying why.
3. POSIX is unchanged in effect (`RuntimeGuard` is a no-op there).

### Files

`src/net/platform/socket_platform.cpp` only.

### Tests

- No portable test can reach `WSACleanup` timing. Add `tests/net/tcp_loopback_test.cpp`
  `ALookupThatOutlivesItsConnectIsHarmless`: a stop during a lookup of `no-such-host.invalid`,
  then 50 further connects and stops in a loop. It passes under ASan on Linux and checks that
  nothing crashes or leaks.
- The MinGW cross build compiles the change; the Windows run is a hardware-plan item.

---

## Gap 3: `finish()` deletes the old file before it builds the rename line

### Root cause

`FileWriter::finish()` deletes the final name (`client.cpp:1137`) and only then builds the rename
line (`client.cpp:1149`). The rename line carries both the temporary and the final path, so it is
the longest line of an upload. When it exceeds `maxCommandBytes`, the call fails after the delete.
The upload is kept and reported, so nothing is lost, but the old file is gone for nothing.

### Fix

1. `openWrite` (`client.cpp:1782`) builds every line `finish()` can send before it sends anything:
   `delete` of the final name, `rename` of the temporary name to the final name, and `delete` of the
   temporary name. A line over the limit fails `openWrite` with `LimitExceeded`, before the lookup
   and before `sendfile`.
2. `FileWriter::State` keeps the three lines; `finish()` and `removeTemporary` use them instead of
   building their own, so they cannot fail on length any more.
3. Header comment on `openWrite`: "fails with LimitExceeded when a command the upload needs would
   exceed maxCommandBytes; nothing is sent then".

### API compatibility

None; `FileWriter::State` is defined in the `.cpp`.

### Tests

- `client_transfer_test.cpp`: `AnUploadWhoseRenameLineIsTooLongFailsBeforeAnything`: a
  `maxCommandBytes` between the `sendfile` line and the rename line; `openWrite` is
  `LimitExceeded`, `commands()` is empty, `pendingCleanup()` and `keptUploads()` are empty.

---

## Gap 4: case-only rename errors do not always say where the file is

### Root cause

`renameCase` (`client.cpp:1487-1530`) adds "the file is now named" or "may now be named" only when
the last step fails (`client.cpp:1518`). The move to the intermediate name (`client.cpp:1508`)
returns its error as it is: after a lost answer the file may already be under the intermediate
name, and nothing says so.

### Fix

Once the fallback has begun, every failure ends with one sentence about the file's location,
decided by how far each step got (per-command delivery):

| Failure | Message ends with |
|---|---|
| move to intermediate refused, or not sent | `; the file is still named <source>` |
| move to intermediate not answered | `; the file is named <source> or <intermediate>` |
| last step not sent (connection gone before it) | `; the file is now named <intermediate>` |
| last step refused, rename back answered 200 | `; the file is still named <source>` |
| last step refused, rename back refused | `; the file is now named <intermediate>` |
| last step refused, rename back not answered | `; the file is named <intermediate> or <source>` |
| last step not answered | `; the file is named <intermediate> or <target>` |

The first `rename` (before the fallback) keeps today's messages. The table goes into the comment
of `renameCase`; the header comment (`client.hpp`, `rename`) says "the error says where the file
is whenever the fallback did not finish".

### Tests

`client_commands_test.cpp`: one case per row, each checking the sentence and `lastDelivery()`.
The existing `ACaseOnlyRenameThatCannotFinishGoesBack` covers two rows already and is adjusted.

---

## Gap 5: `Endpoint::timeout` is spent once for the lookup and again per address

### Root cause

`TcpTransport::connect` (`src/net/tcp_transport.cpp`) passes the full `timeout` to
`resolveBounded` (`tcp_transport.cpp:144`) and again to `connectWithTimeout` for each address
(`tcp_transport.cpp:159`). `localhost` listed twice in `/etc/hosts` resolves to two identical
addresses, so a silent target costs lookup + 2 × timeout.

### Fix

1. One deadline for the whole connect: `start + Endpoint::timeout` (none when the timeout is 0).
   The lookup gets the full remaining time; each address gets what is left; an address reached
   after the deadline is not tried and the error is the timeout (gap 7's code).
2. Identical addresses (same family, address and port) are tried once; `resolve` drops repeats,
   keeping the resolver's order.
3. Header comment (`include/net/tcp_transport.hpp`): "Endpoint::timeout bounds the whole connect,
   the name lookup and every address together."

### Behavior change

A host with several distinct addresses no longer gets a full timeout per address; the later ones
share what is left. With IPv4 only, that is rare.

### Tests

`tcp_loopback_test.cpp`: `TheTimeoutBoundsTheWholeConnect` (`StalledListener`, timeout 300 ms,
host `localhost`: returns within 600 ms whatever `/etc/hosts` holds; SKIP when `localhost` does not
resolve); a unit test of the de-duplication through two numeric connects is not possible from
outside, so the `resolve` change is covered by that test only.

---

## Gap 6: no supported way to get the numeric peer address

### Fix (B1, B2)

1. `TcpTransport::Impl` stores the numeric host and the port separately (today one string,
   `tcp_transport.cpp:19`, `:170`). `peer()` returns an `Endpoint{"tcp", host, port}` with the
   default timeout; `describe()` is unchanged.
2. `XbdmClient::peer()` takes the transport under `transportMutex`, `dynamic_cast`s it to
   `TcpTransport` and returns its `peer()`; nullopt when not connected or not TCP.
3. Header comments: `peer()` is the address actually connected (after name lookup), not the
   endpoint asked for.

### Tests

- `tcp_loopback_test.cpp`: `PeerIsNumeric`: connect to `localhost` (SKIP when it does not resolve),
  `peer().host == "127.0.0.1"`, `peer().port` is the server's.
- `client_cancel_test.cpp` (loopback) or `integration_commands_test.cpp` (TCP link): `peer()` of a
  connected client; nullopt for a `MemoryPipe` client and after `close()`.

---

## Gap 7: no distinct error code for "could not reach the console"

### Root cause

A TCP connect that times out is `ErrorCode::Timeout`
(`socket_platform.cpp:637`, `:652`, `:663`), and so is a name lookup that times out
(`socket_platform.cpp:475`). A greeting that never comes is `Timeout` too. The app cannot tell
"no connection could be made" from "connected, then silence" without reading messages.

### Fix

1. Every failure to make the TCP connection is `ConnectFailed`: refused, unreachable, no usable
   address, resolver failure, and a timeout of the connect or of the name lookup. For a timeout
   `sysError` is `ETIMEDOUT` (`WSAETIMEDOUT` on Windows), so a caller that cares can still see it.
2. Unchanged: an unknown host name stays `InvalidArgument` (a bad argument, CLI exit 2, as decided
   in C2); a stop request stays `Cancelled`; everything after the connection was made keeps its code
   (greeting timeout `Timeout`, 401 `LimitExceeded`, and so on).
3. Header comment of `XbdmClient::connect` lists what the codes mean before a client exists:
   `ConnectFailed` = no connection, `InvalidArgument` = bad endpoint or unknown name, `Timeout`,
   `Protocol`, `LimitExceeded` = connected but no usable greeting, `Cancelled` = stopped.
4. The same rule in `include/net/tcp_transport.hpp`.

### Behavior change

A connect timeout changes from `Timeout` to `ConnectFailed` for every TCP user (UpdServer, XeLL,
XBDM). The CLI exit code stays 1; the JSON `code` changes.

### Tests

- `tcp_loopback_test.cpp`: `ConnectTimeoutAgainstAFullBacklog` (`tcp_loopback_test.cpp:193`)
  expects `ConnectFailed` only, with `sysError == ETIMEDOUT`.
- `client_cancel_test.cpp:493` (`CancelInterruptsAReconnectStuckInTheTcpConnect`) still SKIPs on a
  greeting `Timeout`, which is unchanged.
- `integration_faults_test.cpp`: a silent greeting is still `Timeout`, and a connect to an unused
  port is `ConnectFailed`.

---

## Gaps 8 and 9: "file appeared" and "folder exists" share InvalidArgument; the writer hides what it saw

### Root cause

`finish()` fails with `InvalidArgument` both for a file that appeared during the upload (`appeared`,
`client.cpp:1121`) and for a folder at the final name (`client.cpp:1132`), as does `openWrite` for a
folder (`client.cpp:1805`). The app tells them apart by message. Whether a file existed at
`openWrite` lives only in `FileWriter::State::finalExistedAtOpen`.

### Fix (B3, B4)

1. `ErrorCode::AlreadyExists` is appended to `ErrorCode` (`include/core/error.hpp:24`, after
   `Cancelled`, so no value moves) and named in `errorCodeName` (`src/core/error.cpp`).
2. `finish()` uses it for a file that appeared, both after its own lookup and after a refused rename
   that the second lookup explains. A folder at the final name stays `InvalidArgument`, in
   `openWrite` and in `finish()` alike: the path names a folder, which is an argument problem.
3. `FileWriter::replacesExisting()` returns `finalExistedAtOpen`; false for a moved-from writer.
4. The messages stay as they are.
5. CLI: `exitCodeFor` maps `AlreadyExists` to 1 (the default branch already does); nothing else.

### Decided: `rename()` too

`rename()`'s own "already exists" check (before the `rename` is sent) fails with `AlreadyExists` as
well, so the CLI's `xbdm mv` onto an existing name exits 1 instead of 2. A rename onto the very same
name, which that check used to catch, fails with `InvalidArgument` before anything is sent.

### Tests

- `client_transfer_test.cpp`: `AFileThatAppearsDuringTheUploadIsNotReplaced` and
  `AFileThatAppearsBeforeTheRenameIsNotReplaced` expect `AlreadyExists`;
  `AFolderThatAppearsDuringTheUpload...` and `UploadOntoAFolderFailsBeforeTheData` keep
  `InvalidArgument`.
- Same file: `replacesExisting()` is true in `UploadReplacesAnExistingFile`, false in the
  temporary-name test.
- `integration_transfer_test.cpp`: the two-client race expects `AlreadyExists`.
- `tests/core/error_test.cpp`: `errorCodeName(ErrorCode::AlreadyExists)`.

---

## Gap 10: the CLI reports `command_delivery` for read-only commands

### Root cause

`withXbdm` (`src/cli/session.cpp:87-89`) attaches the delivery to every failure, so `xbdm ls` that
times out says "the console may have carried the command out".

### Fix

`withXbdm` gains a parameter (CLI-internal, `src/cli/session.hpp`):
`enum class XbdmEffect { ReadOnly, ChangesConsole }`. Only `ChangesConsole` sessions attach
`command_delivery` and log the hint. Marked `ChangesConsole`: `rm`, `mv`, `mkdir`, `file send`
(`xbdmSend`), `mem poke` (`xbdmPoke`), `eject`, `raw`, `launch`, `reboot`, `shutdown`. Everything
else is `ReadOnly`. `kept_upload` is unaffected (only uploads produce it).

### Tests

`tests/cli/xbdm_cli_test.cpp`: `xbdm ls` against a mock that drops on `dirlist`: `--json` has no
`command_delivery`; the existing `xbdm rm` case keeps it.

---

## Gap 11: a data race in `discovery_registry_test`

`ConcurrentAddAndEnumerate` (`tests/discovery/discovery_registry_test.cpp:152`) calls
`discoverAll` from four threads on shared providers, whose `StubProvider::calls` and
`lastStopAfterFirst` (`discovery_registry_test.cpp:39-40`) are plain fields. They become
`std::atomic<int>` and `std::atomic<bool>`. The registry itself is not affected. Check: the test
under `-fsanitize=thread` (one-off local build; not added to CI here).

---

## Gap 12: the new overloads break taking the address of the old functions

`&XbdmClient::connect`, `&XbdmClient::open`, `&XbdmDiscovery::discover`, `findByName`,
`probeAddress`, `&xbdm::identify` and `&DiscoveryRegistry::discoverAll` are ambiguous since C3 and C4
(`TcpTransport::connect` already had two overloads before). Calls compile unchanged; only code that
takes an address needs a cast, for example
`static_cast<Result<XbdmClient> (*)(const net::Endpoint &, ClientOptions)>(&XbdmClient::connect)`.

Fix: documentation only. `GAP_FIX_PLAN.md` ("API additions" and the ground rules) and the README's
API notes say "existing calls compile unchanged; taking the address of an overloaded function needs
a cast". Renaming the new functions would break the app that already uses them.

---

## Gap 13: the first plan's behavior-change list omits two changes

`GAP_FIX_PLAN.md`, "Intended behavior changes, all together", gains:
9. A delete in `finish()` that never left the client no longer keeps the upload: the temporary file
   is removed or queued for `reconnect()` instead (C6).
10. A final rename that the console refuses, when nothing was deleted and nothing existed at
    `openWrite()`, is followed by one more lookup of the final name (C7, the deviation from the
    plan's "any 4xx means it appeared").

This plan's own list (below) is checked against its diff before D9.

---

## Gap 14: `rename()` compares drive names with case

`XbdmClient::rename` (`client.cpp:1724`) compares `driveOf(source)` and `driveOf(target)` with `!=`,
so `hdd:\a` to `HDD:\b` is refused as "different drives" although drive names, like file names, are
case-insensitive. It uses `sameName` instead. Test in `client_commands_test.cpp`: that rename sends
one `getfileattributes` and one `rename`.

---

## Gap 15: the mock's in-memory name responder only answers port 730

`MockDatagramSocket` answers only datagrams sent to port 730 (`tests/support/xbdm_mock_server.cpp:33`,
`:1557`), so an in-memory discovery with `DiscoveryOptions::port` set finds nothing; only the real UDP
listener (`listenUdp`) answers on any port. `XbdmMockServer::datagramFactory`
(`tests/support/xbdm_mock_server.hpp:376`) gains `uint16_t port = 730`, and the socket answers that
port (and sets it as `senderPort`). Test in `integration_discovery_test.cpp`: in memory with port 7300,
the device reports `"7300"`; a mock on 7300 ignores a search on 730.

---

## Suggested commits

| # | Subject | Gaps | API |
|---|---|---|---|
| D1 | `transport: the name lookup thread keeps the socket runtime alive` | 2 | none |
| D2 | `transport: one deadline per connect, and ConnectFailed when it runs out` | 5, 7 | none |
| D3 | `transport: peer() gives the connected address, also on XbdmClient` | 6 | B1, B2 |
| D4 | `XBDM: lastDelivery() is NotSent only when the call changed nothing` | 1 | none |
| D5 | `XBDM: case-only rename errors say where the file is` | 4, 14 | none |
| D6 | `XBDM: upload lines checked up front, AlreadyExists, replacesExisting()` | 3, 8, 9 | B3, B4 |
| D7 | `CLI: command_delivery only for commands that change the console` | 10 | none |
| D8 | `tests: atomic stub counters, mock name responder on any port` | 11, 15 | none |
| D9 | `docs: peer(), AlreadyExists, call-level delivery, overload addresses` | 12, 13, all | none |

D9 updates the README API notes (peer, AlreadyExists, `replacesExisting()`, the `lastDelivery()`
rule, the `ConnectFailed` rule, the address-of exception), `docs/EXTENDING.md` (the connect
deadline), `GAP_FIX_PLAN.md` (gaps 12 and 13), and marks this plan implemented.

## Intended behavior changes, all together

1. `lastDelivery()` is `Sent`, not `NotSent`, after a call whose earlier step took effect; delivery
   notes in messages follow.
2. Case-only rename errors after the fallback began always end with where the file is.
3. `openWrite` fails with `LimitExceeded` before sending anything when a line of the upload would be
   too long.
4. `Endpoint::timeout` bounds the whole TCP connect; repeated addresses are tried once.
5. A TCP connect or name lookup that times out is `ConnectFailed` (with `ETIMEDOUT`), not `Timeout`.
6. `finish()` fails with `AlreadyExists` instead of `InvalidArgument` when a file appeared.
7. `rename()` accepts drive names that differ only in case; onto an existing name it fails with
   `AlreadyExists` instead of `InvalidArgument`; onto its own name it fails with `InvalidArgument`
   without sending the existence check.
8. CLI: `command_delivery` and its hint only for commands that change the console; JSON `code`
   may be `AlreadyExists` or, for connect timeouts, `ConnectFailed`.

## Verification

As in the first plan: static and shared builds with `ctest`, the MinGW cross build, and the
exported-symbol comparison against `183c89b` (nothing missing; added only B1 to B4's functions).
Additionally one local `-fsanitize=thread` run of the discovery and cancel suites, and one
`-fsanitize=address` run of the TCP suite for gap 2.

## Open questions

None. Decided: `rename()` onto an existing name is `AlreadyExists` (gap 8).
