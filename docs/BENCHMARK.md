# Benchmark method

What each test measures, how it is measured, and the pass limit. Measured
results for the reference board are in [`../results/RESULTS.md`](../results/RESULTS.md).

Every figure in the results is labelled as **measured** (on the board),
**datasheet** (quoted from a datasheet), or **calculated** (derived from
datasheet values). Nothing is estimated without one of those labels.

## Time references

| Reference | Used for | Resolution / accuracy |
|---|---|---|
| ESP32-C5 crystal (`esp_timer`, 1 µs) | edge timing, short-term rate | 1 µs; its own frequency error is measured against NTP in the accuracy test |
| NTP (own SNTP client, best of 4 per burst) | absolute time, absolute rate | offset error ≤ round-trip/2, typically 5–7 ms on WiFi |
| RTC seconds-register edge | the device under test | ±37 µs capture window (75 µs per register read) |

The RTC exposes only whole seconds, but the moment a second starts can be timed.
`captureEdge()` polls the seconds register and timestamps the change on the
ESP32 timer. Fitting a straight line through many edges gives the rate with
far better than 1 s resolution: 120 edges give about ±0.1 ppm.

## Suite `B`: non-destructive (~16 min)

The RTC time is never written.

### Offline (radio off)

| Step | Method | Pass limit |
|---|---|---|
| Register sanity | Clock burst read: CH = 0, 24 h mode, all fields valid BCD in range, WP bit only, TCS decoded | all valid |
| Write protect | Set WP = 1 and try a RAM write (must be blocked). Set WP = 0 and retry (must work). RAM value restored. | both behaviours correct |
| Bus integrity | 10 rounds × 20 patterns (00, FF, 55, AA, walking 1, walking 0). Each pattern XOR address is written to all 31 RAM bytes and read back, so address aliasing shows up. Then 10 random 31-byte RAM bursts, 3000 clock burst reads checked for valid BCD and time continuity, and the transfer time. RAM is saved and restored. | 0 errors |
| RAM alias probe | For each address: write a background to all 31 bytes, read it back, write one byte, read all 31 again. Reports bytes that do not store the value and bytes that changed with it. | all 31 independent |
| Tick test | 120 consecutive second edges timed on the ESP32 crystal; line fit of edge time vs second count | 0 missed ticks, max residual ≤ 1000 µs |

The tick test reports:
- rate vs the ESP32 crystal (ppm, ±2σ)
- jitter (rms and maximum residual)
- shortest and longest second

### Online + RF stress
WiFi connected, power save off. A separate task sends 200 UDP packets of
1200 B per second to the gateway's discard port, keeping the radio
transmitting next to the RTC crystal. The bus integrity and tick tests are
repeated under this load.

| Result | Pass limit |
|---|---|
| Bus integrity under RF | 0 errors |
| Tick test under RF | 0 missed ticks, max residual ≤ 1000 µs |
| Rate shift (RF − offline) | ≤ ±2 ppm |

The rate shift is RTC and ESP32 crystal together. WiFi TX warms the module,
which also moves the ESP32 crystal, so a shift within the limit means "no
measurable RF effect", not "zero effect". Jitter under RF includes interrupt
latency in the capture loop, so it is an upper bound for the RTC.

### Online accuracy vs NTP (10 min)
- Every RTC edge is timed on the ESP32 crystal.
- Every 30 s an NTP burst is taken (best of 4 by round-trip delay). Samples
  with more than twice the best delay are discarded.
- A line fit of NTP offset vs ESP32 time maps ESP32 time to UTC. This
  measures the ESP32 crystal error instead of assuming it.
- RTC − UTC is computed at every edge, and its slope is the RTC rate error
  in ppm, quoted with ±2σ.
- Cross-check: (RTC vs ESP32 crystal) + (ESP32 crystal vs NTP) must agree
  with the direct result.

| Result | Pass limit |
|---|---|
| RTC rate error | ≤ ±20 ppm (crystal frequency tolerance at 25 °C) |

