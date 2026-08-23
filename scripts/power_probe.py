#!/usr/bin/env python3
"""Run the on-device power probes over the serial console and print what they say.

CMD:PWRPROF measures the loads that can be switched off one at a time
(frontlight, GT911, then what is left) by differencing the gauge's current
between steps; it ends with an esp_restart to bring the digitizer back.
CMD:LSLEEP:<s> / CMD:BASE:<s> spend that many seconds with and without light
sleep and report the coulomb-counter drop, so the saving is measured rather
than assumed. Both need the device on battery: CMD:HIZ:1 puts the charger in
high-impedance so the gauge sees the load and not the cable.

Usage:
  python scripts/power_probe.py --port COM10 --prof
  python scripts/power_probe.py --port COM10 --lsleep 1800 --base 1800
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


def drain(ser, seconds, echo=True, stop_on=None):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if not chunk:
            continue
        buf += chunk
        if echo:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
        if stop_on and any(m in buf for m in stop_on):
            break
    return buf


def send(ser, cmd):
    ser.write(f"CMD:{cmd}\n".encode())
    ser.flush()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--prof", action="store_true", help="run CMD:PWRPROF (frontlight / GT911 / floor)")
    ap.add_argument("--lsleep", type=int, default=0, help="seconds of light-sleep probe")
    ap.add_argument("--base", type=int, default=0, help="seconds of no-sleep baseline probe")
    ap.add_argument("--hiz", action="store_true", help="put the charger in HI-Z first (USB attached)")
    args = ap.parse_args()

    with open_port(args.port, args.baud, 0.2) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        if args.hiz:
            send(ser, "HIZ:1")
            drain(ser, 2)

        if args.prof:
            print("--- PWRPROF (each step waits 4 s for the gauge's averaging window) ---")
            send(ser, "PWRPROF")
            drain(ser, 60, stop_on=[b"PROF_SUMMARY", b"PROF_ERR"])
            print("\n(the device restarts itself to restore the digitizer)")
            return 0

        for cmd, secs in (("BASE", args.base), ("LSLEEP", args.lsleep)):
            if not secs:
                continue
            print(f"--- {cmd}:{secs} ({secs / 60:.0f} min) ---")
            send(ser, f"{cmd}:{secs}")
            # The console dies during light sleep and does not come back until
            # the cable is replugged, so the result lives in RTC_NOINIT: ask for
            # it afterwards instead of expecting a live report.
            drain(ser, secs + 30, stop_on=[b"LSLEEP_ERR", b"LSLEEP_DONE"])
            send(ser, "LSLEEPRESULT")
            drain(ser, 5)
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
