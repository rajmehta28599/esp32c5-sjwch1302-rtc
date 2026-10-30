#!/usr/bin/env python3
"""Serial logger for the RTC firmware.

Holds the port open, writes every line with a host timestamp, and sends
whatever text is written into CMD to the board (the file is then emptied).
Host timestamps make it possible to compare the RTC against the PC clock
(see scripts/daily_summary.py).

    scripts/serial_log.py PORT LOG CMD [--reset] [--summary OUTDIR]
    echo -n C > CMD            # send a command from another shell

LOG is a file, or a directory: then one file per local day,
LOG/rtc_YYYY-MM-DD.log. Each line starts with the host time,
YYYY-MM-DDTHH:MM:SS.mmm+hh:mm (stamped when the first byte of the line
arrives). With --summary, scripts/daily_summary.py LOG OUTDIR runs every
hour. If the board prints nothing for 30 s, the logger resets the ESP32
through RTS (the RTC keeps running). If the port disappears (USB unplugged),
it logs one read error and reopens the port once the board is back.

Requires pyserial.
"""
import datetime
import os
import signal
import subprocess
import sys
import time

import serial

port, log, cmdf = sys.argv[1], sys.argv[2], sys.argv[3]
daily = os.path.isdir(log)
summary_dir = sys.argv[sys.argv.index("--summary") + 1] if "--summary" in sys.argv else None
SUMMARY_EVERY_S = 3600
signal.signal(signal.SIGCHLD, signal.SIG_IGN)  # reap summary runs
summary_script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "daily_summary.py")
open(cmdf, "w").close()


def open_port():
    s = serial.Serial(port, 115200, timeout=0.05)
    s.reset_input_buffer()  # drop output the board buffered while the port was closed
    return s


p = open_port()
QUIET_RESET_S = 30  # the board prints every second; silence = stalled console


def reset_board():  # pulse EN via RTS, like esptool; the RTC keeps running
    p.dtr = False
    p.rts = True
    time.sleep(0.1)
    p.rts = False
    p.dtr = True


if "--reset" in sys.argv:
    reset_board()


def now():
    return datetime.datetime.now().astimezone()


def ts(t):
    return t.isoformat(timespec="milliseconds")


f, f_day = None, None


def write(t, text):
    global f, f_day
    day = t.date()
    if f is None or (daily and day != f_day):
        if f:
            f.close()
        name = os.path.join(log, f"rtc_{day.isoformat()}.log") if daily else log
        f, f_day = open(name, "a", buffering=1), day
    f.write(f"{ts(t)} {text}\n")


write(now(), f"==== monitor opened {port}")
part, part_t = b"", None
last_rx = last_reset = time.monotonic()
next_summary = last_rx + 60
while True:
    if summary_dir and time.monotonic() >= next_summary:
        next_summary = time.monotonic() + SUMMARY_EVERY_S
        subprocess.Popen([sys.executable, summary_script, log, summary_dir],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if p is None:  # board unplugged: retry quietly until it re-enumerates
        time.sleep(1)
        try:
            p = open_port()
            write(now(), "==== port reopened")
            last_rx = time.monotonic()
        except Exception:
            pass
        continue
    if time.monotonic() - max(last_rx, last_reset) > QUIET_RESET_S:
        write(now(), f"==== no data for {QUIET_RESET_S} s, resetting the ESP32")
        last_reset = time.monotonic()
        try:
            reset_board()
        except Exception:
            pass
    try:
        d = p.read(p.in_waiting or 1)
    except Exception as e:  # board reset / re-enumeration
        write(now(), f"==== read error: {e}")
        try:
            p.close()
        except Exception:
            pass
        p = None
        continue
    if d:
        last_rx = time.monotonic()
        t = now()
        while d:
            if part_t is None:
                part_t = t
            line, sep, d = d.partition(b"\n")
            part += line
            if not sep:
                break
            write(part_t, part.decode(errors="replace").rstrip())
            part, part_t = b"", None
    try:
        c = open(cmdf).read()
        if c:
            open(cmdf, "w").close()
            p.write(c.encode())
            write(now(), f">>>> sent {c!r}")
    except Exception:
        pass
