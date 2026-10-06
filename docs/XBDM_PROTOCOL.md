# XBDM Protocol Contract

> Written 2026-10-06 from the third-party clients cloned in `references/` of the
> UnnamedGpl3QtApp repository (read-only, git-ignored there). Nothing here has been
> checked against a console or an emulator yet: every fact is "what the references
> do", and the open questions at the end are to be settled on hardware.

XBDM is the Xbox 360 debug monitor: a line-based text protocol over TCP port 730,
with a few binary transfers, plus a small UDP name protocol on the same port. Every
console and emulator that offers it speaks the same protocol. This document is the
contract for UpdClient's XBDM protocol client (`protocols/xbdm`) and for the mock
server its tests run against. Two people should be able to build either side from
it alone. Where the references disagree, or a fact rests on one reference only, it
says so; where the client has to pick a behaviour anyway, the choice is marked
**Contract:**.

## 0. How to read this

### 0.1 Sources

Citations are `TAG:line` (or `TAG commit`), relative to `references/`:

| Tag | File | Licence | What it is |
|---|---|---|---|
| XC | `XBDM/src/Console.cpp` | MIT | C++ client by ClementDreptin, "for development versions of the Xbox 360" (`XBDM/README.md:4`) |
| XT | `XBDM/test/TestServer.cpp` | MIT | That client's own mock server. Several known deviations from real consoles (section 7) |
| ONO | `OpenNeighborhood-old/src/...` | Apache-2.0 | ImGui app built on XC |
| ON | `OpenNeighborhood/src/lib/...` (`xbdm/xbdm.ts`, `xbdm/constants.ts`, `xbdm/utils.ts`, `consoles.ts`, `utils.ts`) | Apache-2.0 | Same author's later TypeScript client, fixed against a devkit (commit 7a33519) |
| NSc, NSd, NSx, NSg, NSu, NSt | `NeighborSharp/NeighborSharp/XBDMConnection.cs`, `DataTypes.cs`, `Xbox360.cs`, `XboxOG.cs`, `IConsoleDiscovery.cs`, `NeighborTool/Program.cs` | LGPLv3 | C# client by InvoxiPlayGames |
| ED | `EmDbg/EmDbg/XboxDebugger.cs` | LGPLv3 | Debugger on NeighborSharp; distinguishes devkit XBDM from the RGH "Natelx" XBDM |
| DT | `DevTool/src/DevTool/...` | MIT | Devkit tool on Microsoft's XDevkit COM library (no raw protocol, but some facts) |
| MEc, MEf, MEr, MEp, MEe, MEj, MEi, MEt, MEd | `MemoryEngine360/Xbox/MemEngine360.Xbox360XBDM/` `Consoles/Xbdm/XbdmConsoleConnection.cs`, `Consoles/Xbdm/XbdmConsoleConnection.XbdmFeaturesImpl.cs`, `Consoles/Xbdm/XbdmResponseType.cs`, `ParamUtils.cs`, `XbdmEventUtils.cs`, `Consoles/Xbdm/Jrpc2FeaturesImpl.cs`, `Views/ConnectToXboxInfo.cs`, `Consoles/ConnectionTypeXbox360Xbdm.cs`, `Views/DiscoveredConsole.cs` | GPLv3 | Memory scanner/editor; the most complete raw client |
| ME | `MemoryEngine360/MemEngine360/...` | GPLv3 | Its engine (scans, dumps, chunking) |
| XL, XLs, XLr | `xefu-live/xefulive/xbdm.py`, `xefu-live/sdk/csharp/OgXbox/XbdmSocket.cs`, `xefu-live/README.md` | none stated | Live memory editor, tested on a JTAG console with an `xbdm.xex` plugin |

### 0.2 Markers

- **[1]** seen in one reference only.
- **[≠]** the references disagree; the alternatives are listed.
- **[inf]** inferred from what the code does, not stated by it.
- **[none]** not in any reference; only hardware can tell (section 6).
- **Contract:** what the app's client and mock do, where the references leave a choice.

### 0.3 Targets

Every target (devkits, retail consoles with an XBDM plugin, emulators) speaks the same
XBDM protocol, so this document makes no per-target distinctions. Where a reference
was written for one kind of console, the source key says so; that is evidence about
the reference, not a difference in the protocol.

## 1. Connection

### 1.1 Transport and addresses

- TCP, port **730** (XC:42, NSx:21, ON xbdm.ts:73, MEt:110, XLs:23). Port 731 is the
  original Xbox's XBDM (NSg:14) and is out of scope.
- A devkit has two IP addresses, a debug one and a title one; XBDM answers on the
  debug address: "if running a developer kernel, use the debug IP!" (NSt:21) **[1]**.
  `altaddr` returns the title address (section 3.1).
- IPv4 only in every reference (ON `utils.ts:40-42` validates IPv4; ME discovery skips
  IPv6, MEi:96-100).

### 1.2 Greeting

On accept the console sends exactly one line:

```
201- connected\r\n
```

XC compares the whole line byte for byte (XC:77-79); ME lowercases it first
(MEt:114-115, MEi:166-167); NSc skips to the first `\n` without checking (NSc:24).
ON writes its first command *before* reading the greeting (ON xbdm.ts:15-18), so a
console evidently buffers a command sent early **[inf]**.

**Contract:** the client waits for the greeting before its first command, accepts
`201-` followed by any text (compared case-insensitively to `connected` only for a
log warning), and fails the connection on any other status. The mock sends the
greeting immediately after accept and must not lose bytes the client sends before
it.

### 1.3 Command lines (client to console)

```
command[ argument]*\r\n
argument = key=value | key="quoted value" | flag
```

- ASCII, terminated by CR LF (XC:676, NSc:64, MEc:690-694, XLs:66).
- Values: unquoted decimal or `0x` hex for 32 bits, `0q` hex for 64 bits (NSd:85-110,
  MEp:52-60), quoted strings for names and paths (`name="HDD:\x"`), bare flags such as
  `dir`, `running`, `cold`, `clear`, `override`.
- An empty line counts as a command: "Sending duplicate CRLF results in effectively
  two commands being sent" (MEc:690) **[1]**. Never send one.
- Key matching is case-insensitive in ME's parser (MEp:156-157). Command names are
  sent in mixed case: `BYE` (NSc:35) and `bye` (ON xbdm.ts:39, MEc:193); `magicboot COLD`
  (XC:366, NSx:103, ON consoles.ts:465) and `magicboot cold` (ED:238, MEc:374). So
  command names and flags are probably case-insensitive **[inf]**.
- Argument order and a trailing space appear not to matter: NeighborSharp emits flags,
  then strings, then numbers, each followed by a space (NSd:132-151), so EmDbg sends
  `setmem data="0102" addr=0x82000000 ` (ED:190-200) and `break clear addr=0x...`
  (ED:324-332), where ME sends `break addr=0x... clear` (MEf:320) **[inf]**.
- Lines have a maximum length on the console, value unknown **[none]**: XL writes
  memory in 0x80-byte pieces "because xbdm caps the command line length" (XL:95,
  XLs:110), ED splits `setcontext` "due to the size limit" (ED:378), and the error
  table has `406` and `446` for over-long lines (MEr:53-54, MEr:168).

**Contract:** commands are ASCII; the client never pipelines (1.10), never sends an
empty line, puts the command name and flags in lower case, and keeps `setmem` data at
64 bytes or less per line (the smaller of ME's 64, MEc:666, and XL's 128, XL:96). The
mock accepts any argument order, tolerates trailing spaces, and refuses lines over a
configurable limit (default 512 bytes, a mock setting, not a console fact) with
`406- line too long`.

### 1.4 Response status line (console to client)

```
DDD- text\r\n
```

Three decimal digits, `-`, a space, then text. Parsers take the code from the first
three characters and the text from index 5 (NSd:12-16, MEr/`XbdmResponse.cs:45`,
ON xbdm.ts:91-102); XL takes `text[4:].strip()` (XL:57-62). ME requires more than 4
characters (`XbdmResponse.cs:45`).

The text is either a fixed phrase or the result itself:

| Code | Meaning | Text | Sources |
|---|---|---|---|
| 200 | OK, single-line result | `OK`, or the result (`dbgname` gives the name; `bye` gives `bye`) | ON constants.ts:4, ON xbdm.ts:82-88, XC:122-125 |
| 201 | Connected (greeting only) | `connected` | ON constants.ts:5 |
| 202 | Multi-line response follows (1.5) | `multiline response follows` | ON constants.ts:6, XT:98 |
| 203 | Binary response follows (1.6) | `binary response follows` | ON constants.ts:7, XC:404 |
| 204 | Ready for binary data from the client (1.7) | `send binary data` | ON constants.ts:8, XC:517 |
| 205 | Connection dedicated, here to notifications (1.8) | not quoted by any reference | MEr:30, ED:403, MEc:1180 |

