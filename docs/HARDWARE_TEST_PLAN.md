# XBDM hardware test plan

UpdClient's XBDM support has only ever talked to `tests/support/xbdm_mock_server.cpp`, a mock written
from [XBDM_PROTOCOL.md](XBDM_PROTOCOL.md), which in turn was written from third-party clients. Nothing
has been run against a console or an emulator. This plan settles the open questions of section 6 of the
protocol document, and the ones the client-mock integration added (N1 to N8, also listed there as
questions 32 to 39), on real targets. Each entry gives the commands to run, what UpdClient assumes
today, what to report back, and a risk level.

## 1. Before you start

### 1.1 Targets

Run every entry on as many of these as you have; one report per target.

| ID | Target | Notes |
| --- | --- | --- |
| DK-X | Devkit running XDKBuild | Use the debug IP address, not the title address (spec 1.1). |
| DK-R | Devkit running RGLoader | Same. |
| RT-G | Retail console running Glitch2 with an XBDM plugin | Name the plugin and its version in the report. |
| EM-XE | Xenia | Only if it offers XBDM on TCP 730. Run 3.0 first; if the connection is refused, write "no XBDM" and stop. |
| EM-XN | Xenon emulator | Same. |

### 1.2 Risk levels and safety rules

| Risk | Meaning |
| --- | --- |
| Low | Reads, or writes inside the scratch folder only. |
| Medium | Reboots or restarts a title (unsaved progress is lost), changes the clock, fills a scratch drive, or reads memory. |
| **HIGH** | Writes memory or reads memory known to freeze consoles. Can crash or hang the console; a hang needs a power cycle. |

Rules, for every entry:

- **Every write goes to a scratch folder on a scratch drive.** The plan uses `HDD:\updclient-test`.
  If the hard drive holds anything you would miss, use a USB stick formatted by the console instead
  and replace `HDD:` by its drive name (`USB0:` on most consoles; `updclient xbdm drives` lists them).
  Never write to `FLASH:`, `Y:`, the dashboard or system partitions, or to any folder you did not create
  for this plan.
- Entry Q7 fills a drive. **Use a dedicated, small, empty USB stick for it and nothing else.**
- Close debuggers (Neighborhood, Visual Studio, other XBDM tools) before you start; several entries
  open many connections or reboot.
- **HIGH entries are optional.** Run them only on a devkit you can power-cycle, with nothing unsaved,
  and read their warning first.
- Destructive commands (`rm`, `launch`, `reboot`, `raw`, `mem poke`, `power ...`) ask for confirmation;
  the commands below pass `--yes`. Read each command before you run it.

### 1.3 Setup

Build UpdClient (README, "Build"). Then, in a POSIX shell:

```sh
export T=xbdm://192.168.1.50        # the console; port 730 is the default
export NAME=dk-xdk                  # short name of the target, used for file names
u() { updclient --target "$T" --trace "trace-$NAME.txt" "$@"; }
P() { python3 /path/to/UpdClient/docs/xbdm_probe.py "${T#xbdm://}" "$@"; }
```

PowerShell:

```powershell
$env:T = "xbdm://192.168.1.50"; $env:NAME = "dk-xdk"
function u { updclient --target $env:T --trace "trace-$env:NAME.txt" @args }
function P { python3 C:\path\to\UpdClient\docs\xbdm_probe.py $env:T.Replace("xbdm://", "") @args }
```

Console paths are `HDD:\folder\file`, or `/HDD/folder/file` for the CLI commands that take a path, which
needs no quoting. Lines for `xbdm raw` are sent exactly as typed; keep them in single quotes.

`docs/xbdm_probe.py` (Python 3, standard library only) does what UpdClient never does on purpose:
commands before the greeting, two commands in one segment, an upload cut short, many connections, an
idle connection, a notification channel, UDP name lookups. It prints every line it sends and receives.
It was checked against the mock server only.

Create the scratch folder and two small files once:

```sh
u file mkdir /HDD/updclient-test                       # Low
printf hello > five.txt
u file send five.txt /HDD/updclient-test/five.txt     # Low
u file send five.txt /HDD/updclient-test/a.txt
u file send five.txt /HDD/updclient-test/b.txt
```

