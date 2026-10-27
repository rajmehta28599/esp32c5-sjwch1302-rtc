# Hardware

## Board under test

| Item | Part |
|---|---|
| MCU | ESP32-C5 (WROOM-1 module, 16 MB flash) |
| RTC | SJWCH1302Q-W, SOP8, marketed as a pin-for-pin DS1302 replacement |
| Crystal | SJK-7L32768KDY2400A8: 32.768 kHz, 3215 package, ±20 ppm, CL 12.5 pF, ESR ≤ 70 kΩ |
| Backup | cell on VCC1 (trickle charger left disabled) |

### Wiring

| RTC pin | Signal | ESP32-C5 |
|---|---|---|
| 1 | VCC2 (main supply) | 3.3 V |
| 2, 3 | X1, X2 | crystal |
| 4 | GND | GND |
| 5 | CE | GPIO0 |
| 6 | I/O | GPIO4 |
| 7 | SCLK | GPIO5 |
| 8 | VCC1 (backup) | cell |

GPIO0..6 are LP-capable pins on the ESP32-C5. That matters for the driver
(see [DEVELOPER.md](DEVELOPER.md#own-ds1302-driver-instead-of-a-library)).

## Crystal data (datasheet)

| Parameter | Value |
|---|---|
| Nominal frequency | 32.768 kHz |
| Frequency tolerance at 25 °C | ±20 ppm |
| Load capacitance CL | 12.5 pF |
| ESR | 70 kΩ max |
| Shunt capacitance C0 | 1.2 pF typ |
| Motional capacitance C1 | 3.7 fF typ |
| Turnover temperature | 25 ± 5 °C |
| Temperature coefficient | −0.04 ppm/°C² max |
| Drive level | 1 µW typ |
| Aging, first year | ±3 ppm max |

## DS1302Z+ vs SJWCH1302Q-W

DS1302Z+ values are quoted from the Maxim/Analog Devices *DS1302
Trickle-Charge Timekeeping Chip* datasheet, REV 3/15. No datasheet for the
SJWCH1302Q-W could be found. Its column holds only the supplier listing and
what was **measured** on this board.

| Parameter | DS1302Z+ (datasheet) | SJWCH1302Q-W |
|---|---|---|
| Package | 8-pin SO, 150 mil | SOP8 (supplier listing) |
| Operating temperature | 0 to +70 °C (DS1302ZN+: −40 to +85 °C) | 0 to 70 °C (supplier listing) |
| Pinout | VCC2, X1, X2, GND, CE, I/O, SCLK, VCC1 | drop-in on this PCB (measured: works) |
| Supply VCC1 / VCC2 | 2.0 to 5.5 V | not published; tested at 3.3 V only |
| Supply switchover | VCC2 when VCC2 > VCC1 + 0.2 V | not published |
| Timekeeping current on backup (VCC1) | 0.3 µA max at 2.0 V, 1 µA max at 5 V | not published, not measured |
| Timekeeping current on VCC2 | 25.3 µA max at 2.0 V, 81 µA max at 5 V | not published, not measured |
| Crystal load the oscillator is designed for | 6 pF | not published; measured behaviour suggests a much larger load (see below) |
| Crystal ESR | 45 kΩ max | not published; starts reliably with a 70 kΩ-max crystal |
| Oscillator start-up | "usually less than one second" | **measured** 102.6 to 115.0 ms (5 runs, avg 108.4 ms) |
| Clock halt (CH bit) | stops the oscillator | **measured**: stops it |
| Countdown chain reset on seconds write | yes | **measured**: yes (time restore error +29 µs) |
| Interface | 3-wire, SCLK ≤ 0.5 MHz at 2 V, ≤ 2 MHz at 5 V | **measured**: works with DS1302 protocol and 2.0 V worst-case timing |
| Clock/calendar registers | sec … year, WP, trickle charger | **measured**: same map, all fields valid BCD |
| 12/24 h mode | both | **measured**: both (12 h AM→PM and PM→AM pass) |
| Leap years | valid up to 2100 | **measured**: 2027 non-leap and 2028 leap pass |
| Year register | 00–99 | **measured**: 99 → 00 wrap |
| Write protect | WP bit blocks writes | **measured**: works |
| Trickle charger register power-on value | 0x5C (disabled) | **measured**: 0x5C |
| Battery-backed RAM | **31 bytes** | **measured: 30 usable.** Writes to RAM 8 are ignored and RAM 8 reads back RAM 7. |
| Burst mode (clock and RAM) | yes | **measured**: works, except RAM byte 8 |
| Accuracy | crystal + load match + temperature | **measured with this crystal: −78.0 ± 1.1 ppm** (−6.74 s/day, over 27 h) |

### What the RAM result means
Firmware written for a DS1302 that stores data in RTC RAM byte 8 will silently
read back byte 7 instead. Avoid byte 8. This firmware keeps its records in
bytes 0..4 and 10..14.

## Why the clock runs slow: load capacitance (calculated)

A crystal oscillates at its marked frequency only when the circuit presents
exactly its rated load CL. The frequency shift for a different effective load CLx is

```
Δf/f ≈ (C1 / 2) · ( 1/(C0 + CLx) − 1/(C0 + CL) )      C1 = 3.7 fF, C0 = 1.2 pF, CL = 12.5 pF
```

| Effective load CLx | Predicted rate |
|---|---|
| 6 pF (DS1302 design value) | +122 ppm (fast) |
| 8 pF (6 pF + PCB stray) | +66 ppm (fast) |
| 12.5 pF (matched) | 0 ppm |
| 23 pF | −58 ppm |
| 31 pF | −78 ppm |
| 49 pF | −98 ppm |

The measured −78 ppm means an effective load of **≈ 31 pF** if the crystal is
exactly on frequency. The crystal's ±20 ppm tolerance widens that to
**23–49 pF**. Either way, the oscillator loads the crystal with **2 to 4 times**
its rated 12.5 pF.

A genuine DS1302 with this 12.5 pF crystal would run **fast** (+66 to +122
ppm), so the SJWCH1302's oscillator is not behaving like the DS1302's. The DS1302
datasheet also says external noise makes the clock run *fast*. A clock that runs
*slow* points at too much capacitance, not at noise. Possible sources, to check
in this order:

1. capacitors fitted from X1/X2 to ground (the DS1302 needs none)
2. larger internal load capacitance in the SJWCH1302 than in the DS1302
3. flux residue or moisture around X1/X2 (clean with IPA; no ultrasonic
   cleaning, which the crystal datasheet warns against)
4. long X1/X2 traces or a ground pour close to them

The rate is stable: −75 ppm (first 63 h), −77.4 ppm (25 h, ±1 s
readout), and −78.0 ± 1.1 ppm (27 h, millisecond readout). That points to a
fixed load error, not a marginal oscillator. Start-up is fast (≈ 110 ms), which
also argues against a weak oscillator.

## Temperature (calculated, not measured)

Tuning-fork crystals lose frequency on both sides of the turnover temperature:
`Δf/f = −0.04 ppm/°C² × (T − T0)²` with T0 = 25 ± 5 °C. The table adds that to
the measured room-temperature bias of −78 ppm.

| Temperature | Crystal alone (T0 = 25 / worst T0) | Total with −78 ppm bias | Per day |
|---|---|---|---|
| 0 °C | −25 / −36 ppm | −103 / −114 ppm | −8.9 / −9.8 s |
| 25 °C | 0 / −1 ppm | −78 / −79 ppm | −6.7 / −6.8 s |
| 40 °C | −9 / −16 ppm | −87 / −94 ppm | −7.5 / −8.1 s |
| 70 °C | −81 / −100 ppm | −159 / −178 ppm | −13.7 / −15.4 s |

Aging adds up to ±3 ppm in the first year.

## Recommendations

1. **Find the excess load.** Inspect X1/X2 for fitted capacitors and remove
   them. Then rerun `B`, or `U` followed by `C` after 24 h. A matched load
   should land within ±20 ppm.
2. If the load cannot be fixed, the bias is stable enough to **correct in
   firmware**: `t_corrected = t_rtc + (t_rtc − t_last_sync) × 78e−6`. That
   leaves the temperature term (calculated up to −36 ppm at 0 °C, −16 ppm
   at 40 °C) and aging.
3. **Do not use RTC RAM byte 8.**
4. Measure the backup current and the low-voltage limit (procedures in
   [BENCHMARK.md](BENCHMARK.md#worst-case-conditions-that-need-extra-equipment))
   before relying on the published DS1302 battery-life figures. They are not
   published for this part.
