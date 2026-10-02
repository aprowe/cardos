#!/usr/bin/env python3
"""Work on the device over USB: files, commands, the screen.

    python tools/cardctl.py state                 # which shell and app, heap
    python tools/cardctl.py sh "update" "mem"     # console lines, output back
    python tools/cardctl.py ls /config
    python tools/cardctl.py cat /config/today.txt
    python tools/cardctl.py get /config/today.txt [local]
    python tools/cardctl.py put local /config/today.txt
    python tools/cardctl.py append /config/today.txt "Focus | daily focus"
    python tools/cardctl.py rm PATH | mv A B | mkdir PATH
    python tools/cardctl.py open todo             # an app, as a hotkey opens it
    python tools/cardctl.py key quit              # or enter, esc, or text:"abc", or hex:84
    python tools/cardctl.py shot [out.png]        # the screen, over the cable

It talks to kernel/sys/serlink.c in frames (kernel/sys/serframe.h), not
keystrokes: nothing is typed into whatever is on screen, the reply is
picked out of the log lines around it, and a CRC says it arrived whole.
A request that is safe to repeat is repeated when the port drops out, which
the USB serial port does; one that is not (sh) asks for its reply again.
Needs firmware with the link (2026-10-01); older firmware ignores frames.
"""
import base64
import os
import struct
import sys
import time
import zlib

import serial

PORT = os.environ.get("CARDOS_PORT", "COM3")
CHUNK = 3072          # a get
PUT = 1024            # a put: the whole frame must fit the device's 2 KB receive ring
STX, ETX = b"\x02", b"\x03"


class LinkError(Exception):
    pass


class Link:
    def __init__(self, port=PORT):
        self.port = port
        self.s = None
        self.seq = int(time.time()) % 100000
        self.log = os.environ.get("CARDCTL_LOG") == "1"
        self.open()

    def open(self):
        if self.s:
            try:
                self.s.close()
            except Exception:
                pass
        deadline = time.time() + 15
        while True:
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout, s.dtr, s.rts = self.port, 115200, 0.1, False, False
                s.open()
                self.s = s
                return
            except serial.SerialException:
                if time.time() > deadline:
                    raise LinkError("cannot open %s -- is the device plugged in?" % self.port)
                time.sleep(0.5)

    def _send(self, fields):
        body = "\t".join(fields).encode()
        self.s.write(STX + body + b"\t%08x" % zlib.crc32(body) + ETX)

    def _wait(self, rid, timeout):
        """The reply frame with this id, or None at the timeout."""
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                buf += self.s.read(4096)
            except serial.SerialException:
                self.open()
                return "dropped"
            while True:
                a = buf.find(STX)
                if a < 0:
                    if self.log and buf:
                        sys.stderr.write(buf.decode("utf-8", "replace"))
                    buf = b""
                    break
                if self.log and a:
                    sys.stderr.write(buf[:a].decode("utf-8", "replace"))
                b = buf.find(ETX, a)
                if b < 0:
                    buf = buf[a:]
                    break
                frame, buf = buf[a + 1:b], buf[b + 1:]
                head, _, crc = frame.rpartition(b"\t")
                f = head.split(b"\t")
                if len(f) != 3 or f[0] != b"@" + rid.encode():
                    continue                  # someone else's, or a stale one
                if crc != b"%08x" % zlib.crc32(head):
                    return "corrupt"
                return f[1].decode(), base64.b64decode(f[2])
        return None

    def call(self, verb, *args, timeout=10, safe=True):
        """(ok, data bytes). `safe` requests are sent again when lost."""
        self.seq += 1
        rid = str(self.seq)
        fields = [rid, verb] + [str(a) for a in args]
        for attempt in range(4):
            try:
                self._send(fields if attempt == 0 or safe else [rid, "re"])
            except serial.SerialException:
                self.open()
                continue
            got = self._wait(rid, timeout)
            if isinstance(got, tuple):
                return got[0] == "ok", got[1]
            if got is None and not safe:
                timeout = 10                  # it ran; now just ask again
        raise LinkError("no answer to %s (is the firmware new enough to have the link?)" % verb)

    def must(self, verb, *args, **kw):
        ok, data = self.call(verb, *args, **kw)
        if not ok:
            raise LinkError("%s %s: %s" % (verb, " ".join(map(str, args)), data.decode("utf-8", "replace")))
        return data

    # -- files --

    def stat(self, path):
        ok, data = self.call("stat", path)
        if not ok:
            return None
        size, kind, mtime = data.decode().split("\t")
        return int(size), kind == "d", int(mtime)

    def read(self, path):
        st = self.stat(path)
        if not st:
            raise LinkError("%s: not there" % path)
        if st[1]:
            raise LinkError("%s is a folder" % path)
        out = b""
        while len(out) < st[0]:
            piece = self.must("get", path, len(out), CHUNK)
            if not piece:
                break
            out += piece
        return out

    def write(self, path, data):
        off = 0
        while True:
            piece = data[off:off + PUT]
            self.must("put", path, off, base64.b64encode(piece).decode())
            off += len(piece)
            if off >= len(data):
                break
        self.must("commit", path, len(data))