### 1.4 Capturing a raw protocol trace

The global option `--trace FILE` appends to FILE, for every XBDM session:

```
# updclient 0.1.0 XBDM trace of xbdm://192.168.1.50:730, started 2026-10-07T10:00:00.123Z
# '>' command sent, '<' line received; binary data is shown by its size only
     0.404 < 201- connected
     0.608 > getfileattributes name="HDD:\updclient-test\five.txt"
     0.704 < 202- multiline response follows
     0.713 < sizehi=0x00000000 sizelo=0x00000005 createhi=... changelo=...
     0.730 < .
     0.751 > getfile name="HDD:\updclient-test\five.txt"
     0.813 < 203- binary response follows
     1.010 < [9 bytes of binary data]
     1.015 > bye
     1.063 < 200- bye
```

The first column is milliseconds since the connection was made. Every command line sent and every text
line received is written in full; nothing is redacted. Binary data (file contents in both directions,
`getmemex` blocks, frame buffers) is never written, only a line with its size. Note what the trace does
contain before you share it: the debug name, the console id, paths and file names, and `getmem` answers,
which are memory contents as hex text. Control characters are written as `\xNN`.

For timing or segmentation questions a packet capture helps too, but it contains file data, so capture
only while working with the scratch files:

```sh
sudo tcpdump -i any -w xbdm-$NAME.pcap host 192.168.1.50 and port 730
```

### 1.5 What to send back

One block per entry and target, plus `trace-<target>.txt` and the probe output:

```
Entry:    Q5
Target:   DK-X, kernel/XDK version ..., XBDM version if known (Q20, Q21)
Ran:      the commands, as typed
Observed: the fields the entry asks for, copied from the output or the trace
Notes:    anything that differed from "UpdClient assumes"
```

## 2. Smoke test

### 2.0 Connect (Low)

```sh
u info
u xbdm drives
u xbdm ls /HDD
```

UpdClient assumes: the greeting is `201- connected`; `dbgname`, `consoletype`, `getconsoleid`,
`xbeinfo running`, `getexecstate` and `altaddr` answer, or are refused with a 4xx and shown as
"(not answered)".
Report: the output; any line the trace shows that is not `2xx` or `4xx`. If `u info` fails with
`ConnectFailed`, check the address and that the target runs XBDM, then report the error.

## 3. Open questions of section 6

### Q1. What does `getfileattributes` return? (Low)

```sh
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test\five.txt"'
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test"'
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test\missing"'
u --yes xbdm raw 'getfileattributes name="HDD:\"'
u xbdm stat /HDD/updclient-test/five.txt
u file get /HDD/updclient-test/five.txt five-back.txt
```

UpdClient assumes: 200 with the fields in the text, or 202 with them on body lines; `sizehi sizelo
createhi createlo changehi changelo`, and the flag `directory` for a folder; any 4xx for a missing path.
`file get` calls `getfileattributes` before every `getfile` and uses the size as an upper bound (N2);
after a 407, or an answer without `sizehi`/`sizelo`, it lists the parent folder for the size instead.
Report: status line and every field line for each of the four; which flags appear; whether `stat` shows
5 bytes and `file get` succeeds.

### Q2. `dirlist` error codes, and paths without the trailing backslash (Low)

```sh
u --yes xbdm raw 'dirlist name="HDD:\updclient-test\missing\"'
u --yes xbdm raw 'dirlist name="HDD:\updclient-test\five.txt\"'
u --yes xbdm raw 'dirlist name="HDD:"'
u --yes xbdm raw 'dirlist name="HDD:\updclient-test"'
u xbdm ls /HDD/updclient-test
```

If you know a folder the console refuses (`u xbdm ls` fails on it with "access denied"), list it too.
UpdClient assumes: always sends one trailing backslash (`dirlist name="HDD:\updclient-test\"`, the last
`ls` above); a missing folder, a file and a protected folder each give some 4xx; "access denied" in the
text for a protected folder.
Report: the status line of each; whether the forms without the backslash list the same entries as `ls`.