Errors are 4xx. The table is MemoryEngine360's, which names Microsoft's `XBDM_*`
constants (MEr:33-176) **[1]**; the codes the other references meet are marked.

| Code | XBDM name | Meaning | Also seen |
|---|---|---|---|
| 400 | UNDEFINED | Unspecified error | XT uses it for everything (not authoritative) |
| 401 | MAXCONNECT | Too many connections | ON consoles.ts:319 ("401- max number of connections exceeded") |
| 402 | NOSUCHFILE | No such file | `modsections` unknown module (MEf:430) |
| 403 | NOMODULE | No such module | |
| 404 | MEMUNMAPPED | Memory not mapped (`setmem`) | MEc:670 tolerates it |
| 405 | NOTHREAD | No such thread | MEc:312, MEf:344 |
| 406 | CLOCKNOTSET | Clock not set; ME's note: "linetoolong or clocknotset" | |
| 407 | INVALIDCMD | Unknown command | |
| 408 | NOTSTOPPED | Not stopped (`go` when running) | MEc:397, XL:204 |
| 409 | MUSTCOPY | File must be copied, not moved | |
| 410 | ALREADYEXISTS | Already exists (`mkdir`) | XC:627, ON constants.ts:9 |
| 411 | DIRNOTEMPTY | Directory not empty | |
| 412 | BADFILENAME | Invalid file name | |
| 413 | CANNOTCREATE | Cannot create file (`sendfile`) | MEf:232 |
| 414 | CANNOTACCESS | Access denied; also a directory given to a file command | MEf:149, MEf:174, MEf:234 |
| 415 | DEVICEFULL | Device full | |
| 416 | NOTDEBUGGABLE | Title not debuggable | |
| 417, 418 | BADCOUNTTYPE, COUNTUNAVAILABLE | Performance counters | |
| 420 | NOTLOCKED | Console not locked | |
| 421 | KEYXCHG | Key exchange required | |
| 422 | MUSTBEDEDICATED | Needs a dedicated connection | |
| 423 | INVALIDARG | Invalid argument | MEj (`SendCommand`, returns null on 423) |
| 424, 425 | PROFILENOTSTARTED, PROFILEALREADYSTARTED | Profiling | |
| 426 | ALREADYSTOPPED | Already stopped (`stop`) | MEc:388, XL:204 |
| 427-445 | FASTCAPNOTENABLED ... PMCSESSIONNOTACTIVE | Profiling, NOMEMORY (428), TIMEOUT (429), NOSUCHPATH (430), screen formats (431, 432), FIELDNOTPRESENT (437, `xexfield`) | MEr:111-165 |
| 446 | LINE_TOO_LONG | Command line too long | MEr:168 |
| 480, 481 | D3D_DEBUG_COMMAND_NOT_IMPLEMENTED, D3D_INVALID_SURFACE | Direct3D | |
| 496, 497 | (Vx) task pending, too many sessions | Voice | |

- The error text is free-form and may carry the reason: ME detects a protected
  directory by the substring `access denied` in the `dirlist` error line (MEf:110) **[1]**.
- No reference has seen a 5xx from a console; XT sends `500-` for its own failure
  (XT:532).
- DevTool's XDevkit calls return HRESULT `0x02DA0000` for success (DT
  `Classes/XDKUtilities.cs:117`), so XBDM's facility is 0x2DA **[1]**. The mapping of
  4xx codes to HRESULTs is not in the references.

**Contract:** the client parses `DDD-` and an optional space, keeps the code and the
text, and classifies: 2xx success, 4xx error (connection still usable, as XL treats it,
XL:147-148), anything else (5xx, 1xx, 3xx, non-digits) a protocol error that drops the
connection. An unexpected 2xx for a command (for example 203 where 200 was expected)
also drops the connection, because its payload framing cannot be known (1.6).

### 1.5 Multi-line responses (202)

After the `202-` line come zero or more lines, ended by a line that is exactly `.`
(NSc:86-91, MEc:272-281, ON xbdm.ts:25-36). An empty listing is
`202- multiline response follows\r\n.\r\n`; ON's "opening empty folders was throwing"
fix handles exactly that (ON commit 5128cb0).

- Body lines are usually `key=value` lists (dirlist, drivelist, walkmem, modules).
  Others are bare values: decimal thread ids (`threads`, MEc:339-341), hex strings
  (`getmem`, `xexfield`'s value line, MEf:423), or `Key: Value` pairs (`hwinfo`,
  MEc:283-290, MEc:459).
- No reference handles dot-stuffing (a body line beginning with `.`). None of the
  commands used here can produce one **[inf]**.

### 1.6 Binary responses (203)

There is no generic length. Each command frames its own payload, and the client must
know the framing before it sends the command:

| Command | Framing after `203-` | Sources |
|---|---|---|
| `getfile` | 4-byte **little-endian** unsigned length, then exactly that many bytes, no trailer | XC:430-466, NSc:118-123, ON consoles.ts:279-283, MEf:179-200 |
| `getmemex` | Blocks: 2-byte little-endian header (bits 0-14 byte count, bit 15 last block), then that many bytes; repeat | XL:79-92, XLs:78-101, MEc:597-627 |
| `screenshot` | One text line of `key=value` geometry, then `framebuffersize` raw bytes | ON consoles.ts:548-562 |

ME's generic "show me any 203" path assumes the `getmemex` block framing (MEc:640-661,
used from `SendCmdCommand.cs`); that is a guess, not a rule.

### 1.7 Client-to-console binary (204)

For `sendfile` the console answers `204- send binary data`; the client then sends
exactly the announced number of raw bytes, and the console answers once with a status
line, `200- OK` on success (XC:566, MEf:215-218, MEf:224-227). NSx:133-137 and
ON consoles.ts:342-350 never read that last line, which leaves it in the stream for
the next command **[≠]**. Details in 3.6.

### 1.8 Dedicated connections (205)

`notify` turns its connection into a one-way notification stream and answers 205
(ED:397-408, MEc:1176-1188, ME `TestDebuggerEventReceiver.cs:52-55`). From then on the
console sends notification lines (3.18) and the connection takes no further commands
in any reference **[inf]**. Notification lines may arrive *before* the 205 line on that
connection when a debugger is attached, because the console sends events in the
background (MEc:1168-1188) **[1]**.

### 1.9 Line terminators

Both directions use CR LF. Readers differ: NSc reads to CR and discards one more byte
(NSc:48-60), XLs ends a line at LF and drops CRs (XLs:28-39), ME ends at the LF after
a CR (MEc:794-823), ON splits on CR LF (ON reader.ts via `LINE_DELIMITER`,
constants.ts:1).

**Contract:** the client ends a line at LF and strips one trailing CR; the mock always
sends CR LF.

### 1.10 Ending a session, and no pipelining

- `bye` is answered `200- bye`, then the console closes the connection
  (ON xbdm.ts:38-45). ME and XLs send `bye` and close without reading (MEc:193,
  MEi:176, XLs:128).
- ON first sent every command as `command\r\nbye\r\n` in one write; that failed on a
  devkit and was changed to send `bye` only after the response was read (ON commit
  7a33519, "not working on devkit because of the bye command is handled") **[1]**.

**Contract:** one command in flight per connection. The client writes the next command
only after the previous response has been read completely (including any binary
payload and trailing status). The mock still reads robustly: a command split over
several segments, or two commands in one segment, are each handled in order.

### 1.11 Timeouts seen in the references

| Reference | Value |
|---|---|
| XC | `SO_RCVTIMEO` 10 ms on Linux, 5 s on Windows, used to *detect the end of a response* (XC:55-63, XC:646-672). Do not copy |
| NSc | 1 s connect, send and receive (NSc:15-17) |
| ON | 3 s socket timeout (ON xbdm.ts:60) |
| ME | 4 s for the greeting (MEt:108); 5 s without progress while reading a line or binary data, then the connection is closed (MEc:843-874, MEc:981-1047). One reader loop says 5 s but computes 5000 s (MEc:932) |
| XL | 8 s per request (XL:40, XLs:18); 30 s for `screenshot`, "the console can take a few seconds to hand over a frame" (XL:262-272) |