def png(rgb565, w=240, h=135):
    """The device's raw shot (RGB565, high byte first, as the panel takes it)
    as a PNG, without needing Pillow."""
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")
        for x in range(w):
            v = struct.unpack_from(">H", rgb565, (y * w + x) * 2)[0]
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            row += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


KEYS = {"enter": b"\r", "esc": b"\x1b", "quit": b"\x84", "backspace": b"\x7f",
        "up": b";", "down": b".", "left": b",", "right": b"/", "tab": b"\t"}


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cmd, args = argv[0], argv[1:]
    link = Link()
    out = sys.stdout.buffer
    if cmd == "ping":
        out.write(link.must("ping") + b"\n")
    elif cmd == "state":
        out.write(link.must("state") + b"\n")
    elif cmd == "sh":
        for line in args:
            ok, data = link.call("sh", line, timeout=600, safe=False)
            out.write(data)
            if not ok:
                return 1
    elif cmd == "ls":
        out.write(link.must("ls", args[0] if args else "/"))
    elif cmd == "stat":
        print(link.stat(args[0]) or "not there")
    elif cmd == "cat":
        out.write(link.read(args[0]))
    elif cmd == "get":
        data = link.read(args[0])
        dest = args[1] if len(args) > 1 else os.path.basename(args[0])
        with open(dest, "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (dest, len(data)))
    elif cmd == "put":
        with open(args[0], "rb") as f:
            data = f.read()
        t = time.time()
        link.write(args[1], data)
        print("%s: %d bytes in %.1f s" % (args[1], len(data), time.time() - t))
    elif cmd == "append":
        st = link.stat(args[0])
        old = link.read(args[0]) if st else b""
        if old and not old.endswith(b"\n"):
            old += b"\n"
        link.write(args[0], old + "\n".join(args[1:]).encode() + b"\n")
        print("%s: %d lines added" % (args[0], len(args) - 1))
    elif cmd in ("rm", "mkdir"):
        link.must(cmd, args[0])
    elif cmd == "mv":
        link.must("mv", args[0], args[1])
    elif cmd == "open":
        link.must("open", *args[:2])
    elif cmd == "key":
        for k in args:
            raw = (KEYS.get(k) or (k[5:].encode() if k.startswith("text:") else None)
                   or (bytes.fromhex(k[4:]) if k.startswith("hex:") else None))
            if raw is None:
                raise LinkError("key: %s is not one of %s, text:..., hex:..." % (k, ", ".join(KEYS)))
            link.must("key", base64.b64encode(raw).decode())
            time.sleep(0.15)
    elif cmd == "shot":
        path = link.must("shot").decode()
        data = link.read(path)
        dest = args[0] if args else "shot.png"
        with open(dest, "wb") as f:
            f.write(png(data))
        print(dest)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except LinkError as e:
        sys.exit("cardctl: %s" % e)