### Q3. `dirlist` flags and field order (Low)

```sh
u --yes xbdm raw 'dirlist name="HDD:\"'
u --yes xbdm raw 'dirlist name="DEVKIT:\"'          # devkits; any other drive with system files
```

UpdClient assumes: fields in any order; `directory` the only flag it acts on; `readonly` and `hidden` are
read if present; unknown fields and flags are ignored.
Report: two or three raw entry lines per drive from the trace, and every flag or field name you see that
is not `name sizehi sizelo createhi createlo changehi changelo directory`.

### Q4. `drivefreespace`: 200 or 202, and which drives refuse it (Low)

```sh
u xbdm drives
u --yes xbdm raw 'drivefreespace name="HDD:\"'
```

Repeat the `raw` line for every drive `drives` lists.
UpdClient assumes: 200 or 202; the six `...hi`/`...lo` fields; a drive that refuses still appears in
`drives`, without sizes.
Report: status and field line per drive; the code and text of each refusal.

### Q5. Does `sendfile` replace an existing file, and does it need the parent folder? (Low)

```sh
P partial 'HDD:\updclient-test\five.txt' 3 3
u xbdm stat /HDD/updclient-test/five.txt
P partial 'HDD:\updclient-test\nofolder\x.txt' 3 3
u xbdm ls /HDD/updclient-test
```

`partial PATH LENGTH SENT` sends `sendfile` straight to PATH announcing LENGTH bytes and, after a 204,
SENT bytes of `0xA5`. With SENT equal to LENGTH it prints the console's answer; with fewer it closes the
connection at once.
UpdClient assumes: nothing about replacing. It uploads to `<name>.<8 hex>.part`, deletes the final name
if `getfileattributes` finds it, then renames; so this answer only decides whether that delete is needed.
If the rename fails after that delete, the upload is left as the `.part` file and the error names it.
A missing parent is refused before 204 (the mock uses 413).
Report: the line after the `sendfile` command for both; the size `stat` shows afterwards (3 means
replaced); whether `nofolder` was created.

### Q6. What is left after a connection drops during `sendfile`? (Low)

```sh
P partial 'HDD:\updclient-test\cut.bin' 100000 40000
u xbdm ls /HDD/updclient-test
u xbdm stat /HDD/updclient-test/cut.bin
head -c 20000000 /dev/urandom > big.bin
u file send big.bin /HDD/updclient-test/big.bin       # press Ctrl-C after the first progress line
u xbdm ls /HDD/updclient-test
u --yes xbdm rm /HDD/updclient-test/cut.bin
```

UpdClient assumes the worst: the bytes received so far stay under the name used, so it uploads under a
temporary name and deletes it on the next connection. After Ctrl-C the CLI reconnects and prints
"Deleted the unfinished upload ..." or names the file in a warning.
Report: whether `cut.bin` exists and its size (0, 40000, 100000?); how long after the drop `u xbdm ls`
works (at once, after a delay, only after a retry); the CLI's last stderr lines after Ctrl-C; whether
any `*.part` file is left.

### Q7. What does the console answer when the device fills up during `sendfile`? (Medium)

> **WARNING: fills a drive.** Only on a dedicated small USB stick that holds nothing else, never on the
> hard drive. Delete the file afterwards.

```sh
u xbdm drives                                          # note USB0's free bytes, F
u file mkdir /USB0/updclient-test
head -c $((F + 1048576)) /dev/urandom > toobig.bin
u file send toobig.bin /USB0/updclient-test/toobig.bin
u xbdm ls /USB0/updclient-test
u xbdm drives
```

UpdClient assumes: a 4xx before 204 (the mock: 415 when the length exceeds the free space), or a 4xx
after the data, after which it deletes its temporary file on the same connection.
Report: when the refusal came (before 204, after the data, or the connection closed), its code and text
(from the trace), what is left in the folder, the free space afterwards. Delete leftovers with
`u --yes xbdm rm`.

### Q8. Can `getfile` and `sendfile` handle 4 GiB or more? (Medium)

Only where a volume holds a file of 4 GiB or more, or has more than 4 GiB free (a devkit's `DEVKIT:`
for example; FATX files stay below 4 GiB).