**Contract:** connect 5 s; greeting 5 s; afterwards an *idle* timeout (no byte
received) of 10 s per response, 30 s for `screenshot` and for the status line after
`sendfile` data; there is no total time limit, so a large `getfile` may take minutes
as long as bytes keep arriving. All three are settings of the client, injectable by
tests.

### 1.12 Several connections

- Consoles accept several connections at once: ED keeps a command connection and a
  notification connection and opens a third for every memory read or write (ED:152,
  ED:180, ED:193, ED:205, ED:399); ME keeps a command connection and an event
  connection (MEc:1148-1160); ON opens a connection per command (ON xbdm.ts:12).
- There is a limit: opening a connection per file of a large folder led to
  `401- max number of connections exceeded` (ON consoles.ts:319-320) **[1]**. The
  number is **[none]**.
- XL deliberately uses one connection with a request queue (XL:100-110, XLr:103) **[1]**.

**Contract:** the client opens one command connection per open console (the app's
per-mount worker serialises its use) and at most one notification connection. It opens
no connection per file. On `401` it reports "too many connections" and retries once
after closing its own idle connections.

### 1.13 Reconnecting, reserving and keepalive

- `notify reconnectport=N` is the only reconnect-related argument in the references:
  ED sends `reconnectport=0x00000001` (ED:20, ED:401), ME sends `reconnectport=12345`
  with `reverse` (MEc:1176) or without (`TestDebuggerEventReceiver.cs:52`). Both get
  205 and notifications on the same socket. ME's comment: "no idea what reconnectport
  does" (MEc:1175). Its meaning, and that of `reverse`, are **[none]**.
- `boxid`, `dedicate` (as a command) and any keepalive command appear in no reference
  **[none]**.
- No reference sends keepalives. ME checks its own socket state every 2 s (MEc:115-123),
  which detects nothing on an idle TCP connection.
- State on the console survives a dropped connection: XL remembers it stopped the game
  and sends `go` on the next connection (XL:120, XL:142-145, XL:227-228) **[1]**.

**Contract:** no keepalive traffic. A command connection found dead when the next
command is sent is reopened once, transparently, unless a transfer was in progress
(5.2). The client remembers whether it stopped execution and offers to resume after a
reconnect.

### 1.14 Debugger attach

`debugger connect override name="<tool>" user="<host>"` answers 200 (ED:417-426,
MEc:1163-1166). ME sends it before every `break` (MEf:318-337) and before `notify`;
ED once after connecting. `override` takes over an existing debugger session (ED:419).
DevTool uses XDevkit's `ConnectAsDebugger(..., Force)` (DT `Forms/MainForm.cs:89`). The
text of the matching disconnect is **[none]** (DevTool calls the COM method
`DisconnectAsDebugger`, `Forms/MainForm.cs:529`).

## 2. Discovery (UDP port 730)

### 2.1 Packets

All on UDP port 730, one datagram each. ON points to xboxdevwiki's "Name Answering
Protocol" for the format (ON consoles.ts:573).

| Type | Direction | Bytes | Sources |
|---|---|---|---|
| 1, forward lookup | client to broadcast | `01`, name length (1 byte), name (ASCII, no terminator) | NSu:42, ON consoles.ts:574-578 |
| 2, reply | console to client | `02`, name length (1 byte), name (ASCII) | NSd:206-213, MEd:43-54, ON consoles.ts:590 |
| 3, wildcard | client to broadcast (or one address) | `03 00` | NSu:19, NSu:59, MEi:102 (`BitConverter.GetBytes((short)3)`, little-endian) |

- Replies come from the console's address, which is the address to connect to
  (ON consoles.ts:599-601, MEi:126-137).
- Validation differs **[≠]**: ME's discovery requires `len + 2 == datagram size`
  (MEi:131); its parser only requires `len <= size - 2` (MEd:48-50); ON takes everything
  after byte 2 as the name (ON consoles.ts:590). NSd:208-212 checks only the type.
- ON checks that the reply to a type-1 lookup carries the name it asked for
  (ON consoles.ts:593-597). Whether the console matches names case-insensitively is
  **[none]**.

### 2.2 Procedure

- Destination `255.255.255.255:730` (ON `xbdm/utils.ts:294`, NSu:22, MEi:118).
  ME binds one socket per IPv4 interface address so the broadcast leaves on every
  network (MEi:96-118) **[1]**.
- Waiting: NSu 1 s (NSu:21); ON 500 ms, 3 tries (ON `xbdm/utils.ts:276-277`); ME 1.5 s
  per interface, 3 s overall (MEi:83, MEi:111).
- Several consoles answer a wildcard; NSu collects until the timeout (NSu:24-36); ON
  keeps only the first (TODO at ON `xbdm/utils.ts:302`).
- A unicast type 3 to one address checks that a console is there (NSu:57-77) **[1]**.
- A reply gives the name only. ME then connects over TCP and asks `dbgname`
  (MEi:134, MEi:161-185).
- Whether every console answers UDP name queries is **[none]**.

### 2.3 When UDP is blocked

Every reference also accepts a typed IP address (ON consoles.ts:64-73, MEi:62-69 with
the last address remembered, NSt:36-75). No reference scans a subnet over TCP.
DevTool resolves names through the XDK's Neighborhood default console (DevTool README),
which is Windows-only and out of scope.

**Contract:** Discovery broadcasts type 3 on every IPv4 interface, collects replies
for 1.5 s, retries twice, de-duplicates by address, then asks each console `dbgname`
over TCP (its answer wins over the UDP name). Adding a console by name sends type 1
and accepts a reply whose name matches case-insensitively. When nothing answers, the
user types an address; the app remembers consoles it has reached. No subnet scanning.
The mock answers type 3 always, type 1 only for its own name, and can be told to stay
silent (blocked UDP).

## 3. Commands

### 3.0 Support matrix

| Command | Seen in | Section |
|---|---|---|
| `dbgname` | XC, NSx, ON, ME, XL | 3.1 |
| `consoletype` | XC, ON | 3.1 |
| `getconsoleid` | ME | 3.1 |
| `xbeinfo` | XC, NSx, ED, ON, ME | 3.1 |
| `getexecstate` | ED, ME | 3.15 |
| `altaddr`, `getpid`, `hwinfo` | ME | 3.1 |
| `whomadethis` | ED | 3.1 |
| `help` | ME | 3.1 |
| `consolefeatures` | ME | 3.1 |
| `systeminfo`, `dmversion` | none | 3.1 |
| `drivelist` | XC, NSx, ON, ME | 3.2 |
| `drivefreespace` | XC, NSx, ON, ME | 3.2 |
| `dirlist` | XC, NSx, ON, ME | 3.3 |
| `getfileattributes` | XC only | 3.4 |
| `getfile` | XC, NSx, ON, ME | 3.5 |
| `sendfile` | XC, NSx, ON, ME | 3.6 |
| `sendvfile` | none | 3.7 |
| `mkdir` | XC, ON, ME | 3.8 |
| `delete` | XC, ON, ME | 3.9 |
| `rename` | XC, ONO, ON, ME | 3.10 |
| `setfileattributes` | none | 3.11 |
| `setsystime` | XC, ON | 3.12 |
| `screenshot` | ON, XL (through py-xbdm, not in references) | 3.13 |
| `magicboot` | XC, NSx, ED, ON, DT, ME | 3.14 |
| `shutdown` | NSx, ON, ME | 3.14 |
| `dvdeject` | ME | 3.14 |
| `stop`, `go` | ED, ME, XL | 3.15 |
| `suspend`, `resume` | ED, ME | 3.15 |
| `halt`, `continue`, `step` | none (only the `singlestep` notification) | 3.15 |
| `getmem` | ED, ME | 3.16 |
| `getmemex` | ME, XL | 3.16 |
| `setmem` | ED, ME, XL | 3.16 |
| `setmemex` | none | 3.16 |
| `walkmem` | ED, ME, XL | 3.16 |
| `modules`, `modsections`, `xexfield` | ME | 3.16 |
| `threads`, `threadinfo` | ME | 3.16 |
| `getcontext`, `setcontext` | ED, ME | 3.16 |
| `break`, `stopon` | ED, ME | 3.17 |
| `debugger` | ED, ME | 1.14 |
| `notify` | ED, ME | 3.18 |
| `bye` | NSc, ON, ME, XLs | 1.10 |

Each subsection gives syntax, response, errors and quirks. Examples use `>` for
client lines and `<` for console lines; CR LF is implied.

### 3.1 Console information

**`dbgname`** returns the console's debug name as the text of a 200 line.

```
> dbgname
< 200- MyDevkit
```

