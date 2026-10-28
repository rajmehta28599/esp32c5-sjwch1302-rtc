#!/usr/bin/env python3
"""Daily RTC comparison from the serial logs of scripts/serial_log.py.

    scripts/daily_summary.py [LOGDIR] [OUTDIR]     (defaults: logs results/daily)

Reference: the PC clock, NTP-disciplined (check with `chronyc tracking`).
The firmware prints `[RTC ] Unix: N` when its 100 ms poll sees the seconds
register change, so a line arrives 0..~110 ms after the RTC second edge.
For each hour the script keeps the largest (N - arrival time) over
consecutive ticks: the print that came closest to the edge. That is
RTC - PC with a few ms of bias (USB latency, poll granularity), and the
bias is the same every hour, so drift rates are not affected.

Writes OUTDIR/hourly.csv, OUTDIR/manual_checks.csv (`C` results found in
the logs, merged with rows already in the file) and OUTDIR/DAILY.md.
"""
import csv
import datetime as dt
import glob
import os
import re
import sys

IST_S = 19800
IST = dt.timezone(dt.timedelta(seconds=IST_S))
NOMINAL_PPM = -78.0  # 27 h reference measurement, results/RESULTS.md

RE_RTC = re.compile(r"^\[RTC \] Unix: (\d+) .*?Last U: (\d\d/\d\d/\d{4} \d\d:\d\d:\d\d|never)")
RE_DRIFT = re.compile(r"^\[DRIFT\] RTC - NTP = ([+-]?\d+) s")
RE_SINCE = re.compile(r"^\[DRIFT\] Since last 'U': ([\d.]+) h -> ([+-]?[\d.]+) ppm")

logdir = sys.argv[1] if len(sys.argv) > 1 else "logs"
outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join("results", "daily")
os.makedirs(outdir, exist_ok=True)


def last_u_epoch(s):
    if s == "never":
        return None
    t = dt.datetime.strptime(s, "%d/%m/%Y %H:%M:%S").replace(tzinfo=IST)
    return int(t.timestamp())


def ist(ts, fmt="%d/%m/%Y %H:%M"):
    return dt.datetime.fromtimestamp(ts, IST).strftime(fmt)


def new_day():
    return dict(lines=0, ticks=0, jumps=0, flags=0, errors=0, boots=0, resets=0, gaps=0,
                first=None, last=None)


days = {}
hours = {}  # (hour start, last U) -> [best N - t, n, t of best]
manual = []
prev = None  # (t, N) of the previous [RTC ] line
pending_c = None

