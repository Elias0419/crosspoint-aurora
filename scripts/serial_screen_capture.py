#!/usr/bin/env python3
"""Capture CrossPoint screens over USB serial (T5S3 debug loop).

Usage:
  python t5s3cap.py --port COM10 [--key DOWN --key CONFIRM:600 ...] [--settle 2.0] --out shot.png
  python t5s3cap.py --port COM10 --listen 5          # just tail logs for 5s

Each --key sends CMD:KEY:<NAME>[:<holdMs>] and waits --keydelay between keys.
Then waits --settle seconds (e-ink refresh) and pulls the framebuffer with
CMD:SCREENSHOT (1bpp, MSB-first, 1=white). Saves native landscape PNG and, if
--rot is given, a rotated copy.
"""

import argparse
import sys
import time

import serial
from PIL import Image


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 3
    # Never toggle DTR/RTS: the USB-Serial/JTAG ROM watches them for the
    # esptool reset dance and would reboot the running firmware.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def read_line(ser: serial.Serial) -> bytes:
    return ser.readline()


def capture(ser: serial.Serial, out_path: str, rot: int, timeout_s: float = 15.0) -> bool:
    ser.reset_input_buffer()
    ser.write(b"CMD:SCREENSHOT\n")
    ser.flush()
    width, height = 960, 540
    deadline = time.time() + timeout_s
    size = None
    while time.time() < deadline:
        line = read_line(ser)
        if not line:
            continue
        text = line.decode("utf-8", errors="replace").strip()
        if text.startswith("SCREENSHOT_DIM:"):
            try:
                w, h = text.split(":", 1)[1].split("x")
                width, height = int(w), int(h)
            except ValueError:
                pass
        elif text.startswith("SCREENSHOT_START:"):
            try:
                size = int(text.split(":", 1)[1])
            except ValueError:
                return False
            break
        else:
            print(f"  [log] {text}")
    if size is None:
        print("no SCREENSHOT_START seen", file=sys.stderr)
        return False

    data = bytearray()
    while len(data) < size and time.time() < deadline:
        chunk = ser.read(size - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != size:
        print(f"short read: {len(data)}/{size}", file=sys.stderr)
        return False
    tail = read_line(ser).decode("utf-8", errors="replace").strip()
    if "SCREENSHOT_END" not in tail:
        # one stray log line between payload and marker is fine; try once more
        tail = read_line(ser).decode("utf-8", errors="replace").strip()
        if "SCREENSHOT_END" not in tail:
            print(f"missing SCREENSHOT_END (got {tail!r}); saving anyway", file=sys.stderr)

    # infer dims from size if the DIM line was missed
    if size != (width // 8) * height:
        known = {64800: (960, 540), 48000: (800, 480), 52272: (792, 528)}
        width, height = known.get(size, (size // 68, 540))

    img = Image.frombytes("1", (width, height), bytes(data))
    if rot:
        img = img.rotate(rot, expand=True)
    img.convert("L").save(out_path)
    print(f"saved {out_path} ({img.width}x{img.height})")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--key", action="append", default=[], help="NAME[:holdMs], e.g. DOWN or CONFIRM:600")
    ap.add_argument("--keydelay", type=float, default=1.2, help="seconds between keys")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds to wait before capture")
    ap.add_argument("--out", default=None, help="output PNG path; omit to skip capture")
    ap.add_argument("--rot", type=int, default=0, help="rotate output CCW degrees (90/180/270)")
    ap.add_argument("--listen", type=float, default=0, help="tail logs for N seconds and exit")
    args = ap.parse_args()

    ser = open_port(args.port)
    try:
        if args.listen:
            end = time.time() + args.listen
            while time.time() < end:
                line = read_line(ser)
                if line:
                    print(line.decode("utf-8", errors="replace").rstrip())
            return 0

        for key in args.key:
            ser.write(f"CMD:KEY:{key}\n".encode())
            ser.flush()
            print(f"sent KEY:{key}")
            # surface the ack if it shows up quickly
            t0 = time.time()
            while time.time() - t0 < 1.0:
                line = read_line(ser)
                if not line:
                    break
                text = line.decode("utf-8", errors="replace").strip()
                print(f"  [log] {text}")
                if text.startswith("KEY_OK") or text.startswith("KEY_ERR"):
                    break
            hold_extra = 0.0
            if ":" in key:
                try:
                    hold_extra = int(key.split(":")[1]) / 1000.0
                except ValueError:
                    pass
            time.sleep(args.keydelay + hold_extra)

        if args.out:
            time.sleep(args.settle)
            ok = capture(ser, args.out, args.rot)
            for attempt in range(2):
                if ok:
                    break
                print(f"retrying capture ({attempt + 1})...")
                ok = capture(ser, args.out, args.rot)
            return 0 if ok else 1
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