```sh
u xbdm ls /DEVKIT                                      # find a file of 4294967296 bytes or more
P getfile-length 'DEVKIT:\path\to\that\file'
P partial 'DEVKIT:\updclient-test\over4g.bin' 0x100000001 0
u xbdm ls /DEVKIT/updclient-test
```

UpdClient assumes: `file get` of a file of 4 GiB or more is refused before `getfile` is sent
(`Unsupported`), because a console might announce the size modulo 2^32 and the download would look
complete. `sendfile` lengths above 4 GiB - 1 are refused unless `ClientOptions::maxUploadBytes` is raised.
Report: the four length bytes `getfile-length` prints for the big file; the answer to the 4 GiB + 1
`sendfile` (204 or a refusal); whether an empty `over4g.bin` was left (delete it).

### Q9. `rename` onto an existing name, and across drives (Low)

```sh
u --yes xbdm raw 'rename name="HDD:\updclient-test\a.txt" newname="HDD:\updclient-test\b.txt"'
u xbdm ls /HDD/updclient-test
u file mkdir /USB0/updclient-test                       # if a USB drive is present
u --yes xbdm raw 'rename name="HDD:\updclient-test\a.txt" newname="USB0:\updclient-test\a.txt"'
```

UpdClient assumes: `xbdm mv` checks with `getfileattributes` that the new name does not exist and refuses
moves between drives itself; the console is expected to refuse both (the mock: 410 and 409).
Report: both answers; whether `a.txt` and `b.txt` both still exist afterwards.

### Q10. Does `mkdir` create missing parent folders? (Low)

```sh
u --yes xbdm raw 'mkdir name="HDD:\updclient-test\p1\p2\p3"'
u xbdm ls /HDD/updclient-test
```

UpdClient assumes: one level per `mkdir`, and a missing parent is refused (the mock: 413).
Report: the answer; whether `p1` (and `p1\p2`) exist afterwards.

### Q11. Deleting a non-empty folder, a missing path, and a folder without `dir` (Low)

```sh
u file mkdir /HDD/updclient-test/full
u file send five.txt /HDD/updclient-test/full/x.txt
u --yes xbdm rm --dir /HDD/updclient-test/full
u --yes xbdm raw 'delete name="HDD:\updclient-test\full"'
u --yes xbdm rm /HDD/updclient-test/missing
```