**Limitation, measured on the reference board.** Over 10 minutes the
WiFi-NTP offsets wander by a few ms (fit rms 3 ms). Two 10-minute runs put the
ESP32 crystal at different values (see results), while the RTC-vs-ESP32 rate
repeated to ±1 ppm. The quoted ±2σ assumes white noise and is optimistic.
Treat the 10-minute number as a quick screen (is the error tens of ppm or
not?) and use `C` over ≥ 24 h for the absolute rate.

`C` gives the long-term view: the phase measured to the millisecond against
NTP, compared with the phase stored at the last `U` (in RTC RAM). Over days
this resolves the rate to well under 0.1 ppm, power cycles included.

## Command `T`: bus only
The bus integrity test and RAM alias probe on their own (~2 s), with WiFi as it is.

## Suite `D`: destructive (~1 min, confirm with `Y`)
Runs offline.

1. **Reference.** 10 edges are fitted, so that the RTC's own time line
   (epoch + rate against the ESP32 crystal) is known to the microsecond.
2. **Oscillator start-up, 5 runs.** Set CH = 1 (oscillator stops), wait
   1.5 s, write CH = 0 (restart, countdown reset at CE fall), time the first
   tick. Start-up = first tick − one RTC second.
   - Limit: ≤ 1000 ms (DS1302 datasheet: "usually less than one second").
   - Run *halted: yes* confirms the oscillator really stopped.
3. **Calendar rollovers, 10 cases.** Each is written at hh:mm:58 as a raw
   8-byte burst; the registers are compared after two ticks. Cases:
   midnight with day-of-week 7→1, hour carry, 30-day month, Feb non-leap
   2027, Feb 28→29 2028, Feb 29→Mar 1 2028, year end, register wrap 99→00,
   and the 12 h AM→PM and PM→AM (next day) transitions.
   - Limit: all cases match.
4. **Restore.** The RTC is rewritten so CE falls on its own next
   second boundary from step 1. The first tick after the write is compared
   with the reference.
   - Limit: ≤ ±2 ms. Within that, the last-`U` drift record stays valid.

## Soak (normal mode)
In normal mode the firmware polls the RTC every 100 ms and counts the following.
A `[STAT]` line every 60 s reports them:

| Counter | Meaning |
|---|---|
| `rd_err` | burst read failed or gave invalid data |
| `jumps` | consecutive printed seconds differ by ≠ 1 |
| `stalls` | no tick for > 1.1 s |
| `glitch` | implausible seconds value during edge capture |

`[STAT]` also shows heap and die temperature. Run it for 24 h+ with WiFi off
(default), and again with `W` (WiFi on), for an offline/online soak.

## Worst-case conditions that need extra equipment
The firmware supports these, but the bench setup used for the results could not create
the conditions. The *calculated* expectations are in `results/RESULTS.md`.

| Condition | Procedure |
|---|---|
| Backup power only | `U`, then unplug the board for ≥ 24 h, plug in, `C`. The ppm since `U` covers the time on battery. Compare with a run that stayed powered. |
| Low backup voltage | Replace the cell with a bench supply at 2.0 V (DS1302 minimum) on VCC1, unplug USB for an hour, then `C`. Repeat at 1.8 V to find where timekeeping stops (`[RTC*] HALTED`, or garbage). |
| Temperature 0 °C / 70 °C | Board in a chamber: `C` at 25 °C, hold ≥ 2 h at the extreme, `C` again; the difference in ppm/h is the temperature effect. `[STAT]` logs the ESP32 die temperature as a proxy. |
| Oscillation margin | With power off, add a series resistor in the crystal loop. Increase it until `D` start-up fails. Margin = (R + ESR) / ESR, aim ≥ 3. (This crystal's ESR is 70 kΩ max, above the DS1302's 45 kΩ spec.) |
| Backup current | µA meter in series with the cell, USB unplugged. |
