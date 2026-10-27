#!/usr/bin/env python3
"""Serial logger for RTC_Bench.

Holds the port open, writes every line with a host timestamp to LOG, and sends
whatever text is written into CMD to the board (the file is then emptied).
Host timestamps make it possible to compare the RTC against the PC clock.

    scripts/serial_log.py /dev/ttyACM0 bench.log cmd.txt [--reset]
    echo -n B > cmd.txt        # start the benchmark from another shell

Requires pyserial.
"""
import datetime
import sys
import time

import serial

port, log, cmdf = sys.argv[1], sys.argv[2], sys.argv[3]
open(cmdf, "w").close()
p = serial.Serial(port, 115200, timeout=0.05)
if "--reset" in sys.argv:  # pulse EN via RTS, like esptool
    p.dtr = False
    p.rts = True
    time.sleep(0.1)
    p.rts = False
    p.dtr = True
f = open(log, "a", buffering=1)


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


f.write(f"{ts()} ==== monitor opened {port}\n")
part = b""
while True:
    try:
        d = p.read(4096)
    except Exception as e:  # board reset / re-enumeration
        f.write(f"{ts()} ==== read error: {e}\n")
        time.sleep(1)
        try:
            p.close()
            p = serial.Serial(port, 115200, timeout=0.05)
        except Exception:
            pass
        continue
    if d:
        part += d
        while b"\n" in part:
            line, part = part.split(b"\n", 1)
            f.write(f"{ts()} {line.decode(errors='replace').rstrip()}\n")
    try:
        c = open(cmdf).read()
        if c:
            open(cmdf, "w").close()
            p.write(c.encode())
            f.write(f"{ts()} >>>> sent {c!r}\n")
    except Exception:
        pass
