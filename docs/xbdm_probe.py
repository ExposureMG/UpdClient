#!/usr/bin/env python3
"""Raw XBDM probes for docs/HARDWARE_TEST_PLAN.md.

These do what UpdClient deliberately never does (send a command before the
greeting, pipeline, cut an upload short, hold connections open) and print every
line exchanged. Python 3 standard library only.

    python3 xbdm_probe.py HOST[:PORT] raw 'dbgname' ['consoletype' ...]
    python3 xbdm_probe.py HOST early
    python3 xbdm_probe.py HOST pipelined
    python3 xbdm_probe.py HOST partial 'HDD:\\updclient-test\\cut.bin' LENGTH SENT
    python3 xbdm_probe.py HOST getfile-length 'HDD:\\updclient-test\\five.txt'
    python3 xbdm_probe.py HOST getmemex ADDRESS LENGTH
    python3 xbdm_probe.py HOST connections MAX
    python3 xbdm_probe.py HOST idle SECONDS
    python3 xbdm_probe.py HOST notify 'notify' [SECONDS]
    python3 xbdm_probe.py BROADCAST name [NAME]      (UDP; no NAME sends the wildcard)
"""

import socket
import struct
import sys
import time

PORT = 730


def endpoint(text):
    host, _, port = text.partition(":")
    return host, int(port) if port else PORT


def connect(target, timeout=10.0):
    s = socket.create_connection(endpoint(target), timeout=timeout)
    s.settimeout(timeout)
    return s


class Lines:
    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        raw, _, self.buf = self.buf.partition(b"\n")
        text = raw.rstrip(b"\r").decode("latin-1")
        print("< " + text)
        return text

    def exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed after %d of %d bytes" % (len(self.buf), n))
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out


def send(sock, text):
    print("> " + text)
    sock.sendall(text.encode("latin-1") + b"\r\n")


def answer(lines):
    status = lines.line()
    if status.startswith("202"):
        while lines.line() != ".":
            pass
    return status


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    target, what, args = argv[1], argv[2], argv[3:]
    if what == "name":
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        u.settimeout(1.5)
        packet = b"\x03\x00" if not args else bytes([1, len(args[0])]) + args[0].encode("ascii")
        u.sendto(packet, endpoint(target))
        try:
            while True:
                data, sender = u.recvfrom(512)
                print("reply from %s: %s" % (sender[0], data.hex()), data[2:2 + data[1]] if len(data) > 1 else b"")
        except socket.timeout:
            print("no more replies")
        return 0
    if what == "connections":
        held = []
        for i in range(int(args[0])):
            s = connect(target)
            if not Lines(s).line().startswith("201"):
                print("connection %d refused" % (i + 1))
                break
            held.append(s)
        print("%d connections accepted; closing them in 5 s" % len(held))
        time.sleep(5)
        return 0
    s = connect(target)
    lines = Lines(s)
    if what == "early":
        send(s, "dbgname")
        lines.line()
        lines.line()
        return 0
    lines.line()
    if what == "raw":
        for command in args:
            send(s, command)
            answer(lines)
    elif what == "pipelined":
        print("> dbgname + consoletype in one segment")
        s.sendall(b"dbgname\r\nconsoletype\r\n")
        lines.line()
        lines.line()
    elif what == "partial":
        path, length, sent = args[0], int(args[1], 0), int(args[2], 0)
        send(s, 'sendfile name="%s" length=0x%x' % (path, length))
        if lines.line().startswith("204"):
            s.sendall(b"\xa5" * sent)
            if sent >= length:
                print("> [%d bytes]" % sent)
                lines.line()
            else:
                print("> [%d of %d bytes], then close" % (sent, length))
    elif what == "getfile-length":
        send(s, 'getfile name="%s"' % args[0])
        if lines.line().startswith("203"):
            raw = lines.exact(4)
            print("length bytes %s: little-endian %d, big-endian %d" %
                  (raw.hex(), struct.unpack("<I", raw)[0], struct.unpack(">I", raw)[0]))
    elif what == "getmemex":
        address, length = int(args[0], 0), int(args[1], 0)
        send(s, "getmemex addr=0x%x length=0x%x" % (address, length))
        if lines.line().startswith("203"):
            got = 0
            while got < length:
                header = struct.unpack("<H", lines.exact(2))[0]
                count, last = header & 0x7FFF, bool(header & 0x8000)
                lines.exact(count)
                got += count
                print("block header 0x%04x: %d bytes, last=%s, %d of %d" % (header, count, last, got, length))
                if last:
                    break
            s.settimeout(1.0)
            try:
                extra = s.recv(64)
                print("after the data: %r" % extra)
            except socket.timeout:
                print("nothing after the data")
    elif what == "idle":
        time.sleep(float(args[0]))
        try:
            send(s, "dbgname")
            lines.line()
        except (OSError, EOFError) as error:
            print("after %s s idle: %s" % (args[0], error))
    elif what == "notify":
        send(s, args[0])
        end = time.time() + (float(args[1]) if len(args) > 1 else 30)
        s.settimeout(1.0)
        asked = False
        while time.time() < end:
            try:
                lines.line()
            except socket.timeout:
                if not asked:
                    send(s, "dbgname")
                    asked = True
            except EOFError as error:
                print(error)
                break
    else:
        print(__doc__)
        return 2
    s.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (OSError, EOFError) as error:
        print("error: %s" % error)
        sys.exit(1)
