# JRPC Protocol Contract

> Written 2026-10-08 from the third-party tools cloned in `references/` of the
> UpdClient repository (read-only, git-ignored here). Nothing here has been
> checked against a console or an emulator yet: every fact is "what the references
> do", and the open questions at the end are to be settled on hardware.

**JRPC** here means the `JRPC.xex` plugin: a **standalone TCP server** the console
runs, which executes remote procedure calls and a handful of system actions carried
in a single text command line (`consolefeatures ...`). It is *not* XBDM and does not
ride on port 730. Its on-connect banner literally reads `JRPC2 connected` and its
internal version field is `2`; despite that wording this document never calls it
"v1" (the name settled with the user is just "JRPC").

This must not be confused with **JRPC2** (`JRPC2.xex`), a different plugin that hooks
**xbdm.xex** and answers `consolefeatures` as an XBDM command over port 730 (another
agent documents it). The distinction matters because the `JRPC.dll` / `JRPC.cs`
client shipped in almost every tool here is a **JRPC2 client**, not a JRPC (TCP)
client — see section 0.4. This document is the contract for a JRPC (TCP) client in
UpdClient (`protocols/jrpc`) and a mock server for its tests. Where the references
disagree, or a fact rests on one source only, it says so; where the client must pick
a behaviour anyway, the choice is marked **Contract:**.

## 0. How to read this

### 0.1 Sources

Citations are `TAG:line` for text, or `TAG @VA` for a virtual address inside the
disassembled server image. Paths are relative to `references/`.

| Tag | File | Licence | What it is |
|---|---|---|---|
| JC | `xdevkit-xrpc-jrpc2-example/XboxCore.Xbox.Connection/JRPC.cs` | none stated | C# `JRPC` client class. Builds the `consolefeatures` command text and parses replies. Transport is `IXboxConsole.SendTextCommand` (XBDM/xdevkit COM), so strictly it is a **JRPC2** client — but it is the only readable source for the command-line *text* that the JRPC TCP server also parses, so it is the primary evidence for request/response wording. |
| JX | `All-RPC-Tools-Trainers/bo2_shit_1.7.3-xdevkit-xrpc-jrpc-jrpc2-rpc/Plugins/JRPC.xex` | none stated | The **JRPC TCP server** binary itself (Xbox 360 PPC, `JRPC.pdb` = `...\Projects\JRPC\JRPC\Release\JRPC.pdb`). Zero-key encrypted + LZX; unpacked per section 0.3. Facts cited `JX @0x9117xxxx` are from my disassembly of its `.text` (load VA `0x91170000`, `.text` at `0x91175000`). This is the only source for TCP transport, framing, banner and port. |
| JI | `All-RPC-Tools-Trainers/bo2_shit_1.7.3-xdevkit-xrpc-jrpc-jrpc2-rpc/Plugins/JRPC.ini` | none stated | Server config file read off the console HDD. |
| JDLL | `All-RPC-Tools-Trainers/{aio_tool_devexpress-xdevkit-xrpc-jrpc2,extra_shit-xdevkit-jrpc2,one_tool-xdevkit-jrpc2,XRPC_Tool-xdevkit-xrpc-jrpc2,Tsunami-xdevkit-jrpc2/bin/Debug}/JRPC.dll`, `The_Cipher_Mod_Tool-xdevkit-jrpc2/.../JRPC.dll`, `Xnotify source-xdevkit-jrpc2/JRPC.dll` | none stated | The compiled client DLL bundled in the labelled tools. Two byte-identical variants (md5 `1a720eff…` and `767b303f…`); both decompile to the same class as JC. All use XBDM, i.e. all are JRPC2 clients (section 0.4). |

The server binary at `JRPC2-XRPC-Trainers/JRPC2.xex` and `Xnotify source-xdevkit-jrpc2/JRPC2.xex`
(md5 `7f286c6c…`, `JRPC2.pdb`) is the **JRPC2** plugin and is cited only to draw the
boundary (section 0.4). `references/JRPC2-client` was deleted by the user and is not used.

### 0.2 Markers

- **[1]** seen in one source only.
- **[≠]** sources disagree; the alternatives are listed.
- **[inf]** inferred (here, usually from reading the server disassembly), not stated literally.
- **[none]** in no source; only hardware can tell (section 10).
- **Contract:** what the app's client and mock do, where the sources leave a choice.

### 0.3 Unpacking the server binary (reproducible)

`JRPC.xex` is a retail XEX2, `enc=1` (zero AES key), `comp=2` (LZX, 32768-byte window).
Recipe used (tools: python `cryptography`, `cabextract`, `/home/e3xp0/devkitxenon/bin/xenon-objdump`):