UpdClient assumes: 411 for a non-empty folder (the mock's choice), some 4xx for a missing path, 414 for a
folder deleted without `dir` (spec 3.9).
Report: the three answers.

### Q12. Case, forward slashes, maximum path length (Low)

```sh
u xbdm stat /HDD/UPDCLIENT-TEST/FIVE.TXT
u --yes xbdm raw 'getfileattributes name="HDD:/updclient-test/five.txt"'
p='HDD:\updclient-test'
for i in $(seq 1 20); do
  p="$p\\abcdefghijklmnopqrstuvwxyz0123456789"
  printf '%s: ' "${#p}"; u --yes xbdm raw "mkdir name=\"$p\"" | head -1
done
```

UpdClient assumes: names compare case-insensitively (FATX); it never sends `/`; it refuses a command
line over 1024 bytes itself.
Report: whether the upper-case `stat` finds the file; the answer to the `/` form; the length of the
longest path whose `mkdir` answered 200, and the code of the first refusal. Delete the nested folders
afterwards from the deepest up with `u --yes xbdm rm --dir ...`.

### Q13. Is there an escape for `"` inside a quoted value? (Low)

```sh
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test\a\"b.txt"'
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test\a""b.txt"'
u --yes xbdm raw 'getfileattributes name="HDD:\updclient-test\five.txt" extra="x\"y"'
```

UpdClient assumes: no escape exists, so it refuses every name with `"` before sending.
Report: the three answers. Do not create files with such names.

### Q14. Maximum command line length (Low)

```sh
for n in 200 400 500 512 600 800 1000; do
  pad=$(printf '%*s' "$n" '' | tr ' ' a)
  printf '%s: ' "$n"; u --yes xbdm raw "getfileattributes name=\"HDD:\\updclient-test\\$pad\"" | head -1
done
```

`xbdm raw` stops at 1022 characters; for longer lines use `P raw "..."`.
UpdClient assumes: nothing beyond its own limit of 1024 bytes; `setmem` lines carry at most 64 bytes.
Report: the longest line answered as a missing or invalid file would be (402, 412), the code and text of
the first one refused for its length (406 or 446?), and whether the connection stayed usable.

### Q15. How many connections at once (401), and does the notification channel count? (Low)

```sh
P connections 40
```

Then, with one notification channel open in a second terminal (`P notify 'notify' 60`), run it again.
UpdClient assumes: one command connection per client; a refusal is `401- max number of connections
exceeded` instead of the greeting, then a close; it reports `LimitExceeded` and does not retry.
Report: how many were accepted each time; the refusal line; whether the refused connection got
`201- connected` first.

### Q16. Does the console close idle connections? (Low)

```sh
P idle 60
P idle 300
P idle 900
```

UpdClient assumes: no keepalive; a dead connection shows as an error on the next command and the
application reconnects.
Report: whether `dbgname` was answered after each wait, or the error.

### Q17. `reconnectport` and `reverse` on `notify`; `boxid`, `dedicate` (Low)

```sh
P notify 'notify reconnectport=0x3039 reverse' 30
P notify 'notify reconnectport=0x3039' 30
u --yes xbdm raw 'boxid'
u --yes xbdm raw 'dedicate'
```

UpdClient does not use any of these. With `reverse` the console might try to connect back to this
machine on port 0x3039; nothing listens, so expect nothing.
Report: everything the probe prints; the two `raw` answers.

### Q18. The 205 line, and commands on a dedicated connection (Low)

```sh
P notify 'notify' 20
```

The probe sends `dbgname` after the first second of silence.
Report: the exact 205 line; any notification lines and whether some came before 205; what (if anything)
answered `dbgname`.

### Q19. Are FILETIMEs UTC or local? `setsystime` and `tz` (Medium: changes the clock)

```sh
u file send five.txt /HDD/updclient-test/now.txt; date -u
u xbdm stat /HDD/updclient-test/now.txt
```

`stat` prints times as UTC. Compare with `date -u` and with the console's own clock and time zone.
Then, only if changing the clock is acceptable on this target:

```sh
python3 -c 'import time; f = int(time.time() * 1e7) + 116444736000000000; print("clockhi=0x%x clocklo=0x%x" % (f >> 32, f & 0xffffffff))'
u --yes xbdm raw 'setsystime clockhi=0x... clocklo=0x...'      # the values printed above
u --yes xbdm raw 'setsystime clockhi=0x... clocklo=0x... tz=0'
```

UpdClient assumes: UTC.
Report: the offset between `stat` and `date -u`; the console's time zone setting; both `setsystime`
answers.

### Q20. `consoletype` values; `systeminfo`, `dmversion`, `getconsoleid` (Low)

```sh
u --yes xbdm raw 'consoletype'
u --yes xbdm raw 'getconsoleid'
u --yes xbdm raw 'systeminfo'
u --yes xbdm raw 'dmversion'
```

UpdClient assumes: `consoletype` and `getconsoleid` (`consoleid=<id>`) answer 200; it never sends
`systeminfo` or `dmversion`.
Report: all four answers in full (body lines too). The console id identifies the console; mask it before
posting the report publicly.

### Q21. `whomadethis` (Low)

```sh
u --yes xbdm raw 'whomadethis'
```

Report: the answer.

### Q22. `help` (Low)

```sh
u --yes xbdm raw 'help'
```

Report: the status line, the number of body lines, and the trace with all of them. Whether every command
of the support matrix (spec 3.0) is listed decides whether `help` can serve as a capability probe.

### Q23. `magicboot`: answers and effects (Medium: reboots and restarts titles)

Keep a second connection open during each reboot to see whether it is dropped too (N7):
`P idle 120` in a second terminal, started just before.

```sh
u --yes xbdm reboot --cold
u info                                                  # repeat until it answers; note the time
u --yes power reboot
u info
u --yes xbdm launch /HDD/path/to/your/default.xex      # a title you own
u --yes xbdm raw 'magicboot title="HDD:\path\to\your\default.xex" directory="HDD:\path\to\your\"'
u --yes xbdm raw 'magicboot title="HDD:\path\to\your\default.xex"'
```

UpdClient assumes: a 2xx, or the connection closing without an answer, both mean success; the CLI says
which ("acknowledged" or "closed the connection without an answer"). `launch` sends `directory` without
a trailing backslash, except for a drive root, where it sends `directory="FLASH:\"` (N3).
Report, per command: the answer (trace); where the console ended up (dashboard, the same title
restarted, the launched title); how long until `u info` answered again; whether the second connection
was dropped.

### Q24. `getmemex`: presence, largest length, block size, bit 15 (Medium: reads memory)

> Read only regions whose `protect` value contains neither 0x200 nor 0x280. Reading those can freeze the
> console (spec 3.16).

```sh
u xbdm regions                                          # pick a region R with protect 0x00000004
P getmemex 0x<R base> 0x400
P getmemex 0x<R base> 0x8000
P getmemex 0x<R base> 0x10000
P getmemex 0x<R base> 0x20000
P getmemex 0x<R base> 0x40000
P getmemex 0x<R base + R size - 0x10> 0x20             # crosses the end of R
u mem peek 0x<R base> 64
```

UpdClient assumes: blocks of at most 0x7FFF bytes; a read ends once `length` bytes have arrived,
whatever bit 15 says; bit 15 before that means the rest is unreadable; at most 0x20000 per request.
Report: for each probe the block headers and whether anything followed the data; the largest length
answered and the code of the first refusal; what the region-crossing read returned. If `getmemex`
answers 407, `mem peek` falls back to `getmem`, in requests of at most 0x400 bytes: report that too.

### Q25. `getmem` on unmapped memory, and bytes per line (Medium: reads memory)

```sh
u --yes xbdm raw 'getmem addr=0x00001000 length=0x10'
u --yes xbdm raw 'getmem addr=0x<R base> length=0x400'
u --yes xbdm raw 'getmem addr=0x<R base + R size - 0x8> length=0x10'
```

UpdClient assumes: 202 with `??` for unreadable bytes, in pairs; any number of bytes per line.
Report: the answers; the number of hex digits per body line; whether `?` ever appears unpaired.

### Q26. `setmem` on a partly unmapped range, and its 200 text (**HIGH**)

> **WARNING: writes console memory. It can crash the title or hang the console.** Only on a devkit you can
> power-cycle, with a title running that you wrote, in a writable data region of that title (protect
> 0x00000004), never in kernel or system memory. Read the bytes first and write them back afterwards.

```sh
u mem peek 0x<E - 2> 4                                  # E: the end of a writable region; note the bytes
u --yes xbdm raw 'setmem addr=0x<E - 2> data=AABBCCDD'
u mem peek 0x<E - 2> 2
u --yes xbdm raw 'setmem addr=0x<E - 2> data=<the two bytes noted>'
```

UpdClient assumes: 404 for unmapped memory, and nothing about partial writes; it sends at most 64 bytes
per line, data as upper-case hex without quotes.
Report: both answers in full; whether the two mapped bytes changed.

### Q27. Do `setmemex`, `sendvfile` and `setfileattributes` exist? (Low)

```sh
u --yes xbdm raw 'setmemex'
u --yes xbdm raw 'sendvfile'
u --yes xbdm raw 'setfileattributes'
```

Without arguments a console that knows the command should refuse with an argument error rather than
407 "unknown command".
Report: the three answers.

### Q28. Which `walkmem` protections are unsafe to read? (**HIGH** for the second part)

```sh
u xbdm regions
```

Report the full list (attach the trace). Then, optionally:

> **WARNING: this can freeze the console until it is power-cycled.** Devkit only, nothing unsaved.

Peek 16 bytes at the start of one region of each protect value, safe-looking values first, and stop at
the first hang: `u mem peek 0x<base> 16`.
Report: the protect values read without trouble, and the one that hung, if any.

### Q29. A command before the greeting, two commands in one segment (Low)

```sh
P early
P pipelined
```

UpdClient never does either; the mock accepts both. ON's devkit failure with `command\r\nbye\r\n`
(spec 1.10) suggests some target does not.
Report: what each probe prints; whether a following `u info` works.

### Q30. UDP name lookup: does type 1 match case-insensitively; is UDP answered at all? (Low)

```sh
updclient discover --protocol xbdm
python3 /path/to/UpdClient/docs/xbdm_probe.py 255.255.255.255 name
python3 /path/to/UpdClient/docs/xbdm_probe.py 255.255.255.255 name MyDebugName
python3 /path/to/UpdClient/docs/xbdm_probe.py 255.255.255.255 name mydebugname
```

Use the console's real debug name (from `u info`) in its exact case and in another case. If the broadcast
does not reach the console, send to its address instead of 255.255.255.255.
UpdClient assumes: type 3 is answered by every console with type 2; type 1 only by the console of that
name, compared case-insensitively.
Report: the replies to each; whether `discover` lists the console and with which name.

### Q31. Is the `getfile` length really little-endian? (Low)

```sh
P getfile-length 'HDD:\updclient-test\five.txt'
u file get /HDD/updclient-test/five.txt five-back.txt
```

UpdClient assumes: little-endian. If it were big-endian, `file get` would fail with `LimitExceeded`
because the length would exceed the size from `getfileattributes`.
Report: the four bytes printed; whether `file get` succeeded.

## 4. Questions added by the client-mock integration

### N1 (spec Q32). Is the `sendfile` target created before the 204? (Low)

```sh
P partial 'HDD:\updclient-test\early.bin' 1000 0
u xbdm ls /HDD/updclient-test
```

The probe closes right after the 204, without data. UpdClient now assumes the file may exist and queues
its temporary name for deletion when the 204 is lost.
Report: whether `early.bin` exists, and its size. Delete it.

### N2 (spec Q33). Does every target implement `getfileattributes`? (Low)

Covered by Q1: report the answer. On a 407, `file get` takes the size from the parent's listing and
`file send` finds a file to replace there; on any other 4xx, `file get` works without the size bound.

### N3 (spec Q34). Launching a file in a drive root (Medium)

```sh
u --yes xbdm launch /USB0/default.xex                  # a title of yours in a drive root, if you have one
```

UpdClient sends `directory="USB0:\"`; DevTool sends only `title` for `FLASH:` roots (spec 3.14).
Report: the answer and whether the title started.

### N4 (spec Q35). Number and data formats the client sends (Low)

Every command above uses them: lower-case `0x` hex without padding (`length=0x2a`, `addr=0x82000000`),
`setmem` data as upper-case hex without quotes, values quoted without escapes.
Report any command of this plan refused with 423 or a parse error that looks related.

### N5 (spec Q36). Is `bye` answered before the close? (Low)

Every `u` command ends with `bye`.
Report: whether the trace shows `< 200- bye` after `> bye` on this target.

### N6 (spec Q37). Is a refused connection greeted first? (Low)

Covered by Q15.

### N7 (spec Q38). Does a reboot drop every connection, or only the one that asked? (Medium)

Covered by Q23 (the second connection).

### N8 (spec Q39). Screenshot geometry and size (Low)

```sh
u xbdm screenshot -o shot-$NAME.raw
```

UpdClient assumes: one geometry line `pitch= width= height= format= offsetx= offsety= framebuffersize=`
(commas allowed after values), then `framebuffersize` bytes, at most `pitch * height` rounded up to 32
rows and at most 64 MiB. It saves the bytes as they come (tiled).
Report: the geometry line from the trace; whether the command succeeded; the file size; what the screen
showed. Keep `shot-<target>.raw` for writing an untiler.

## 5. After the plan

```sh
u xbdm ls /HDD/updclient-test
```

Delete everything left in the scratch folder with `u --yes xbdm rm ...` (files) and
`u --yes xbdm rm --dir ...` (empty folders, deepest first), then the folder itself. Send back the
reports, the trace files and the probe output.
