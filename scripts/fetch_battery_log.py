#!/usr/bin/env python3
"""Pull /.crosspoint/battery.csv off a connected device over the serial console.

The firmware answers CMD:BATTLOG with

    BATTLOG_START:<bytes>
    <csv bytes, paced in 192-byte chunks>
    BATTLOG_END

Usage:  python scripts/fetch_battery_log.py --port COM10 --out battery.csv
"""
import argparse
import sys
import time

import serial


def open_port(port: str, baud: int, timeout: float) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = timeout
    # Never toggle DTR/RTS: the USB-Serial/JTAG ROM watches them for esptool's
    # reset dance, so simply opening the port with the defaults reboots the
    # running firmware -- which showed up as a log full of BOOT rows and no
    # usable intervals.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="battery.csv")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the dump")
    args = ap.parse_args()

    with open_port(args.port, args.baud, 0.2) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(b"CMD:BATTLOG\n")
        ser.flush()

        deadline = time.time() + args.timeout
        buf = bytearray()
        total = None
        start = None
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                if total is None and b"BATTLOG_START:" in buf:
                    head = buf.index(b"BATTLOG_START:")
                    nl = buf.find(b"\n", head)
                    if nl != -1:
                        total = int(buf[head + len(b"BATTLOG_START:"):nl])
                        start = nl + 1
                if total is None and b"BATTLOG_ERR" in buf:
                    print("device says: no battery.csv on the SD card", file=sys.stderr)
                    return 2
                if total is not None and b"BATTLOG_END" in buf[start:]:
                    break

        if total is None:
            print("no BATTLOG_START seen — is the device awake and the console free?", file=sys.stderr)
            return 1

        body = buf[start:]
        end = body.find(b"\nBATTLOG_END")
        if end != -1:
            body = body[:end]
        # Log lines the firmware prints while dumping would corrupt the CSV; keep
        # only rows that look like the telemetry format (uptime first field).
        rows = [ln for ln in body.split(b"\n") if ln[:1].isdigit() or ln.startswith(b"uptime_s")]
        out = b"\n".join(rows) + b"\n"
        with open(args.out, "wb") as f:
            f.write(out)
        print(f"{args.out}: {len(out)} bytes ({len(rows)} lines), device reported {total} bytes")
        if len(out) < total * 0.9:
            print("warning: short read — re-run to confirm", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