XC:113-125 (text from index 5), NSx:14, ON consoles.ts:53, MEc:405-407, MEi:171.
`dbgname name=<new>` sets it, unquoted in ME (MEc:497-499) **[1]**.

**`consoletype`** returns the type as text. XT answers `200- reviewerkit` (XT:287), XC
takes the text after `200- ` (XC:318-330), ON stores it (ON consoles.ts:61). The set of
values is not in the references **[none]**; XDevkit's numeric types are development
kit, test kit and reviewer kit (DT `Classes/XDKUtilities.cs:121-126`).

**`getconsoleid`**: `200- consoleid=<id>`; ME strips the first 10 characters
(MEc:401-403) **[1]**. The id's format (hex digits, length) is **[none]**.

**`xbeinfo running`** describes the running title; **`xbeinfo name="<path>"`** a file
(MEc:409-418). Response 202 with `key=value` lines; the `name` value is the path:

```
> xbeinfo running
< 202- multiline response follows
< timestamp=0x00000000 checksum=0x00000000
< name="\Device\Harddisk0\SystemExtPartition\20449700\dash.xex"
< .
```

The body above is XT's (XT:270-274); the real field set and whether `name` is a device
path or a drive path are **[none]**. NSx:42, ED:223, ON consoles.ts:529-534 and
MEc:409-418 read only `name`. ME's author notes `xbeinfo` did not work for modules
(MEf:407) **[1]**.

**`getexecstate`**: 200 with one word: `start`, `stop`, `pending`, `reboot`,
`pending_title`, `reboot_title` (MEc:445-456); ED tests for `start` (ED:246-249).

**`altaddr`**: `200- addr=0x<hex>`, the title IP address (MEc:485-491) **[1]**. ME
byte-swaps the number into .NET's `IPAddress`, so the value reads as the address in
network order: `0xC0A80102` is 192.168.1.2 **[inf]**.

**`getpid`**: `200- pid=0x<hex>` (MEc:478-483) **[1]**. ME byte-swaps it for display;
why is unclear.

**`hwinfo`**: 202 with `Key: Value` lines, in order flags, number of processors, PCI
bridge revision, reserved bytes, boot loader magic, boot loader flags (MEc:458-476)
**[1]**. Exact text **[none]**.

**`whomadethis`**: its text contains `Natelx` on the RGH XBDM; EmDbg treats any other
answer as devkit XBDM (ED:415, ED:218) **[1]**. What a console that does not know the
command answers (perhaps 407) is **[none]**.

**`help`**: 202 listing the commands the console knows (ME
`Commands/ListHelpCommand.cs`, `SendCommandAndReceiveLines("help")`) **[1]**. The line
format is **[none]**. It is the only capability probe in the references.

**`consolefeatures ver=2 type=<n> params="A\0\A\..."`** is JRPC2's extension
(MEj throughout): type 15 reads a temperature, 10 the CPU key, 13 the dashboard
version, 16 the title id, 17 the motherboard. ME probes JRPC2 with type 15 and, if the
answer is not 200, considers the connection "corrupted" and closes it (MEc:133-154)
**[1]**. Out of scope for the app; never sent on the command connection.

**`systeminfo`, `dmversion`**: not in any reference **[none]**. DevTool reads system
information (kernel, XDK version, flags, console revision) through XDRPC calls into
`xbdm.xex` ordinals 161 and 140 (DT `Classes/XDKUtilities.cs:113-133`), not through text
commands.

**Contract:** the console page shows `dbgname`, `consoletype`, `getconsoleid` and
`xbeinfo running` when they answer, and hides each field that errors. `systeminfo` and
`dmversion` are tried once per connection; a 4xx hides them.

### 3.2 Drives

**`drivelist`**: 202, one `drivename="<NAME>"` line per drive, name without colon or
backslash (XC:134-165, XT:95-103, ON consoles.ts:134-140, MEf:69-78).

```
> drivelist
< 202- multiline response follows
< drivename="DEVKIT"
< drivename="HDD"
< drivename="Z"
< .
```

The original Xbox instead answers one 200 line of drive letters (NSg:33-34); not a
360 behaviour.

**`drivefreespace name="<NAME>:\"`** with the trailing backslash (XC:182, NSx:56-59,
ON consoles.ts:147, MEf:81). Fields, each a 32-bit hex half of a 64-bit byte count:
`freetocallerhi/lo`, `totalbyteshi/lo`, `totalfreebyteshi/lo`.

```
> drivefreespace name="HDD:\"
< 202- multiline response follows
< freetocallerhi=0x00000003 freetocallerlo=0x1a2b0000 totalbyteshi=0x00000004 totalbyteslo=0x00000000 totalfreebyteshi=0x00000003 totalfreebyteslo=0x1a2b0000
< .
```

**[≠]** NSx, ON and ME read a 202 with one line (NSx:59, ON consoles.ts:144-148,
MEf:81-82); XT answers a single `200-` line (XT:122-128) and XC parses whatever it gets
(XC:189-191). The real form is 202 by majority **[inf]**. NSx records a drive without
sizes when the command fails (NSx:61-63), so it may fail on some drives.

