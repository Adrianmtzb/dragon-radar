#!/usr/bin/env python3
"""Grab the LCD framebuffer over serial (`shot` command) and save it as a PNG.

Usage: tools/screenshot.py /dev/cu.usbmodemXXXX out.png
No pyserial needed: the port is configured with termios.
"""
import base64
import os
import select
import struct
import sys
import termios
import time
import zlib


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0  # lflag: raw
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_shot(fd, timeout=15):
    os.write(fd, b"\nshot\n")
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            buf += os.read(fd, 65536)
            if b"ENDSHOT" in buf:
                break
    start = buf.find(b"SHOT ")
    end = buf.find(b"ENDSHOT")
    if start < 0 or end < 0:
        raise SystemExit("no screenshot received")
    header, _, body = buf[start:end].partition(b"\n")
    w, h = map(int, header.split()[1:3])
    data = base64.b64decode(b"".join(body.split()))
    return w, h, data


def write_png(path, w, h, rgb565):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            (v,) = struct.unpack_from("<H", rgb565, (y * w + x) * 2)  # little-endian in memory
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
        rows.append(bytes(row))

    def chunk(kind, payload):
        c = struct.pack(">I", len(payload)) + kind + payload
        return c + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    fd = open_port(sys.argv[1])
    try:
        w, h, data = read_shot(fd)
    finally:
        os.close(fd)
    write_png(sys.argv[2], w, h, data)
    print(f"saved {sys.argv[2]} ({w}x{h})")


if __name__ == "__main__":
    main()
