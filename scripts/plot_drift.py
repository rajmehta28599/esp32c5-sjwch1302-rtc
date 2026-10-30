#!/usr/bin/env python3
"""Drift chart from results/daily/hourly.csv (scripts/daily_summary.py).

    scripts/plot_drift.py [CSV] [OUTDIR] [--u "DD/MM/YYYY HH:MM:SS"]
                          [--gap LABEL ...] [--social]

Plots RTC - PC against hours since one `U` (default: the run with the most
hours), with a least-squares line and the crystal's +-20 ppm envelope.
Gaps of more than 1.5 h between hourly points are shaded; --gap labels
them in order. Writes OUTDIR/drift-light.svg and OUTDIR/drift-dark.svg
(defaults: results/daily/hourly.csv, docs/images), and with --social a
1280x640 OUTDIR/social-preview.png for the GitHub repository card.

Requires matplotlib and numpy.
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

SPEC_PPM = 20.0  # crystal tolerance at 25 C, docs/HARDWARE.md
GAP_H = 1.5

THEMES = {
    "light": dict(text="#0b0b0b", muted="#52514e", grid="#e4e3df", band="#8a8985",
                  gap="#f0efec", series="#2a78d6", surface="#ffffff"),
    "dark": dict(text="#f0efec", muted="#c3c2b7", grid="#383835", band="#9a9994",
                 gap="#262624", series="#3987e5", surface="#0d1117"),
}

args = sys.argv[1:]


def opt(name, n=1):
    if name not in args:
        return None
    i = args.index(name)
    vals = args[i + 1:i + 1 + n] if n else []
    del args[i:i + 1 + n]
    return vals


social = opt("--social", 0) is not None
u_sel = opt("--u")
gap_labels = []
while "--gap" in args:
    gap_labels += opt("--gap")
csv_path = args[0] if len(args) > 0 else os.path.join("results", "daily", "hourly.csv")
outdir = args[1] if len(args) > 1 else os.path.join("docs", "images")
os.makedirs(outdir, exist_ok=True)

runs = {}
for r in csv.DictReader(open(csv_path)):
    if r["hours_since_u"]:
        runs.setdefault(r["last_u_ist"], []).append(
            (float(r["hours_since_u"]), float(r["rtc_minus_pc_ms"]) / 1000))
u = u_sel[0] if u_sel else max(runs, key=lambda k: max(h for h, _ in runs[k]))
pts = sorted(runs[u])
h = np.array([p[0] for p in pts])
y = np.array([p[1] for p in pts])
slope, icept = np.polyfit(h, y, 1)
ppm = slope / 3600 * 1e6
rms_ms = np.sqrt(np.mean((y - (slope * h + icept)) ** 2)) * 1000
gaps = [(h[i], h[i + 1]) for i in range(len(h) - 1) if h[i + 1] - h[i] > GAP_H]
xmax = h[-1] * 1.04
print(f"U {u}: {len(h)} points, {h[-1]:.2f} h, fit {ppm:+.2f} ppm "
      f"({slope * 24:+.2f} s/day), intercept {icept * 1000:+.0f} ms, rms {rms_ms:.0f} ms")


def num(v, nd=2):  # signed, with a real minus sign
    return f"{v:+.{nd}f}".replace("-", "\u2212")


def draw(ax, c, big=False):
    fs = 15 if big else 10
    xs = np.array([0, xmax])
    spec = SPEC_PPM * 1e-6 * 3600 * xs
    ax.fill_between(xs, -spec, spec, color=c["band"], alpha=0.18, lw=0)
    ax.axhline(0, color=c["muted"], lw=0.8)
    for i, (a, b) in enumerate(gaps):
        ax.axvspan(a, b, color=c["gap"], lw=0, zorder=0)
        if i < len(gap_labels):
            ax.text((a + b) / 2, -1.2, gap_labels[i], ha="center", va="top",
                    color=c["muted"], fontsize=fs * 0.85, wrap=True)
    ax.plot(xs, slope * xs + icept, color=c["series"], lw=2, ls=(0, (4, 3)), alpha=0.7)
    ax.plot(h, y, "o", color=c["series"], ms=8 if big else 6.5,
            mec=c["surface"], mew=1.5, zorder=3)
    ax.annotate(f"{num(y[-1])} s after {h[-1]:.1f} h",
                (h[-1], y[-1]), xytext=(-10, -4), textcoords="offset points",
                ha="right", va="top", color=c["text"], fontsize=fs)
    ax.text(xmax * 0.015, slope * xmax * 0.62,
            f"least-squares fit: {num(ppm, 1)} ppm\n({num(slope * 24)} s/day, {len(h)} hourly points)",
            ha="left", va="top", color=c["series"], fontsize=fs)
    ax.text(xmax * 0.02, SPEC_PPM * 1e-6 * 3600 * xmax * 0.02 + 0.5,
            f"crystal tolerance ±{SPEC_PPM:.0f} ppm", ha="left", va="bottom",
            color=c["muted"], fontsize=fs)
    ax.set_xlim(0, xmax)
    ax.set_ylim(min(y.min(), slope * xmax) * 1.12, SPEC_PPM * 1e-6 * 3600 * xmax * 1.6 + 1)
    ax.set_xlabel("hours since last U (RTC set from NTP)", color=c["muted"], fontsize=fs)
    ax.set_ylabel("RTC − PC clock (s)", color=c["muted"], fontsize=fs)
    ax.grid(True, color=c["grid"], lw=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors=c["muted"], labelsize=fs * 0.9, length=0)
    for s in ax.spines.values():
        s.set_visible(False)


plt.rcParams["svg.fonttype"] = "path"
for name, c in THEMES.items():
    fig, ax = plt.subplots(figsize=(9, 4.2))
    fig.patch.set_alpha(0)
    ax.set_facecolor("none")
    draw(ax, c)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, f"drift-{name}.svg"), transparent=True)
    plt.close(fig)

if social:
    c = THEMES["dark"]
    fig = plt.figure(figsize=(12.8, 6.4), dpi=100, facecolor=c["surface"])
    fig.text(0.05, 0.9, "esp32c5-sjwch1302-rtc", color=c["text"], fontsize=30, weight="bold")
    fig.text(0.05, 0.83, "DS1302-compatible RTC on ESP32-C5: measured drift, crystal load, bus timing",
             color=c["muted"], fontsize=16)
    ax = fig.add_axes([0.08, 0.13, 0.88, 0.62])
    ax.set_facecolor(c["surface"])
    draw(ax, c, big=True)
    fig.savefig(os.path.join(outdir, "social-preview.png"), facecolor=c["surface"])
    plt.close(fig)