Friendly names, "from neighborhood" (XC:167-179, ON `xbdm/utils.ts:84-101`): `DEVKIT` and
`E` Game Development Volume, `HDD` Retail Hard Drive Emulation, `Y` Xbox360 Dashboard
Volume, `Z` Devkit Drive, `D` and `GAME` Active Title Media, anything else Volume.
DevTool shows that XBDM keeps a table of drive names (up to 35 characters, A-Z and 0-9)
mapped to device paths starting with `\`, at most 42 entries, each browsable or not
(DT `Classes/NeighborhoodDrives.cs:46-112`, `:161`) **[1]**.

**Contract:** total = `totalbytes`, free = `freetocaller`, used = total - free (XC:192,
ON consoles.ts:184). The client accepts 200 or 202. A drive whose `drivefreespace`
fails is still listed, without sizes.

### 3.3 Directory listing (`dirlist`)

```
> dirlist name="HDD:\Content\"
< 202- multiline response follows
< name="0000000000000000" sizehi=0x0 sizelo=0x0 createhi=0x01d11fb5 createlo=0x59683c00 changehi=0x01d11fb5 changelo=0x59683c00 directory
< name="default.xex" sizehi=0x0 sizelo=0x0004f000 createhi=0x01d11fb5 createlo=0x59683c00 changehi=0x01d11fb5 changelo=0x59683c00
< .
```

- One line per entry; no `.` or `..` entries in any example **[inf]**.
- Fields: `name` (quoted), `sizehi`/`sizelo` (64-bit size in two 32-bit hex halves),
  `createhi`/`createlo` and `changehi`/`changelo` (FILETIME halves, 4.3), and a bare
  `directory` flag for folders (XC:232-253, NSd:186-194, ON consoles.ts:220-238,
  MEf:118-138). Field order in the example follows the order clients read them; XT
  writes `name`, times, then size (XT:166-179).
- ON detects folders with `line.endsWith(" directory")` (ON consoles.ts:225), so on its
  console the flag ends the line **[inf]**; ME searches for the flag anywhere (MEf:138).
- Other flags (`readonly`, `hidden`, ...) appear in no reference **[none]**.
- Errors: a path that does not exist is any non-202 (MEf:112-113); a protected path
  carries `access denied` in its text (MEf:110). The exact codes (402? 430? 414?) are
  **[none]**.
- Trailing backslash: XC and ME always add one (XC:209, MEf:106-107); ON sends drive
  roots with one (`HDD:\`) and subfolders without (ON commit fce4704, `drive-button.tsx:42`)
  **[≠]**. A drive root without the backslash (`HDD:`) is untested anywhere.

**Contract:** the client always sends `dirlist` with exactly one trailing backslash,
parses fields by name in any order, treats a missing size as 0 and missing times as
"unknown", and ignores flags it does not know. Entries named `.` or `..` are dropped.

### 3.4 `getfileattributes`

`getfileattributes name="<path>"` (XC:266-290) **[1]**. XC reads the line after the
first as `sizehi sizelo createhi createlo changehi changelo` (XC:279-287); XT answers
202 with one such line (XT:213-222). Whether the real reply is 200 or 202, carries the
`directory` flag, and what a missing path returns (XT says 404, which is wrong for a
file, 3.5) are **[none]**. XC uses it to test existence before uploading a folder
(XC:572-585).

**Contract:** the client parses 200 or 202 alike and treats any 4xx as "does not
exist or not accessible". `FileSystem::stat()` uses it when available and falls back to
listing the parent folder.

### 3.5 Download (`getfile`)

```
> getfile name="HDD:\file.bin"
< 203- binary response follows
< [4 bytes: length, unsigned 32-bit little-endian]
< [length bytes]
```

- The length prefix is little-endian in every reference (XC:430-436 reads a native
  `int` on x86, NSc:118-120, ON consoles.ts:279-280, MEf:179-183 "this value is always
  LE...???"), although the console is big-endian **[inf]**.
- Nothing follows the data: the console neither closes the connection nor sends a
  status line (ON consoles.ts:307-308, ON `app/[ipAddress]/files/download/route.ts:53-62`,
  MEf:185-200 reads exactly `length` and carries on) **[≠]** XC drains the socket after
  it "just in case" (XC:473-474).
- No checksum in any reference.
- No chunking in the protocol: the whole file follows as one stream. Clients read it
  in pieces of 1 KiB (XC:440), 64 KiB (MEf:186) or the stream's own (ON).
- Errors come instead of the 203: 414 for a directory or access denied (MEf:174-176),
  402 for no such file **[inf]** (XT says 404, XT:342).
- Files of 4 GiB or more cannot be described by the 32-bit length; what the console
  does then is **[none]**. FATX files are smaller than 4 GiB, so this matters only for
  other filesystems.
- Partial transfer and cancel: there is no way to stop a download in the protocol. The
  only way out is to close the connection (ON destroys the stream,
  ON `app/[ipAddress]/files/download/route.ts:39-45`).

**Contract:** the client reads exactly `length` bytes into the sink and only then
returns the connection to the idle state. Cancelling a download closes the connection
(it is reopened for the next command) and discards the partial local file, which the
app's sinks already do on a failed transfer. A `length` larger than the size `dirlist`
reported, or larger than the sink allows, is refused by closing the connection before
reading the body (5.2).

### 3.6 Upload (`sendfile`)

```
> sendfile name="HDD:\file.bin" length=0x2a
< 204- send binary data
> [0x2a raw bytes]
< 200- OK
```

- `length` is hex with `0x` (XC:512, ON consoles.ts:342, MEf:231 as `0x%08X`); NSd emits
  `0x%08X` too (NSd:145). Decimal is untested.
- Errors before 204: 413 cannot create (ME's note: "Does a directory with the same name
  exist?", MEf:232-233); 414 when the path is a directory (MEf:234-235).
- After the data the console answers one status line; 200 on success (XC:566,
  MEf:215-218). Errors after the data (a full device mid-way: 415?) are **[none]**.
- Data is sent as fast as TCP allows (MEf:206-212 writes 64 KiB pieces; ON pipes a
  stream, ON consoles.ts:346-349). XC sleeps 20 ms after every 1 KiB (XC:544-562);
  that is a workaround for its own receive loop, not a protocol need **[inf]**.
- An existing file of the same name is replaced: ON's upload deletes only folders that
  clash, not files (ON `components/upload-dropzone.tsx:61-80`, commit 0514255 asks the
  user before "replacing" a file) **[inf]**.
- Lengths of 4 GiB or more: **[none]**. ME passes an `int` (MEf:203, MEf:230).
- Cancel and drop: ON removed cancelling an upload because it "corrupted the file on
  the console" (ON commit acc1dc4) **[1]**. What remains after a dropped connection (a
  truncated file under the final name? nothing?) is **[none]**.
- No checksum.

**Contract:** an upload goes to a temporary name in the target folder
(`<name>.<random>.part`, a FATX-valid name of at most 42 characters), then is renamed
to the final name after `200`. If the final name exists the client deletes it right
before the rename (the app's Replace confirmation has already been given). A cancel or
a drop closes the connection; on the next connection the client deletes the temporary
file, best effort. The client never sends more or fewer bytes than announced.

### 3.7 `sendvfile`

Not in any reference **[none]**. Not used.

### 3.8 New folder (`mkdir`)

`mkdir name="<path>"`, 200 on success, `410` if it exists (XC:619-632, ON consoles.ts:404-424,
MEf:168-170). XT answers 400 for an existing folder (XT:517-520), contradicting XC:627
**[≠]**; 410 wins (XC, ON). ME's interface claims "all subdirectories will be created"
(ME `Connections/Features/IFeatureFileSystemInfo.cs:106`) **[1]**,
while ON creates each level itself when uploading a tree
(ON `components/upload-dropzone.tsx:97-118`).

**Contract:** the client creates one level per `mkdir` and maps 410 to "already exists".

### 3.9 Delete

`delete name="<path>"` for a file; `delete name="<path>" dir` for a folder
(XC:609, ON consoles.ts:373-377, MEf:147-154).

- Not recursive: every reference empties folders itself, depth first (XC:599-617,
  ON consoles.ts:357-378, ME `Connections/Features/IFeatureFileSystemInfo.cs:66-88`).
- A folder without `dir` gives 414; ME tries without and retries with `dir`
  (MEf:147-154). A non-empty folder presumably gives 411 **[inf]** (MEr:68-69).
- Deleting a missing path: XT says 404 (XT:483-486), real code **[none]**.

**Contract:** the client knows from the listing whether a path is a folder and sends
`dir` accordingly; `remove()` of a folder deletes its contents first, depth first, and
stops at the first error.

### 3.10 Rename and move

`rename name="<old full path>" newname="<new full path>"`, 200 on success
(XC:634-644, ON consoles.ts:390-394, MEf:164-166). It also moves between folders: ONO's
cut and paste and ME's `MoveFile` are `rename` to another folder
(ONO `src/Panels/ContentsPanel.cpp:161-175`, MEf:164).

- Renaming onto an existing name: ON deletes the target first
  (ON `components/files-page-context-menu.tsx:55-70`), suggesting the console refuses
  **[inf]**; XT refuses with 400 (XT:568-572). Real behaviour **[none]**.
- Across drives: the error table has 409 "must copy, not moved" (MEr:62-63), so a
  cross-volume rename is probably refused with 409 **[inf]**.

**Contract:** `rename()` stays inside one folder and one drive. The client checks that
the new name does not exist before sending.

### 3.11 `setfileattributes`

Not in any reference **[none]**. Not used; the app has no attribute editing.

### 3.12 Clock (`setsystime`)

`setsystime clockhi=0x<hex> clocklo=0x<hex>`, a FILETIME (XC:340-362, ON consoles.ts:506-521).
A `tz` argument exists but its format is unknown (XC:346-348) **[1]** **[none]**. Errors:
406 "clock not set" belongs to reading the clock **[inf]**. Not needed by the app's
milestone; listed for the mock.

### 3.13 Screenshot

```
> screenshot
< 203- binary response follows
< pitch=0x00000c00 width=0x00000500 height=0x000002d0 format=0x... offsetx=0x0 offsety=0x0, framebuffersize=0x...
< [framebuffersize bytes]
```

ON consoles.ts:537-570 **[1]** for the framing (a port of a Go client, ON
`xbdm/utils.ts:111-113`). Fields `pitch width height format offsetx offsety
framebuffersize`; a value may be followed by a comma, which the parser strips
(ON `xbdm/utils.ts:69-73`). The pixels are the GPU's tiled 32x32 layout and must be
untiled; ON supports only format 8888, 8-in-32 endianness, tiled, unsigned
(ON `xbdm/utils.ts:125-174`), and refuses others. The console can take seconds to answer
(XL:264). The exact field order and spacing are **[none]**.

**Contract:** not in the first M3 deliverable; when added, the client reads the line,
checks `framebuffersize` against `pitch * height` (with tiling slack) and a cap of
64 MiB, then reads the bytes.

### 3.14 Reboot, launch, shutdown

| Command | Effect | Response |
|---|---|---|
| `magicboot` | Warm reboot. XC and ON call it "go to dashboard" (XC:377-392, ON consoles.ts:440-447); ED and ME call it "restart title" / "soft reboot" (ED:241-244, MEt:64) **[≠]** | 200 expected by XC, ON and ME; XC waits 20 ms first because the answer is slow (XC:381-383, XBDM commit b8487a4) |
| `magicboot cold` / `COLD` | Cold reboot | **[≠]** ON expects the console to close the connection with no answer (ON consoles.ts:461-479); XC expects 200 (XC:364-375); ME reads a response, then closes (MEc:373-376) |
| `magicboot title="<path>" directory="<dir>"` | Launch an executable | **[≠]** ON expects 200 (ON consoles.ts:437); XC reads nothing ("Nothing should be received but just in case", XC:296-299); `directory` given without trailing backslash (XC:294, ON consoles.ts:432) or with one (MEf:157-161). A file in the root of `FLASH:` is launched with `title` only (DT `Classes/XDKUtilities.cs:1051-1064`) |
| `shutdown` | Power off | Connection closed with no answer (ON consoles.ts:482-505, MEc:378-381) |
| `dvdeject` | Open the tray | 200 **[1]** (MEf:58) |

XT's author removed argument checks from its `magicboot` "because the actual XBDM
server on an Xbox 360 doesn't do any" (XBDM commit 15d6b6e) **[1]**. After any reboot all
connections drop; ED treats an `execution rebooting` notification as the signal to
disconnect (ED:59-65).

**Contract:** these are user actions with a confirmation. The client sends the command
and accepts either a 2xx or the connection closing within the idle timeout as success;
afterwards the console is shown as rebooting and the command connection is reopened on
demand.

### 3.15 Execution control

- `stop`: 200, or 426 when already stopped (MEc:383-390, XL:200-206).
- `go`: 200, or 408 when not stopped (MEc:392-399, XL:204).
- `suspend thread=0x<id>` / `resume thread=0x<id>`, 200 (MEf:376-378). ED has the two
  swapped (ED:251-263), a bug.
- `getexecstate` (3.1).
- `halt`, `continue`, `step`: in no reference **[none]**; ME's `StepThread` is a TODO
  (MEf:380-383). Single-stepping shows only as the `singlestep` notification (MEe:41).
- Speed: while a title runs, XBDM gets little CPU time (about 100 KB/s, 28 ms per
  request); stopped, about 1-1.4 MB/s (XL:107-109, XLr:103-108) **[1]**. XL, ME's scanner,
  dumper and viewer stop the console around large reads and resume it afterwards, but
  only if they stopped it (XL:208-228, ME `Engine/Scanners/FirstTypedScanTask.cs:103-111`,
  ME `Commands/DumpMemoryCommand.cs:191-224`).

**Contract:** the memory viewer offers "pause while reading" (default off for small
reads, on above 256 KiB, as XL's 0x40000 threshold, XL:112). The client remembers if it
sent the `stop` and sends `go` afterwards, also after a reconnect.

### 3.16 Memory and process state

Addresses are 32-bit. ME's default viewer address is `0x82000000`, where titles load
(ME `Engine/HexEditing/MemoryViewer.cs:43`); XL reads the emulator there too (XL
`guest.py:18`).

**`getmem addr=0x<hex> length=0x<hex>`** (ED:177-188, MEc:565-595):

```
> getmem addr=0x82000000 length=0x8
< 202- multiline response follows
< 4D5A9000????????
< .
```

- 202, then lines of hex digit pairs, two per byte, in ascending address order; lines
  are concatenated (ED:184-185, MEc:572-574).
- An unreadable byte is `??`; ME zero-fills it (MEc:585-587) and uses a `?` anywhere to
  call a range protected (MEc:501-524). Whether `?` comes in pairs is **[inf]**.
- Bytes per line: ME's comment says "typically 128 when reading big chunks" (MEc:582)
  **[1]**. Clients must not depend on it.
- The total must equal `length`; ME throws otherwise (MEc:576-578).
- Hex digit case **[none]**; parse both.
- Slow: about 7 KB/s according to XL (XL:3-4) **[1]**. ME uses it only up to 128 bytes,
  where it is faster than `getmemex` (MEc:553-563).

**`getmemex addr=0x<hex> length=0x<hex>`** (XL:78-92, XLs:73-104, MEc:597-627) **[≠]**
in the details:

```
> getmemex addr=0x82000000 length=0x800
< 203- binary response follows
< [2 bytes LE: 0x0400] [0x400 bytes]
< [2 bytes LE: 0x8400] [0x400 bytes]
```

- Blocks of "up to 0x400 bytes" (XL:79, XLs:78) **[1]**, each after a 2-byte
  little-endian header: bits 0-14 the byte count, bit 15 set on the last block (XL's
  reading) or "status" with "most likely reading invalid/protected memory" (ME's reading,
  MEc:605-616).
- When bit 15 comes before `length` bytes have arrived, the rest is unreadable: XL
  reports the first unreadable address (XL:90-91), ME zero-fills the rest (MEc:607-610).
- A block larger than what remains: ME throws (MEc:617-619); XLs reads it and keeps only
  what was asked (XLs:87-93) **[≠]**.
- The end: every reader stops as soon as `length` bytes have arrived, whether or not that
  block has bit 15 set, and reads no further header (XL:88, XLs:99, MEc:626). A console
  that sent an empty last block after the data would leave it for the next command.
- Sizes used successfully: up to 0xC000 per request in ME (MEc:126-128, "3/4th of the
  receive buffer"), 0x10000 in ME's dumper (ME `Commands/DumpMemoryCommand.cs:204-205`),
  0x8000 while running and 0x20000 while stopped in XL (XL:238). The console's maximum is
  **[none]**.

**`setmem addr=0x<hex> data=<hex digits>`** (MEc:663-677, XL:94-97, XLs:106-119, ED:190-200):

```
> setmem addr=0x82000000 data=DEADBEEF
< 200- set 4 bytes
```

- 200 on success; 404 when memory is not mapped, which ME accepts without error
  (MEc:670-672). The 200 text is not quoted by any reference (shown as an example only).
- Data per command: 64 bytes in ME (MEc:666), 128 in XL (XL:96) **[≠]**; both under the
  line limit (1.3). ED sends the data quoted (`data="..."`, ED:196) **[1]**.
- Hex case: ME uppercase (MEc:723-728 from a table), XL lowercase (XL:97) **[inf]**: both
  accepted.
- Whether a partly unmapped range is partly written, and what the 200 text says about
  it, is **[none]**.

**`setmemex`**: not in any reference **[none]**.

**`walkmem`**: 202, one line per committed region (ED:149-166, MEc:420-443, XL:193-198):

```
< base=0x82000000 size=0x00a40000 protect=0x00000004 phys=0x00000000
```

ME skips regions it must not touch, because touching them can freeze the console:
for reading, protections with `0x280` (NoCache, ExecuteWriteCopy); for writing, those
with `0x1222` (MEc:426-433, comment "we cannot clear memory caches via XBDM ... without
risking freezing the console") **[1]**.

**`modules`**: 202, lines with `name="..." base=0x.. size=0x.. check=0x.. timestamp=0x..
osize=0x..` (MEf:385-413, ME `PluginXbox360Xbdm.cs` `FillModuleManager`) **[1]**; ME reads
`timestamp` as Unix seconds (MEf:411).

**`modsections name="<module>"`**: 202, lines `name="..." base= size= index= flags=`; 402
when unknown (MEf:429-447) **[1]**.

**`xexfield module="<module>" field=0x10100`**: 202, `fieldsize=0x..` then the value as
hex; 437 when the field is absent (MEf:415-426) **[1]**.

**`threads`**: 202, one decimal thread id per line (MEc:332-341) **[1]**.
**`threadinfo thread=0x<id>`**: 202 with one line `suspend= priority= tlsbase= base= limit=
slack= nameaddr= namelen= proc= lasterr=`, or 405 (MEc:292-330) **[1]**. The name is read
from memory at `nameaddr`.

**`getcontext thread=0x<id> control int fp`**: 202, lines `Name=0x...` (32-bit) or
`Name=0q...` (64-bit): `Cr Msr Xer Iar Lr Ctr Gpr0..Gpr31` and floating point (MEf:339-374,
ED:344-364). ED reads them as one argument list, ME line by line **[≠]** in layout
only. **`setcontext thread=0x<id> Name=value ...`**, split over several commands because
of the line limit (ED:366-395).

**`getpid`** (3.1).

**Pointer chains and scans** (what the memory tools build from the above): read 4 bytes,
interpret big-endian, add an offset, repeat (ME `Connections/BaseConsoleConnection.cs:124-150`; 0 or above 0xFFFFFFFF ends the chain); scans read `walkmem` regions in
64 KiB chunks with an overlap for values crossing a chunk border
(ME `Engine/Scanners/DataTypedScanningContext.cs:46`, `FirstTypedScanTask.cs:116-190`),
often with the console stopped (3.15).

**Contract:** the memory viewer reads with `getmemex` in requests of at most 0x8000 bytes
(0x20000 while stopped), falls back to `getmem` (at most 0x400 bytes per request) when
`getmemex` answers 407, shows unreadable bytes as `??`, and starts read-only. Writes use
`setmem` with at most 64 bytes per line, are refused for regions whose `walkmem`
protection has `0x200` (NoCache), and need the explicit unlock from the design goals.

### 3.17 Breakpoints

- `break addr=0x<hex>` sets an execution breakpoint; `break addr=0x<hex> clear` (ME) or
  `break clear addr=0x<hex>` (ED) removes it (MEf:318-321, ED:311-332).
- `break read|write|readwrite|execute=0x<hex> size=0x<hex> [clear]` for data breakpoints
  (ED:273-309, MEf:323-337). ME sends `read` for read-or-write (MEf:330), ED `readwrite`
  **[≠]**.
- `break clearall` (ED:265-271).
- `stopon fce` makes the console stop on exceptions; ED sends it only to the Natelx XBDM
  because it "causes exceptions frequently on dk xbdm" (ED:427-434, EmDbg commit a3aa2bf)
  **[1]**.

Not in the first M3 deliverable; the mock implements `break` only to answer 200.

### 3.18 Notifications

Opened on a separate connection (ED:397-408, MEc:1148-1188):

```
> debugger connect override name="UnnamedGpl3QtApp" user="host"
< 200- connected
> notify reconnectport=0x3039 reverse
< execution stopped            (may come before the 205)
< 205- now a notification channel
< debugstr thread=0xfb000010 lf string=Hello world
```

(Texts after the codes are illustrative; no reference quotes them.)

One line per event, the first word naming it (MEe:28-215, ED:36-135):

| Event | Fields | Source |
|---|---|---|
| `execution` | flag `started`, `stopped`, `pending`, `rebooting`, `reboot_title`, `pending_title` | MEe:116-131, ED:55-69 |
| `debugstr` | `thread=`, flags `lf`, `stop`, then `string=` running to the end of the line, unquoted | MEe:133-145, NSd:60-64 ("the 'string' value in debug logs is weird") |
| `break`, `singlestep` | `addr=`, `thread=`, flag `stop` | MEe:40-50, ED:89-100 |
| `data` (ME) / `databreak` (ED) **[≠]** | `addr=`, `thread=`, one of `read= write= readwrite= execute=` | MEe:42-62, ED:102-127 |
| `exception` | `code= thread= address=`, `read=` or `write=`, flags `first`, `noncont` | MEe:67-88, ED:70-87 |
| `rip` | `thread=` | MEe:90-94 |
| `assert` | `thread=`, then `string=` or flag `prompt` | MEe:96-114 |
| `create` / `terminate` | `thread=`, `start=` | MEe:147-160 |
| `modload` | `name=" " base= size= timestamp= checksum=`, flags `tls`, `xbe` | MEe:162-186 |
| `sectload` / `sectunload` | `name= base= size= index= flags=` | MEe:188-206 |
| `fiber` | `id=`, `start=` or flag `delete` | MEe:208-224 |
| a first word containing `!` | external, from a plugin | MEe:33-35 |

Not in the first M3 deliverable. When used: the client treats a notification connection
as receive-only, parses known events, keeps unknown ones as raw text, and tears down all
connections on `execution rebooting`.

## 4. Data encodings

### 4.1 Paths

- `DRIVE:\folder\file`, backslash separators (all references). Drive names have one or
  more letters (`HDD`, `DEVKIT`, `GAME`, `FLASH`, `E`, `Z`), which is why ON wrote its own
  path join after Node stopped accepting multi-letter drives (ON `utils.ts:85-113`).
- A drive root is `DRIVE:\` (ON `drive-button.tsx:42`, `drivefreespace` everywhere).
- Executables may be named by device path, e.g. `\Device\Harddisk0\SystemExtPartition\...`
  in XT's `xbeinfo` (XT:273) and DevTool's drive table (DT `Classes/NeighborhoodDrives.cs:112`).
- Whether forward slashes are accepted, whether names compare case-insensitively, and
  the maximum path length are **[none]**.

**Contract:** the app's virtual paths map as `/HDD/a/b` to `HDD:\a\b`; drives are the
top-level folders. The client never sends `/`. Names are compared case-insensitively
for clash checks (FATX rules).

### 4.2 Quoting

String values are wrapped in `"`; no reference escapes anything inside (XC:209,
ON consoles.ts:216, MEf:109). Parsers differ on embedded quotes **[≠]**: XC and ON end the
value at the next `"` (XC:704-716, ON `xbdm/utils.ts:3-38`); NSd treats `\"` as escaped
(NSd:115-123), which would break on a path ending in `\` before the closing quote; ME
treats `""` as a literal quote (MEp:130-134). Whether the console understands any escape
is **[none]**. Spaces inside quotes are fine.

**Contract:** the client refuses, before sending, any name or path containing `"`, CR,
LF, NUL or a byte above 0x7E; FATX forbids `"` in names anyway (the app's FATX name
check, `src/core/FatxFileSystem.cpp:405-423`). Parsers end a quoted value at the next `"`.