for path in sorted(glob.glob(os.path.join(logdir, "rtc_*.log"))):
    with open(path, errors="replace") as f:
        for raw in f:
            stamp, _, text = raw.rstrip("\n").partition(" ")
            try:
                t = dt.datetime.fromisoformat(stamp).timestamp()
            except ValueError:
                continue
            d = days.setdefault(dt.datetime.fromtimestamp(t, IST).date(), new_day())
            d["lines"] += 1
            d["first"] = t if d["first"] is None else d["first"]
            d["last"] = t
            if text.startswith("[RTC*]"):
                d["flags"] += 1
            elif text.startswith("[RTC ] E:") or text.startswith("[RTC] E:"):
                d["errors"] += 1
            elif "Initializing DS1302" in text:
                d["boots"] += 1
                prev = None
            elif text.startswith("==== no data"):
                d["resets"] += 1
            elif text.startswith("==== monitor opened"):
                prev = None

            m = RE_DRIFT.match(text)
            if m:
                pending_c = [stamp, int(m.group(1)), "", ""]
                manual.append(pending_c)
            m = RE_SINCE.match(text)
            if m and pending_c:
                pending_c[2], pending_c[3] = m.group(1), m.group(2)

            m = RE_RTC.match(text)
            if not m:
                continue
            n, lu = int(m.group(1)), last_u_epoch(m.group(2))
            d["ticks"] += 1
            if prev:
                dt_host, dn = t - prev[0], n - prev[1]
                if dt_host > 5:
                    d["gaps"] += 1
                elif abs(dn - dt_host) > 1.5:
                    d["jumps"] += 1
                elif dn == 1 and 0.85 < dt_host < 1.15:
                    key = (int((t + IST_S) // 3600) * 3600 - IST_S, lu)
                    h = hours.setdefault(key, [-1e18, 0, 0.0])
                    h[1] += 1
                    if n - t > h[0]:
                        h[0], h[2] = n - t, t
            prev = (t, n)

pts = sorted((k[0], k[1], v[0], v[1], v[2]) for k, v in hours.items() if v[1] >= 600)

with open(os.path.join(outdir, "hourly.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["hour_ist", "samples", "rtc_minus_pc_ms", "last_u_ist", "hours_since_u", "ppm_since_u"])
    for hs, lu, off, n, tb in pts:
        since = (tb - lu) / 3600 if lu else None
        w.writerow([ist(hs), n, f"{off * 1000:.1f}", ist(lu, "%d/%m/%Y %H:%M:%S") if lu else "",
                    f"{since:.2f}" if since else "",
                    f"{off * 1e6 / (tb - lu):.2f}" if lu and tb - lu > 3600 else ""])

mpath = os.path.join(outdir, "manual_checks.csv")
rows = {}
if os.path.exists(mpath):
    with open(mpath, newline="") as f:
        for r in csv.reader(f):
            if r and r[0] != "host_time":
                rows[r[0]] = r
for r in manual:
    rows[r[0]] = [str(x) for x in r]
with open(mpath, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["host_time", "rtc_minus_ntp_s", "hours_since_u", "ppm_since_u"])
    for k in sorted(rows):
        w.writerow(rows[k])


def fit(points):
    n = len(points)
    if n < 2:
        return None
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points)
    return sum((p[0] - mx) * (p[1] - my) for p in points) / sxx if sxx else None


out = []
out.append("# Daily RTC log\n")
out.append("Generated by `scripts/daily_summary.py` from the raw serial logs "
           f"(updated {dt.datetime.now(IST).strftime('%d/%m/%Y %H:%M')} IST).\n")
out.append("**Reference:** the PC clock, NTP-disciplined. **RTC − PC** is taken once per hour "
           "from the `[RTC ]` print closest to the RTC second edge; it carries a constant "
           "bias of a few ms (USB latency, 100 ms poll), which cancels in the drift rates. "
           "**ppm since U** assumes the RTC was exact at the last `U`. "
           f"Expected rate from the 27 h measurement: {NOMINAL_PPM:+.1f} ppm "
           f"({NOMINAL_PPM * 0.0864:+.2f} s/day). Crystal limit: ±20 ppm.\n")
out.append("| Date (IST) | Logged | RTC lines | Jumps | Flags / read errors | ESP boots / stalls "
           "| RTC − PC at end | Rate that day | Since last U | Rate since U |")
out.append("|---|---|---|---|---|---|---|---|---|---|")
for day in sorted(days):
    d = days[day]
    day_pts = [p for p in pts if dt.datetime.fromtimestamp(p[0], IST).date() == day]
    if day_pts:
        lu = day_pts[-1][1]
        seg = [(p[4], p[2]) for p in day_pts if p[1] == lu]
        end_off = seg[-1][1]
        slope = fit(seg) if seg[-1][0] - seg[0][0] >= 6 * 3600 else None
        rate_day = f"{slope * 1e6:+.1f} ppm ({slope * 86400:+.2f} s/day)" if slope else "< 6 h, n/a"
        since = seg[-1][0] - lu if lu else None
        since_s = f"{since / 3600:.1f} h" if since else "—"
        rate_u = f"{end_off * 1e6 / since:+.1f} ppm" if since and since > 6 * 3600 else "—"
        end_s = f"{end_off:+.3f} s"
    else:
        end_s = rate_day = since_s = rate_u = "—"
    logged = f"{(d['last'] - d['first']) / 3600:.1f} h"
    out.append(f"| {day.strftime('%d/%m/%Y')} | {logged} | {d['ticks']} | {d['jumps']} "
               f"| {d['flags']} / {d['errors']} | {d['boots']} / {d['resets']} "
               f"| {end_s} | {rate_day} | {since_s} | {rate_u} |")

out.append("\n**Columns.** *Jumps*: consecutive RTC prints whose second count disagrees with "
           "the PC by more than 1.5 s (the RTC skipped or repeated time). *Flags*: `[RTC*]` "
           "lines (halted, not ticking, not set). *ESP stalls*: the console went quiet for "
           "30 s and the logger reset the ESP32 (the RTC is not reset). "
           "Hourly values: [hourly.csv](hourly.csv).\n")

if rows:
    out.append("## `C` checks (firmware, NTP, 1 s resolution)\n")
    out.append("| Host time | RTC − NTP | Since last U | Rate since U |")
    out.append("|---|---|---|---|")
    for k in sorted(rows):
        r = rows[k]
        out.append(f"| {k[:16].replace('T', ' ')} | {int(r[1]):+d} s | {r[2] or '—'} h "
                   f"| {(r[3] + ' ppm') if r[3] else '—'} |")
    out.append("")

with open(os.path.join(outdir, "DAILY.md"), "w") as f:
    f.write("\n".join(out))
print("\n".join(out))
