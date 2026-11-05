#!/usr/bin/env python3
"""Record the board's microphones (CLI `rec`) and save a WAV.

Usage: tools/record.py PORT out.wav [ms] [sfx name | tone HZ MS [WAVE] [VOL]]
       tools/record.py /dev/cu.usbmodemXXXX build/audio/beep.wav 800 radar_beep
The board starts recording, plays the sound, and dumps the 16-bit stereo PCM (MIC1 left, MIC2
right) as base64 between "REC <rate> <channels> <frames>" and "ENDREC".
No pyserial needed: the port is configured with termios, as in tools/screenshot.py.
"""
import base64
import os
import select
import sys
import termios
import time
import wave


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


def record(fd, args, timeout):
    os.write(fd, ("\nrec " + " ".join(args) + "\n").encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            try:
                buf += os.read(fd, 65536)
            except BlockingIOError:
                continue
            if b"ENDREC" in buf:
                break
    start, end = buf.find(b"REC "), buf.find(b"ENDREC")
    if start < 0 or end < 0:
        tail = buf[-300:].decode("utf-8", "replace")
        raise SystemExit(f"no recording received; the board said:\n{tail}")
    header, _, body = buf[start:end].partition(b"\n")
    rate, channels, frames = map(int, header.split()[1:4])
    data = base64.b64decode(b"".join(body.split()))
    if len(data) != frames * channels * 2:
        raise SystemExit(f"corrupt dump: {len(data)} bytes for {frames} frames")
    return rate, channels, data


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    port, out = sys.argv[1], sys.argv[2]
    args = sys.argv[3:] or ["1000"]
    ms = int(args[0])
    fd = open_port(port)
    try:
        rate, channels, data = record(fd, args, timeout=ms / 1000 + 25)
    finally:
        os.close(fd)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with wave.open(out, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)
    print(f"saved {out} ({len(data) // (2 * channels)} frames at {rate} Hz, {channels} ch)")


if __name__ == "__main__":
    main()