### 4.3 FILETIME

64-bit count of 100 ns intervals since 1601-01-01, sent as two 32-bit hex halves
(`createhi`/`createlo`, `changehi`/`changelo`, `clockhi`/`clocklo`). Unix seconds =
FILETIME / 10,000,000 - 11,644,473,600 (XC:6-7, ON `xbdm/utils.ts:103-109`). NSd and ME treat
it as UTC (NSd:191-192, MEf:130-135). Whether the console reports UTC or local time is
**[none]**; the app's FATX notes say FATX stores local time (`docs/ROADMAP.md`, M1).

### 4.4 Numbers

- `0x` hex for 32-bit values (all), `0q` hex for 64-bit (NSd:90-93, MEp:60, MEf:365-369),
  plain decimal (thread ids, JRPC2 `type=`). ME parses unprefixed values as decimal
  (MEp:44-46); NS also (NSd:94-109).
- Leading zeros optional: XC writes `0x2a` (XC:512), NS and ME `0x0000002A` (NSd:145,
  MEf:231).
- Hex digits in either case.

**Contract:** the client writes `0x` with lower-case digits, no padding; parsers accept
`0x`, `0X`, `0q`, and decimal, upper and lower case, and refuse values that overflow
their width.

### 4.5 Endianness

- Console memory is big-endian: ME defaults to big-endian (MEi:43-46, "xbox is BE by
  default"), DevTool wraps memory in a big-endian reader (DT `Forms/MainForm.cs:90`).
  `getmem` and `getmemex` return bytes in address order, so a 32-bit value reads
  big-endian **[inf]** (every decoder copies bytes in order).
- Binary length headers are little-endian (`getfile`, `getmemex`; 1.6).
- Values inside an emulated original-Xbox title are little-endian (xefu, XL `sdk/cpp360/ogxbox.h:9`);
  out of scope.

### 4.6 Sizes in one place

| What | Value | Status |
|---|---|---|
| `getfile` length | 32-bit; file under 4 GiB | all references |
| `sendfile` length | announced in hex; no maximum known | **[none]** |
| `getmem` per request | ME uses up to 128 bytes | client choice |
| `getmemex` per request | 0x8000 to 0x20000 used; block size up to 0x400 | **[1]**, maximum **[none]** |
| `setmem` per line | 64 (ME) or 128 (XL) bytes | **[≠]** |
| Command line | limited, value unknown | **[none]** |
| Connections | limited, value unknown | **[none]** |
| Drive table | 42 entries, names up to 35 characters | **[1]** DevTool |
| UDP name | 1-byte length | all |

## 5. Obligations

### 5.1 Mock server

The mock runs in the test process on a loopback port of its choosing (the client's port
is configurable; 730 is the default only for real consoles), or on an in-memory
transport if sockets are unavailable. It serves a virtual tree of drives and files and
a sparse memory map. It must:

1. Send `201- connected\r\n` on accept; accept bytes that arrive before it.
2. Read commands as CR LF lines (also bare LF), in order, one at a time, however they
   are split across segments; answer `407- unknown command` for unknown commands and
   ignore nothing.
3. Parse arguments in any order, keys and flags case-insensitively, quoted values up to
   the next `"`, tolerate trailing spaces; refuse a line longer than its limit with 406.
4. Answer every command in this document that the client uses, with the shapes given:
   200 single lines, 202 bodies ended by `.`, `getfile` with a 4-byte little-endian length
   and no trailer, `getmemex` in blocks of at most 0x400 bytes with the last-block bit,
   `sendfile` with 204 then exactly `length` bytes then 200.
5. Errors as the references show them: 410 `mkdir` on an existing name; 414 `delete`
   of a folder without `dir`, `getfile` of a folder, `sendfile` onto a folder; 404 for
   `setmem` on unmapped memory; `??` pairs from `getmem` and an early last block from
   `getmemex` for unreadable memory; 405 unknown thread; 408 and 426 for `go` and `stop`.
   Where the references leave the code open, the mock uses these choices, and tests may
   rely only on "some 4xx" for them: 411 deleting a non-empty folder; 402 for missing
   files and paths; 414 with `access denied` in the text for paths marked protected; 413
   when the parent folder is missing; 410 for `rename` onto an existing name.
6. `bye`: answer `200- bye` and close.
7. `magicboot cold` and `shutdown`: close without an answer (the strictest reading);
   plain `magicboot`: answer 200, then close; a setting switches cold reboot to "answer
   200 first".
8. Treat a dropped connection during `sendfile` like a console might: keep the bytes
   received so far under the target name (the worst case, so the client's clean-up is
   tested).
9. Enforce a connection limit (default 4) with `401- max number of connections exceeded`
   followed by close.
10. Offer fault injection, each a test switch: silence after the greeting; silence in the
    middle of a 202 body or a binary payload; close in the middle of a line, a body or a
    payload; a status line split byte by byte; an unknown status code (`299-`, `500-`,
    `abc`); a 203 where 200 was expected; a `getfile` length larger than the file
    (the console then stalls) and one above 4 GiB - 1 (0xFFFFFFFF); a `getmemex` block
    larger than requested; notifications interleaved before 205; UDP silent; UDP replies
    malformed (wrong type byte, length beyond the datagram).
11. Answer UDP type 3 with type 2 carrying its name, type 1 only when the name matches
    (case-insensitive), and ignore other datagrams.
12. Never depend on timing: no sleeps in the protocol path, so tests are deterministic
    and sanitizer runs stay fast.

### 5.2 Client

1. **Framing.** Read lines to LF, strip one CR, cap a line at 64 KiB (longer is a
   protocol error). Never interpret a byte stream as lines while a binary payload is
   pending, and never leave payload bytes in the buffer for the next command.
2. **One command at a time** per connection (1.10); a mutex or the per-mount worker
   guarantees it.
3. **Status handling** as in 1.4: 2xx expected by the command proceeds; 4xx becomes a
   `Status` failure with the code and text, connection kept; anything else drops the
   connection.
4. **Timeouts** as in 1.11, idle-based, with a cancel that works at any point (the app's
   `Status` cancel). Cancel during a response drops the connection, because the rest of
   the response cannot be skipped reliably.
5. **Huge or wrong lengths.** Refuse before reading: a `getfile` length above the size
   from the listing (when known) or above the sink's limit; a screenshot above 64 MiB; a
   `getmemex` block above 0x7FFF (impossible) or above what remains (drop, as ME does;
   do not read past). Never allocate from a length the console sent: stream into the sink
   in pieces of at most 64 KiB.
6. **Silence.** A console that stops sending trips the idle timeout; the connection is
   closed, the job fails with "the console stopped responding", and queued commands for
   that console fail at once instead of each waiting for its own timeout (XL:162-174 does
   the same).
7. **Drops mid-transfer.** `getfile`: the partial local file is discarded (the sink is
   not finished). `sendfile`: the temporary remote file is deleted on the next connection,
   best effort; the final name is never touched until the upload has its 200 (3.6).
8. **Reconnect.** A drop while idle is repaired once on the next command. The greeting
   is required again; no state is assumed to persist except what the client itself
   changed (execution stopped, debugger attached).
9. **Input checks** before sending (4.2): no `"`, CR, LF, NUL or non-ASCII in names; one
   `mkdir` per level; `rename` within one folder; `setmem` at most 64 bytes per line.
10. **No pipelining, no keepalive, no per-file connections** (1.10, 1.12, 1.13).
11. **Parsing.** Fields by key in any order; unknown keys and flags ignored; missing
    optional fields default sensibly; values that do not parse fail that entry, not the
    listing (the listing reports how many entries were skipped).
12. **Portability.** Sockets live behind a small transport interface (POSIX and Winsock),
    which also lets the tests run the client against the mock without sockets.

## 6. Open questions (for the user, on hardware)

Each needs a real console (or an emulator that implements XBDM) to settle.

1. What exactly does `getfileattributes` return (200 or 202, which fields, the
   `directory` flag), and which error for a missing path?
2. Which codes does `dirlist` give for a missing folder, a file instead of a folder, and
   a protected folder? Does `dirlist name="HDD:"` (no backslash) work?
3. Does `dirlist` have flags besides `directory` (read-only, hidden, ...), and is the
   field order always name, size, times, flags?
4. Is `drivefreespace` 200 or 202, and which drives refuse it?
5. Does `sendfile` onto an existing file replace it? Does it need the parent folder to
   exist (413)?
6. After a connection drops during `sendfile`, what is left on the console: nothing, a
   truncated file under the final name, or something else? Does a later command on a new
   connection work at once?
7. What does the console answer after `sendfile` data when the device fills up, and when
   does it send it?
8. Can `getfile` and `sendfile` handle 4 GiB or more on any console (`DEVKIT:` and other
   non-FATX volumes)?
9. Does `rename` onto an existing name fail, and with which code? Does a rename across
   drives give 409?
10. Does `mkdir` create missing parent folders (ME's claim)?
11. Which code does deleting a non-empty folder give (411?), and deleting a missing path?
12. Are path names compared case-insensitively? Are forward slashes accepted? What is the
    maximum path length?
13. Is there any escape for `"` in quoted values?
14. What is the maximum command line length (406 or 446 when exceeded)?
15. How many connections does a console allow at once (401)? Does it count the
    notification connection?
16. Does the console close idle connections, and after how long? Is a keepalive needed?
17. What do `reconnectport` and `reverse` do on `notify`? Do `boxid` or a `dedicate`
    command exist?
18. What text does the 205 line carry, and does a dedicated connection accept further
    commands?
19. Are FILETIMEs from `dirlist` UTC or local time? Does `setsystime` take `tz`, in what
    format?
20. What values does `consoletype` return, and what do `systeminfo`,
    `dmversion` and `getconsoleid` return, if they exist?
21. What does `whomadethis` return on a devkit?
22. What does `help` list, and in what format? Can it serve as a capability
    probe?
23. Does `magicboot cold` answer 200 before the connection closes, or close at once?
    Does plain `magicboot` go to the dashboard or restart the title? Does `magicboot
    title=` answer, and is `directory` needed with or without a trailing backslash?
24. Is `getmemex` always present? What is its largest
    accepted `length`, its block size, and the exact meaning of bit 15?
25. `getmem` on unmapped memory: 202 with `??`, or an error? How many bytes per line?
26. Does `setmem` on a partly unmapped range write the mapped part? What is its 200 text?
27. Do `setmemex`, `sendvfile` and `setfileattributes` exist, and in which
    form?
28. Which `walkmem` protections are unsafe to read (ME's NoCache warning)?
29. Does sending a command before the greeting, or two commands in one packet, break any
    target (ON's devkit `bye` problem)? Does it depend on the console?
30. Does a type-1 lookup match names case-insensitively?
31. Is the `getfile` length really little-endian?

## 7. Known defects in the references (do not copy)

- XC finds the end of a response by a 10 ms receive timeout on Linux and 5 s on Windows
  (XC:55-63, XC:646-672), sleeps after every command and every 1 KiB of upload
  (XC:681, XC:561), and truncates after the first `.\r\n` (XC:665-669).
- XC compares the exact `203-` and `204-` header text and reads it with a fixed-size
  `recv` (XC:403-428, XC:516-542), and reads the 4-byte length with one `recv` that may
  return fewer bytes (XC:431-436).
- XT: `dirlist` sends `.` after every entry (XT:181); `sizehi`/`sizelo` are
  `size & 0xffff0000` and `size & 0xffff` (XT:174-175, XT:216-217); `magicboot` answers
  without CR LF (XT:253); missing files are 404 (memory not mapped) instead of a file
  error (XT:156, XT:342); `sendfile` sends 204 before it knows it can create the file
  (XT:418-427); `mkdir` on an existing name is 400, not 410 (XT:517-520).
- ONO's paste uses `XboxPath::FileName()`, which drops the extension
  (ONO `src/Panels/ContentsPanel.cpp:169`, `XBDM/src/XboxPath.cpp:29-45`).
- NSc and ON never read the `200` after `sendfile` data (NSx:133-137, ON consoles.ts:342-350);
  NSc reads binary bodies with single `Stream.Read` calls that may return short
  (NSc:118-123).
- ED swaps `suspend` and `resume` (ED:251-263).
- ME's reader loop computes a 5000-second timeout where it means 5 s (MEc:932).