1. File key: `AES-ECB(decrypt, key=16×0x00)` over the 16 bytes at `sec_off+0x150`.
2. Body: `AES-CBC(decrypt, key=filekey, iv=0)` over the data from `pe_off` to EOF.
3. Walk the compression blocks (block list head at `fileformat_opt+12`; each block is
   `[u32 next_size][20-byte hash]` then `[u16 chunk_size][chunk]*` until a 0 size),
   concatenate chunks, wrap as a single-file LZX CAB (uncompressed size = XEX
   `image_size`, here `0x54000`), `cabextract` it.
4. Disassemble: `xenon-objdump -D -EB -b binary -m powerpc:common64 --adjust-vma=0x91175000 <text.bin>`.

The decompressed image is a normal little-endian PE (`.text` RVA `0x5000`, size
`0x1a1a4`; load VA `0x91170000`, JX:header). Imports: **xam.xex** (sockets via
`NetDll_*` ordinals, `XamShowMessageBoxUI` ord 714, `XNotifyQueueUICustom` ord 656,
`XamGetCurrentTitleId` ord 463) and **xboxkrnl.exe** (JX:import libs `xam.xex`,
`xboxkrnl.exe`). So JRPC does its own BSD-socket networking through xam — it is *not*
layered on XBDM. **[1]** (the whole TCP picture rests on this one binary.)

### 0.4 Which `JRPC.dll` is which — resolved

- Every `JRPC.dll` / `JRPC.cs` in the tool folders sends commands with
  `console.SendTextCommand(connectionId, Command, out reply)` and reads memory with
  `console.DebugTarget.GetMemory(...)` (JC:816, JC:574). `SendTextCommand` /
  `DebugTarget` are **XBDM / xdevkit COM** calls on port 730. There is **no**
  `System.Net.Sockets` / `TcpClient` / `connect(...,1409)` anywhere in these clients.
  → These clients are **JRPC2 clients** (they drive the `JRPC2.xex` XBDM plugin).
- `JRPC.xex` (JX), by contrast, calls `NetDll_socket/bind/listen/accept/recv/send`
  and runs its own `accept` loop on **TCP 1409** (section 1). → It is the **JRPC**
  (TCP) server, with no client in these references.
