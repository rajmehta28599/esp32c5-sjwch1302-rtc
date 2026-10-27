# esp32c5-sjwch1302-rtc-bench

Bench firmware and measured results for a **DS1302-compatible RTC
(SJWCH1302Q-W)** with a **32.768 kHz crystal (3215, ±20 ppm, CL 12.5 pF)** on
an **ESP32-C5**.

The firmware runs the board as an offline clock and adds tools to qualify the
RTC and crystal. It measures the RTC rate against the ESP32 crystal and NTP to
fractions of a ppm, and checks the chip against the DS1302 datasheet.

## Results on the reference board

| Test | Result |
|---|---|
| RTC accuracy (27 h vs NTP, ms readout) | **−78.0 ± 1.1 ppm, −6.74 s/day: FAIL** (crystal limit ±20 ppm) |
| Ticks (timed seconds, offline and under WiFi TX) | 0 missed; offline jitter 21 µs rms, 38 µs max |
| WiFi RF stress effect on rate | +1.2 ppm (limit ±2): no measurable effect |
| Oscillator start-up after halt | 103–115 ms (5 runs; datasheet: < 1 s) |
| Calendar rollovers (leap years, month ends, 12 h, 99→00) | 10/10 pass |
| Countdown reset on seconds write / time restore | +29 µs |
| Write protect, register map, trickle default (0x5C) | DS1302-identical |
| Battery-backed RAM | **30 usable bytes, not 31: RAM 8 reads RAM 7, writes to it are lost** |
| Bus (6200 RAM ops, 6000 clock reads, offline and RF) | clean except RAM 8 |

The slow clock is consistent with **2–4× too much load capacitance on the
crystal** (calculated). A genuine DS1302 with the same crystal would run fast.
See [docs/HARDWARE.md](docs/HARDWARE.md) for the analysis, the DS1302Z+
comparison table and recommendations, and [results/RESULTS.md](results/RESULTS.md)
for every number with its source.

A side finding: on Arduino-ESP32 3.3.x, `pinMode()` on ESP32-C5 GPIO0..6
takes **12.2 ms**, because `gpio_config()` calls `rtc_gpio_deinit()`. A
DS1302 driver that flips the IO line with `pinMode()` (such as ErriezDS1302)
blocks ~24.5 ms per transfer. This repo's driver uses `gpio_set_direction()`
and needs 75 µs per register read.

## Quick start

```bash
cp firmware/RTC_Bench/credentials_example.h firmware/RTC_Bench/credentials.h
# edit WIFI_SSID / WIFI_PASS (used only for NTP)
scripts/build.sh /dev/ttyACM0              # build + flash (arduino-cli)
arduino-cli monitor -p /dev/ttyACM0 --config baudrate=115200
```

Arduino IDE: open `firmware/RTC_Bench/RTC_Bench.ino`, board *ESP32C5 Dev
Module*, **USB CDC On Boot: Disabled**, upload, Serial Monitor at 115200.

### ESP-IDF version

`firmware/RTC_Test_IDF/` is a plain RTC test for **ESP-IDF v6.1** (no
Arduino). It prints the RTC time once per second and keeps the last-update
record in the same RTC RAM bytes as the Arduino firmware. It has the commands
`U` (set from NTP), `C` (drift check, 1 s resolution), `R` (register/RAM dump),
`L` (last update) and `H` (help). The benchmark suites are in the Arduino
firmware only.

```bash
cd firmware/RTC_Test_IDF
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # set WiFi (or use idf.py menuconfig > RTC test)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor                           # Ctrl+] exits the monitor
```

Close any other serial monitor first: only one program can open the port.

## Commands

| Key | Action |
|---|---|
| `U` | Set the RTC from NTP, written on the exact UTC second. **The only command that changes the time from the network.** |
| `C` | RTC − UTC to the millisecond, plus drift in ppm since the last `U` (survives power loss) |
| `B` | Benchmark suite, non-destructive, offline + online (~16 min) |
| `D` | Destructive suite: oscillator start-up, calendar rollovers; time restored (~1 min, confirm `Y`) |
| `T` | Bus integrity + RAM alias probe (~2 s) |
| `E` | Bus timing and the `pinMode()` cost on this chip |
| `W` | Keep WiFi on between commands (default: off, offline) |
| `S` | Status and soak counters |
| `R` | Raw register and RAM dump |
| `L` | When the RTC was last updated |
| `X` | Abort a running benchmark |
| `H` | Help |

Normal output, once per second, read only from the RTC:

```
[RTC ] Unix: 1793091390 | IST: 27/10/2026 14:26:30 | Last U: 26/10/2026 11:06:17 (1d 3h 20m ago)
[STAT] up 0d 00:14:35 | WiFi off | heap 219540 (min 111644) | rd_err 0 | jumps 0 | stalls 0 | glitch 0 | die 33.2 C
```

## Repository

```
firmware/RTC_Bench/   Arduino bench sketch, DS1302 driver, credentials template
firmware/RTC_Test_IDF/ ESP-IDF v6.1 RTC test, DS1302 driver in C
scripts/              build/flash, timestamped serial logger
docs/HARDWARE.md      wiring, crystal data, DS1302Z+ comparison, load-capacitance analysis
docs/BENCHMARK.md     test method and pass limits
docs/DEVELOPER.md     architecture, design decisions, extending the tests
results/              measured results and logs from the reference board
```

## License

MIT, see [LICENSE](LICENSE).