- **Consequence:** the JRPC (TCP) *wire text* below is taken from JC (the command
  strings are byte-for-byte what the TCP server's parser expects — confirmed against
  JX's dispatcher and format strings), while all JRPC (TCP) *transport* facts come
  from JX alone. No reference exercises the TCP path end to end, so the whole of
  sections 1 and 9 is effectively single-source.

### 0.5 Labels vs. code — contradictions flagged

The user's folder labels mark these as JRPC tools: `aio_tool_devexpress-xdevkit-xrpc-jrpc2`,
`extra_shit-xdevkit-jrpc2`, `The_Cipher_Mod_Tool-xdevkit-jrpc2`,
`one_tool-xdevkit-jrpc2`, `XRPC_Tool-xdevkit-xrpc-jrpc2`,
`bo2_shit_1.7.3-xdevkit-xrpc-jrpc-jrpc2-rpc`, and the example
`xdevkit-xrpc-jrpc2-example`. **Contradiction to flag:** the `-jrpc` label is accurate
that the tool *bundles* `JRPC.dll`, but that DLL is a **JRPC2 (XBDM) client**, not a
JRPC-TCP client (0.4). Only `bo2_shit...` actually ships the JRPC **server**
(`Plugins/JRPC.xex` + `JRPC.ini`) that this document describes. So "uses JRPC" in the
labels means "can drive a console that has JRPC/JRPC2 installed", not "opens a TCP
socket to 1409".

## 1. Transport (JX only — [1] throughout)

### 1.1 Socket and port

- IPv4 TCP, **port 1409** (`0x0581`). Server does `socket(AF_INET=2, SOCK_STREAM=1,
  IPPROTO_TCP=6)` (JX @0x91175408), `setsockopt(level=0xFFFF/SOL_SOCKET, optval=1)`
  (JX @0x91175068), then `bind` to `sin_family=2`, `sin_port=0x0581`,
  `sin_addr=INADDR_ANY` (JX @0x91175444: `li r10,1409; sth r10,82(r1)`), then
  `listen(backlog=0x7FFFFFFF)` (JX @0x91175474). **[inf]** port = 1409 decimal.
  (The author's AIM handle embedded in the sister plugin is `XxjAmestxX1409`,
  JRPC2 strings — consistent, not proof.)
- The server starts a dedicated accept thread (`ExCreateThread`-style spawn at
  JX @0x91175588) that loops `accept` and keeps a fixed table of **up to 8**
  simultaneous client sockets (slot array length 8, JX @0x9117a660..0x9117a680).
  A 9th connection waits until a slot frees. **[inf]**

### 1.2 Connection lifetime and banner

| Step | Bytes on the wire | Source |
|---|---|---|
| Server → client, immediately on `accept` | `JRPC2 connected\r\n` (17 bytes, sent with `send(fd, buf, 17, 0)`) | JX @0x91175b68 / rdata `0x91170b68` |
| Client → server | one command line per request, LF-terminated (section 2) | JX recv loop @0x9117a2a0 |
| Server → client | one reply line per command, CRLF-terminated (section 6) | JX format strings @0x91170a60–b60 |
| Client → server, to close | the literal line `Bye` | JX compares each line to `Bye\r\n` @rdata `0x91170b7c` |
| Server | on `Bye`, closes that fd and frees the slot | JX @0x9117a524 |

- There is **no** XBDM-style numeric status greeting (`201- connected`) and **no**
  `200-`/`4xx-` status prefix on replies. The reply is the bare formatted value
  (section 6). This is the sharpest difference from JRPC2/XBDM.
- The connection is persistent: many commands may be sent on one socket; the server
  reads continuously until `Bye` or socket close.

### 1.3 Framing and buffers

- **Request framing:** the server appends each `recv` into an 8500-byte per-connection
  buffer and scans for `\n` (0x0A) (JX @0x9117a318: `cmplwi r10,10`). Everything up to
  and including the first `\n` is one command; the remainder is kept for the next read.
  So a command line is terminated by **LF**; a preceding `\r` (the client sends CRLF)
  is tolerated and stripped as leading/trailing whitespace by the field parser. **[inf]**
- Per-connection receive buffer is **8500 bytes** (JX @0x9117a274 `li r5,8500`); a
  command longer than that before a `\n` overflows the scan and is dropped. **[inf]**
  **Contract:** client keeps one command line ≤ 8191 bytes (the server's own
  `as=`/length fields are clamped to `0x7FFFFFFF`→-1 on overflow, JX @0x91178efc).
- **Reply framing:** one line, CRLF-terminated (section 6). The client reads until
  `\r\n`.
- A string/byte-array argument is hex-encoded inline in the command line (section 3),
  so there is **no** separate binary transfer channel and **no** `buf_addr=` handshake
  on the TCP server (the `buf_addr=%p` scratch-buffer dance exists only in the JRPC2
  XBDM build and in JC's reply loop, JC:193–197). The TCP server allocates its own
  scratch buffer with `malloc` and frees it after the call. **[inf]**

### 1.4 Endianness

- The console is **big-endian PPC**. On the wire everything is **ASCII text**
  (hex/decimal digits), so byte order only matters for the hex blobs of array/struct
  args and for memory the call touches. The JRPC2 client byte-reverses multi-byte
  scalars it reads back from memory (JC:728,749,769 `ReverseBytes`) because
  `GetMemory` returns raw big-endian bytes; a TCP JRPC client doing its own reads must
  do the same. **[inf]**

### 1.5 Timeouts

- No socket timeout is set by the server; `recv` blocks. JC sets the XBDM
  conversation/connect timeout to `0x3d0900` (4,000,000 ms ≈ 66 min) while a call runs
  and back to `0x7d0`/`0x1388` (2 s / 5 s) after (JC:112,198–199) — an XBDM-client
  concern, not a TCP wire fact. **Contract:** TCP client uses a long read timeout
  (a called function may run arbitrarily long) and a short connect timeout.

## 2. Command line format

All requests are one line of this shape (JC:113, parsed by JX @0x91178e08):

```
consolefeatures ver=2 type=<T>[ system][ module="<name>" ord=<ord>] as=<arraysize> params="A\<addrHex>\A\<argc>\<arg>*"\n
```

| Field | Meaning | Width/type | Source |
|---|---|---|---|
| `consolefeatures ` | fixed verb | ASCII | JC:113 |
| `ver=2` | protocol version (always 2) | decimal | JC:19,113 |
| `type=<T>` | call return-type tag **or** system opcode (section 4) | decimal 0–18 | JC:113 |
| ` system` | present ⇒ run on a system thread; absent ⇒ title thread | flag | JC:95,113 |
| ` module="<name>" ord=<n>` | present ⇒ resolve entry point by module export ordinal instead of raw address | quoted string + decimal | JC:113 |
| `as=<n>` | expected return **array** element count (0 for scalar) | decimal | JC:113 |
| `params="…"` | argument block, see below | quoted | JC:113 |

Argument block grammar (inside the quotes):

```
A \ <addrHex> \ A \ <argc> \   (arg)*        where arg = <tag> (\|/) <value> \
```

- `addrHex` = target function address as **uppercase hex, no `0x`** (JC:113
  `Address.ToString("X")`). For a by-ordinal call the address field is `0`.
- `A\…\A\…\` — the two literal `A` tokens delimit the address and arg-count sections
  (JX @0x91178864 scans params for `'A'`=0x41, then digits). `argc` is decimal.
- Each argument is `tag` + separator + value + `\`. Separator is `\` for scalars and
  `/` for length-prefixed blobs (string/array); e.g. a string arg is
  `7/<charcount>\<hexbytes>\` and a bool is `1/<0|1>\` (JC:131,149). The server's
  parser keys only off the first character (the tag) and the `/`-vs-`\` does not
  change decoding. **[inf]**
- Max **37** arguments (`0x25`); the client throws above that and the server's frame
  is sized for it (JC:114–116). **[1]**

## 3. Argument marshalling

Tag is the first char of each `params` argument. The server decodes and places values
into a PPC call frame: integer/pointer args into GPRs **r3–r10** then the stack, float
args into FPRs **f1–f8**, before `bctrl` to the target (JX @0x91179440 builds the
frame, JX @0x91179554/0x91179668/… are 9 call stubs selected by float-arg count 0–8).

| Tag | C# source type (JC) | On-wire form | Server decode | Passed to target as | Source |
|---|---|---|---|---|---|
| `1` Int | `int`, `uint`, `bool` | `1\<dec>\` (int/uint), `1/<0\|1>\` (bool) | `sscanf %i` | 32-bit GPR (sign-/zero-extended) | JC:122–135; JX @0x91178864 tag `'1'`(0x31)/`'4'`(0x34) |
| `2` String | `string` | `2/<charcount>\<asciiHexPerChar>\` | hex-decode to bytes, `malloc` scratch, copy, NUL-terminate | pointer (GPR) to scratch buffer | JC:146–150; JX @0x9117893c tag `'2'`(0x32)/`'7'`(0x37) |
| `3` Float | `float`, `double` | `3\<decimalText>\` | `sscanf` float | FPR f1–f8 | JC:151–160; JX @0x91178a24 tag `'3'`(0x33) |
| `4` Byte | `byte` | `4\<dec>\` | `sscanf %i` | 32-bit GPR | JC:134; JX tag `'4'` |
| `5` IntArray | `int[]`, `uint[]` | serialized **as tag `7`** (byte blob, big-endian 4-byte elems) | as byte array | pointer to scratch | JC:137–145 |
| `6` FloatArray | `float[]` | serialized **as tag `7`** (byte blob, 4-byte BE floats) | as byte array | pointer to scratch | JC:161–174 |
| `7` ByteArray | `byte[]` (and int/float arrays) | `7/<bytecount>\<hexBytes>\` | hex-decode, `malloc`, copy | pointer to scratch | JC:175–183; JX @0x9117893c |
| `8` Uint64 | fallthrough for `long`,`ulong`,enums,etc | `8\<dec>\` | `sscanf %lli` | 64-bit GPR | JC:186–188; JX @0x91178a8c tag `'8'`(0x38) |

- Only tags **1,2,3,4,7,8** ever appear on the wire; `5`,`6`,`9` are client-side names
  that get re-encoded as `7` (int/float arrays → byte blob) before sending (JC:137–174).
- Array/float-array elements are written **big-endian** into the blob (`Array.Reverse`
  per element, JC:168). **[1]**
- Float args: the server can also store a float into a GPR slot converted to int via
  `fctidz` when the call stub needs it in a GPR (JX @0x911794d4). High-level ABI
  behaviour; mark **[inf]**.
- Scratch buffers for string/blob args are **local to the server** (malloc/free around
  the call); the caller does not supply or learn an address. **[inf]**

**Contract (UpdClient, `jrpc::encodeArgument` / `buildCommand`):** the choices the table leaves
implicit. All of them are unverified on a console.

- *Integers.* int32, uint32 and bool go through tag `1`, a byte through tag `4`, as decimal. The server
  reads `1` and `4` with `sscanf %i` into an int, so a uint32 above `INT32_MAX` is sent as the equal
  negative int32 (a decimal above the range could be clamped by the C runtime). A bool is `1/0\` or
  `1/1\`.
- *64-bit.* int64 and uint64 go through tag `8` as decimal (read with `sscanf %lli`); a uint64 above
  `INT64_MAX` is sent as the equal negative int64, for the same reason.
- *Floats.* float and double go through tag `3` as `%.9g` and `%.17g` text in the C locale, never the process
  locale. NaN and infinities are refused (`InvalidArgument`) because the text would not parse back. Whether
  the server keeps a `float` or a `double` is unknown; both are sent with enough digits for either.
- *Strings.* Tag `2`, written `2/<byte count>\<hex of the bytes>\`; the count is bytes, which equals the
  document's "charcount" for ASCII. An embedded NUL or text that is not valid UTF-8 is refused, because it
  becomes a C string on the console. The empty string is `2/0\\`; whether the server accepts it is unknown.
- *Blobs and arrays.* Byte, int32 and float arrays all go through tag `7`, `7/<byte count>\<hex>\`, with
  4-byte big-endian elements for ints and floats; tags `5`, `6` and `9` never go on the wire. An empty blob
  is encodable, with the same caveat as the empty string.
- *Separators.* `\` after the tag for scalars, `/` for strings and blobs, as in the examples of section 7.
  The server's parser is believed to ignore the difference, so the mock accepts either.
- *Limits, checked before anything is sent.* At most 37 arguments; a command line of at most 8191 bytes
  without its terminator (a payload that cannot fit is refused before it is hex-encoded); `as` from 1 to 8
  for the array return kinds and 0 for the others. A 64-bit array return (`type=9`) is refused as
  `Unsupported` (open question 2).
- *Targets.* A call by address with address `0` is refused, because `0` is the marker of a by-ordinal call.
  A module name must be non-empty printable ASCII without space, quote or backslash. Opcode 18 carries its
  target address in the address field; every other opcode sends `0`.
- *The five arguments of opcode 18* are the value, then a flag and a value for each guard:
  `1\<value>\1\<useIf>\1\<ifValue>\1\<useTitle>\1\<titleId>\`, all through tag `1` as above. A guard that is
  not wanted sends `0` and `0`.

## 4. `type=` values — the command table

`type` is overloaded: `0–8` select a **function call** and its return decoding;
`9–18` are **built-in system opcodes** (no user function address). The server
dispatches `type<=8` to the generic call path, else to the opcode handlers
(JX @0x91178f44 `cmpwi r10,8; ble …`).

### 4.1 Function-call return tags (type 0–8)

For these the server resolves the entry point (raw `addr`, or `module`+`ord`), builds
the arg frame, `bctrl`s, then formats the return per the tag.

| type | Meaning | Return read as | Reply line format | Client method | Source |
|---|---|---|---|---|---|
| 0 | Void | — (GPR r3 ignored) | `%X\r\n` of r3 (or `S_OK\r\n` for some ops) | `CallVoid` | JC:411; JX @0x9117a044, fmt `0x91170b5c` |
| 1 | Int | r3 → `uint`/`int`/`short`/`ushort` | `%X\r\n` | `Call<int>` etc. | JC:201–219; JX fmt `0x91170b5c` |
| 2 | String | r3 = `char*`, copied out | `%s\r\n` | `CallString` | JC:220–230; JX @0x9117a05c, fmt `0x91170b48` |
| 3 | Float | f1 | `%f\r\n` | `Call<float>`/`<double>` | JC:231–238; JX @0x9117a06c, fmt `0x91170ad4` |
| 4 | Byte | r3 low 8 bits | `%X\r\n`/`%02X` | `Call<byte>` | JC:240–250; JX @0x9117a08c |
| 5 | IntArray | `as` ints from r3 ptr | `%i,%i,…,%i;\r\n` (≤8 shown) | `CallArray<int>` | JC:261–281; JX @0x9117a09c, fmt `0x91170a78` |
| 6 | FloatArray | `as` floats | `%f,%f,…,%f;\r\n` | `CallArray<float>` | JC:282–302; JX @0x9117a0d8, fmt `0x91170a94` |
| 7 | ByteArray | `as` bytes | `%X,%X,…,%X;\r\n` | `CallArray<byte>` | JC:303–323; JX @0x9117a144, fmt `0x91170ab0` |
| 8 | Int64 | r3 (64-bit) | `%llX\r\n` | `Call<long>`/`<ulong>` | JC:251–259; JX @0x9117a180, fmt `0x91170a70` |

- The array formats emit **up to 8** elements then `;` (the format literally has 8
  `%` specifiers, JX `0x91170a78`). `as` larger than 8 is an open question (section 10).
- `type=9` collides in naming with the client's `Uint64Array` tag, but on the TCP
  server `type=9` is always **ResolveFunction** (4.2) because the opcode check beats
  the generic path. A 64-bit array return over raw TCP is therefore unreachable; see
  section 10. **[inf]**

### 4.2 System opcodes (type 9–18)

| type | Name | Extra request fields | Reply | Client method | Server impl | Source |
|---|---|---|---|---|---|---|
| 9 | ResolveFunction | `params` carries `2/…\<moduleHex>\` + `1\<ordinal>\`, `addr=0` | `%X\r\n` (address) | `ResolveFunction(module,ord)` | resolves export; on failure `error=Could not resolve function address…` (JRPC2 build) | JC:786–792; JX @0x91179064 |
| 10 | GetCPUKey | none (`A\0\A\0\`) | `%X%X\r\n` (hi then lo 32 bits) | `GetCPUKey` | reads CPU key halves | JC:552–558; JX @0x91179094 |
| 11 | ShutDownConsole | none | (connection ends) | `ShutDownConsole` | `HalReturnToFirmware(6)` (krnl ord 41) | JC:849–852; JX @0x911791a4 `li r3,6` |
| 12 | XNotify | `2/<len>\<textHex>\` + `1\<type>\` | — | `XNotify(text,type)` | `XNotifyQueueUICustom` (xam ord 656) | JC:1105–1112; JX @0x911791c8 |
| 13 | GetKernelVersion | none | `%d\r\n` | `GetKernalVersion` | reads kernel version halfword | JC:561–567; JX @0x91179234, fmt `0x91170b38` |
| 14 | SetLeds | `1\<tl>\1\<tr>\1\<bl>\1\<br>\` (4 ints) | — | `SetLeds` | `HalWriteSMBusByte`-style LED set (krnl ord 40) | JC:835–840; JX @0x91179268 |
| 15 | GetTemperature | `1\<which>\` (0=CPU,1=GPU,2=EDRAM,3=MB) | `%X\r\n` | `GetTemperature` | reads SMC temp table | JC:580–586; JX @0x911792c8 |
| 16 | GetCurrentTitleId | none | `%X\r\n` | `XamGetCurrentTitleId` | `XamGetCurrentTitleId` (xam ord 463) | JC:1093–1096; JX @0x9117934c |
| 17 | ConsoleType | none | one of `Xenon`/`Zephyr`/`Falcon`/`Jasper`/`Trinity`/`Corona`/`Unknown` + `\r\n` | `ConsoleType` | reads motherboard id, maps to name | JC:459–463; JX @0x91179378, strings `0x91170adc`+ |
| 18 | constantMemorySet | `params`: 5 ints `A\<addr>\A\5\ 1\<value>\ 1\<useIf>\ 1\<ifVal>\ 1\<useTitle>\ 1\<titleId>\` | — | `constantMemorySet*` | registers a background "keep writing this value" task | JC:477–483; JX @0x91178f60 |
| 19 | (byte-blob op) | builds a byte string from params | — | *(no client method in JC)* | internal; likely a raw `setmem`-style write | JX @0x91178f90 **[inf]** |

- Memory **read** (`GetMemory`) and **write** (`SetMemory`) in JC go over XBDM
  (`DebugTarget.GetMemory`/`SetMemory`, JC:574,846), **not** over the JRPC command
  line — so a pure-TCP JRPC client cannot use them and must read/write memory by
  calling a resolved kernel export (e.g. resolve + `Call` of a copy routine) or by
  opcode 18. This is a real gap for the TCP path; section 10. **[inf]**

## 5. Function / entry-point resolution

- **By address:** `addr` hex in `params` is the raw effective address; server
  `bctrl`s it directly.
- **By ordinal:** `module="name.xex" ord=N` → server resolves export N of that module
  and calls it. JC always sends the module name in plain ASCII inside the quotes
  (JC:113), *and* repeats it hex-encoded as a `2`-tag arg for opcode 9. **[inf]**
- **Thread context:** ` system` flag runs the call on a system thread; absent = title
  thread. JC exposes this via `ThreadType` (JC:94–103). The server creates or selects a
  thread accordingly. **[inf]**

## 6. Reply / error table

| Situation | Reply line | Source |
|---|---|---|
| Scalar int/uint/byte return | `<hex>\r\n` (`%X`) | JX fmt `0x91170b5c` |
| Int64 return | `<hex>\r\n` (`%llX`) | JX fmt `0x91170a70` |
| Float return | `<decimal>\r\n` (`%f`) | JX fmt `0x91170ad4` |
| String return | `<text>\r\n` (`%s`) | JX fmt `0x91170b48` |
| Int / Float / Byte **array** | `v,v,…,v;\r\n` (comma list, `;` terminator, max 8 elems in the format) | JX fmt `0x91170a78/a94/ab0` |
| Void / success | `<hex>\r\n` or `S_OK\r\n` | JX `0x91170b40` |
| Kernel version | `<dec>\r\n` (`%d`) | JX fmt `0x91170b38` |
| Console type | `Xenon`/`Zephyr`/`Falcon`/`Jasper`/`Trinity`/`Corona`/`Unknown` + `\r\n` | JX `0x91170adc…b20` |
| Function address not resolvable | *(JRPC2 build)* `error=Could not resolve function address, params = %s, %d` | JX2 strings |
| xbdm not the right version *(JRPC2 build only)* | `Current xbdm.xex is unsupported!…` | JX2 strings |

- The TCP server (JX) emits **no status-code prefix**. JC's habit of
  `str.Substring(str.find(" ") + 1)` (JC:203 etc.) strips the XBDM `200- ` prefix and
  is meaningless against a bare TCP reply — a TCP client parses the value from column 0.
  **Contract:** TCP client reads one CRLF line and parses it directly; treats a line
  beginning `error=` as a failure and a line containing `DEBUG` as "JRPC not installed"
  (mirrors JC:817–822).

**Contract (UpdClient, `jrpc::parse*Reply`, `JrpcClient`):** how a reply line is read. The server's
wording of failures on the TCP build is not known, and nothing in this section has met a console.

- *Order.* A line starting `error=` is checked first, then a line containing `DEBUG` anywhere in it, then
  the line is parsed by the `type` that was sent. A string result that contains `DEBUG` is therefore taken
  for "JRPC not installed".
- *Terminator.* CR LF; a bare LF is tolerated, and exactly one trailing CR is stripped. A lone CR is not a
  terminator, so such a reply never completes. A reply is read up to 64 KiB by default.
- *Numbers.* `%X` is 1 to 8 hex digits (types 1, 9, 15, 16), `%llX` 1 to 16 (type 8), without sign, `0x` or
  white space, unpadded or padded. A byte (type 4) takes 1 to 8 digits and keeps the low 8 bits, because the
  server may or may not mask. A void call accepts `S_OK` or 1 to 16 hex digits. A float is read with
  `from_chars` from the `%f` text, which has six decimals, so a tiny float reads as 0; `inf` and `nan`
  spellings are accepted. The kernel version (`%d`) is a signed decimal that fits an int32 and a negative
  one is refused. The console type is exactly one of the seven names, case sensitive.
- *Strings.* The whole line; a NUL, CR or LF inside is a protocol error.
- *Arrays.* `v,v,…,v;` with nothing after the `;` and exactly `as` values (1 to 8). Any other count, a
  missing `;`, an empty list or a trailing comma is a protocol error. Whether the server really prints
  `as` values, fewer, or loops past 8 is open question 1.
- *Wrong shape.* A reply that does not parse for the `type` sent closes the connection (the position in
  the stream is unknown), as do a timeout, an over-long line, a `DEBUG` line, bytes that arrive with a
  reply or before any command was sent, and a drop. `error=` keeps the connection.
- *`error=` text.* Becomes `ErrorCode::Io` with the text at the end of the message; the client tells
  `Could not resolve function address…`, `Version mismatch` and `The paramaters were not found` (the typo is
  the server's) from other text. These three are the JRPC2 build's wording and are unverified for the
  TCP server.

## 7. Worked byte-level examples (inferred from JC; verified against JX's parser)

All lines are ASCII; `·` marks the single trailing `\n` the server scans for. The
client sends CRLF; the server tolerates the `\r`.

**7.1 Call `void f(void)` at `0x82000000`, title thread** (`CallVoid(0x82000000)`):
```
consolefeatures ver=2 type=0 as=0 params="A\82000000\A\0\"·
```
Reply: `0\r\n` (or `S_OK\r\n`).

**7.2 Call `int g(int a=5, int b=0x10)` at `0x82010000`** (`Call<int>(0x82010000,5,0x10)`):
```
consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\"·
```
(`0x10`=16 decimal; ints are decimal on the wire.) Reply e.g. `2A\r\n` (= 42).

**7.3 Call by ordinal, system thread, `ulong h()` in `xam.xex` ord 0x1B4**
(`Call<ulong>(System,"xam.xex",0x1B4)`):
```
consolefeatures ver=2 type=8 system module="xam.xex" ord=436 as=0 params="A\0\A\0\"·
```
Reply e.g. `0000000248173A00\r\n`.

**7.4 Call `void notify(char* s)` with string arg "hi"** (string `"hi"` = chars `68 69`):
```
consolefeatures ver=2 type=0 as=0 params="A\82020000\A\1\2/2\6869\"·
```
Server hex-decodes `6869`→`"hi"`, mallocs, passes the pointer.

**7.5 XNotify("Hello", type 0)** (opcode 12; "Hello" = `48656C6C6F`):
```
consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\0\"·
```
No reply body.

**7.6 GetCPUKey** (opcode 10):
```
consolefeatures ver=2 type=10 params="A\0\A\0\"·
```
Reply e.g. `A1B2C3D4E5F60718\r\n` (two `%X` halves concatenated).

## 8. Usage patterns seen in real tools

- The labelled tools never open 1409 themselves; they call the JC methods
  (`console.Call<…>`, `XNotify`, `SetLeds`, …) which go over XBDM to **JRPC2**
  (section 0.4). Representative call sites: `aio_tool_devexpress-xdevkit-xrpc-jrpc2/MainForm.cs`
  (`...Connect(out …, "default")` then feature calls), `The_Cipher_Mod_Tool.../PCTool.cs`.
  They are evidence for the *command vocabulary*, not for TCP transport.
- `bo2_shit…/Plugins/JRPC.ini` (JI) shows how the TCP server is deployed: it is loaded
  as a Dashlaunch plugin from `Hdd:\JRPC.xex` alongside `XRPC.xex`, and has a config
  block `"Settings"{ "KV Stealer Protection" }` and `"Plugins"{ "plugin1"…"pluginN" }`.
  The server reads `Hdd:\JRPC.ini` at startup (JX string `\Device\Harddisk0\Partition1\JRPC.ini`).

## 9. How a C++ client implements JRPC (TCP)

1. `connect()` a TCP socket to `<consoleIP>:1409`.
2. Read one line; expect `JRPC2 connected\r\n`. (Do **not** treat it as a version — it
   is just the banner.)
3. Per call: format the ASCII command of section 2, end with `\n` (CRLF is fine),
   `send` it. Keep each line ≤ ~8 KB.
4. Read one CRLF-terminated reply line; parse per section 6 (no status prefix). Lines
   starting `error=` are failures.
5. Reuse the socket for further calls; send `Bye` (or just close) to disconnect.
6. For arguments: ints/bytes as decimal, 64-bit as decimal with tag `8`, floats as
   decimal text with tag `3`, strings/arrays hex-encoded with tag `2`/`7` and a
   `/<count>\` length prefix. Byte-reverse multi-byte scalars you later read from
   console memory (big-endian).
7. Memory read/write is **not** a JRPC command on the TCP path — resolve and call a
   console copy/poke routine, or use opcode 18, or pair JRPC with an XBDM connection.

**Contract (UpdClient, additions):** how the client handles what steps 1 to 7 leave implicit.

- *Banner.* The line must be exactly `JRPC2 connected`. A `DEBUG` line means JRPC is not installed
  (`Unsupported`); any other line is a protocol error; no banner is a timeout. A 9th connection waits for
  a slot without a banner (section 1.1), so the timeout message hints at the limit of 8.
- *One command in flight (D3).* The client sends a command only after it read the previous reply. The
  one exception is the barrier below. A call that timed out may still be running on the console, so that
  connection is not reused.
- *Timeouts.* The first byte of a reply is awaited for at most `callTimeout` (default 60 s; 0 means no
  bound), because a called function may run long; the whole call is also bounded by it, so a reply that
  trickles cannot stretch it; once the reply has begun, `idleTimeout` (10 s) bounds the gap between bytes.
- *Bye.* Sent once, as `Bye\r\n`, only on a healthy idle connection; never after a failure or a cancel.
  The server's answer to it is nothing, so the client does not wait.
- *Opcodes that may not answer (D6).* The document does not say whether opcodes 12 (XNotify), 14 (SetLeds)
  and 18 (constantMemorySet) reply. With `silentOpBarrier` (the default) the client sends opcode 17
  (ConsoleType, read-only) right behind each and reads lines until one of the seven console names. At most
  one line may come before it, and that line is the opcode's own answer: `S_OK` or hex is accepted, `error=`
  fails the call after the barrier was read. Two lines, an unrecognised line or a `DEBUG` line close the
  connection. The sentinel is a console name and not a number, because an opcode that answers `0` or any
  digits would be indistinguishable from a decimal answer. The server must therefore accept two lines in one
  `recv` (it keeps the rest of the buffer, section 1.3); that is unverified. With the option off, the client
  returns at once and a reply that does arrive is read as the answer to the next command. Opcode 11
  (ShutDownConsole) is sent and the connection is closed without waiting and without `Bye`.
- *CPU key (D8).* The reply is two unpadded `%X` halves run together, so it can be split only when all
  digits are present. The client accepts exactly 16 hex digits (the document's own example, 8 bytes) or 32
  (a 16-byte key); any other length is a protocol error with the raw text in the message. When it is hex
  alone, the connection stays open, since that is the ambiguity of open question 6 and not a sign of lost
  framing. Which length a console prints is not known.
- *Delivery.* Every command, calls and opcodes alike, is treated as one that may change the console; the
  client records how far the last one got (not sent, partly sent, sent, answered).
- *Not exposed.* Opcode 19, a 64-bit array return, and memory access (step 7); the CLI keeps `mem` and `file`
  XBDM-only.

**Contract (UpdClient):** the client speaks exactly section 2, waits for the banner,
uses `\r\n` line endings both ways, parses bare replies, and never assumes a `200-`
prefix. The mock server sends `JRPC2 connected\r\n` on accept, scans for `\n`, echoes
the section-6 format matching the `type`, closes on `Bye`, and supports at least the
scalar return tags and opcodes 10/13/16/17.

## 10. Open questions (settle on hardware / with more sources)

1. **Array returns > 8 elements** — the reply format has exactly 8 `%` slots. Does the
   server loop the format, truncate at 8, or send `as` elements some other way? [none]
2. **type=9 collision** — a 64-bit *array* return (client `Uint64Array`) can't reach a
   generic call because `type=9` is ResolveFunction on the server. Is `CallArray<ulong>`
   simply unsupported over TCP, or does the server special-case it? [none]
3. **Exact `setsockopt`** — level `0xFFFF` with optval 1: `SO_REUSEADDR`? (assumed). [inf]
4. **Opcode 19** payload and purpose (byte-blob builder) — no client calls it here;
   likely a raw memory write. Needs a client sample or deeper trace. [inf]
5. **`buf_addr=` on TCP** — confirmed absent in JX's string table; confirm the server
   never needs a client-visible scratch address for very large blobs. [inf]
6. **GetCPUKey reply** — two `%X` with no separator; confirm the client splits on
   fixed 8+8 hex, not a delimiter. [inf]
7. **Line length ceiling** — 8500-byte recv buffer vs. the 37-arg limit; find the real
   max command size the server accepts before dropping. [none]
8. **Thread-context semantics** — does ` system` create a new system thread per call or
   reuse one, and what are the lifetime/blocking guarantees? [none]
9. **Multiple-client fairness** — behaviour at the 9th concurrent connection, and
   whether replies can interleave across sockets. [none]
