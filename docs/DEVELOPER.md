# Developer guide

How the firmware is put together, how to build it, and how to extend it.

## Toolchain

| Item | Version used |
|---|---|
| Arduino-ESP32 core | 3.3.8 (any 3.3.x should work) |
| arduino-cli | 1.x |
| Board | ESP32C5 Dev Module |
| Libraries | none beyond the core |

Board options that matter:

| Option | Value | Why |
|---|---|---|
| USB CDC On Boot | **Disabled** (`CDCOnBoot=default`) | The console uses the ESP-IDF USB-Serial-JTAG driver. With CDC enabled, Arduino's HWCDC was seen to stop sending after a few seconds on the ESP32-C5. |
| PSRAM | Disabled | Not used. |
| Partition scheme | Default | The app is ~1 MB. |

FQBN: `esp32:esp32:esp32c5:CDCOnBoot=default,PSRAM=disabled`

```bash
cp firmware/RTC_Bench/credentials_example.h firmware/RTC_Bench/credentials.h   # set WiFi
scripts/build.sh                  # build only
scripts/build.sh /dev/ttyACM0     # build + flash
```

From the Arduino IDE: open `firmware/RTC_Bench/RTC_Bench.ino`, set the options above, upload.

### ESP-IDF project (`firmware/RTC_Test_IDF/`)

| Item | Value |
|---|---|
| ESP-IDF | v6.1 |
| Target | esp32c5 (set in `sdkconfig.defaults`) |
| Partition table | single factory app, large (1.5 MB) |
| Console | ESP-IDF log on UART0 at WARN level; application output on USB-Serial-JTAG through the driver, 50 ms write timeout |
| WiFi credentials | `menuconfig` > *RTC test*, or `sdkconfig.defaults.local` (git-ignored; see `.example`) |

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh      # or your export.sh
cd firmware/RTC_Test_IDF
idf.py build flash monitor
```

`sdkconfig.defaults` and `sdkconfig.defaults.local` apply only when no
`sdkconfig` exists. Delete `sdkconfig` after editing them.

| File | Contents |
|---|---|
| `main/main.c` | console, commands, WiFi STA, SNTP, 1 Hz RTC print loop |
| `main/ds1302.c`, `ds1302.h` | DS1302 driver: one-time `gpio_config()`, `gpio_set_direction()` for the IO turnaround, 2.0 V worst-case timing |
| `main/Kconfig.projbuild` | `CONFIG_RTC_WIFI_SSID`, `CONFIG_RTC_WIFI_PASSWORD` |

The RTC RAM layout (bytes 0..4: last `U`) matches the Arduino firmware, so
both read the same record.

## Source layout

```
firmware/RTC_Bench/
├── RTC_Bench.ino           application: console, commands, benchmarks
├── ds1302.h                DS1302 3-wire driver (header only)
├── bench_types.h           structs shared by the .ino (see "Arduino prototypes")
├── credentials_example.h   template, committed
└── credentials.h           your WiFi, git-ignored
scripts/
├── build.sh                arduino-cli build / flash
└── serial_log.py           timestamped logger + command injection
```

### RTC_Bench.ino sections

| Section | Contents |
|---|---|
| Configuration | pins, time zone, NTP host, test lengths, pass limits, RTC RAM map |
| `UsbJtagConsole` | `Stream` over the ESP-IDF USB-Serial-JTAG driver, 50 ms TX timeout so output never blocks without a host |
| RTC helpers | epoch conversion (`daysFromCivil`, TZ independent), init, boot RAM check, last-`U` record |
| Edge capture | `captureEdge()`, `waitUntil()`, `clockBurstUs()`, `linfit()` |
| WiFi + NTP | `netUp()` / `netDown()`, own SNTP client (`ntpQuery`, `ntpBurst`) |
| Phase | `measurePhase()`, `reportPhase()`, commands `C` and `U` |
| Benchmark blocks | bus, register, write-protect, tick, RF stress task, accuracy |
| `B` / `D` | suites and summaries |
| `setup()` / `loop()` | boot checks; 1 Hz RTC print, soak counters, `[STAT]` |

## Key design decisions

### Own DS1302 driver instead of a library
`pinMode()` on Arduino-ESP32 3.3.x calls `gpio_config()`, which calls
`rtc_gpio_deinit()` on LP-capable pins. On the ESP32-C5, GPIO0..6 are LP-capable,
and that call measures **12.2 ms** (command `E`). A driver that switches the
bidirectional IO line with `pinMode()`, as ErriezDS1302 2.0.0 does, therefore
spends ~24.4 ms on every transfer. `ds1302.h` configures the pins once with
`gpio_config()` and then turns IO around with `gpio_set_direction()`
(5.6 µs for the pair). A 1-byte register read takes 75 µs.

Bus timing uses the DS1302 limits at VCC = 2.0 V, the slowest case: 4 µs CE
setup and inactive time, 1 µs CLK high/low. That keeps a margin at 3.3 V and
still works if the RTC rail sags.

### The RTC holds UTC
The registers hold UTC in 24 h mode. Local time (`TZ_OFFSET_SEC`, `TZ_LABEL`) is
applied only when printing. Day-of-week register 1..7 = Sunday..Saturday, the
same convention as ErriezDS1302.

### Only `U` writes the time from the network
`C`, `B` and the soak counters only read. `D` rewrites the calendar, but
restores the time from its own timing reference, so the drift record stays valid.

### Own SNTP client
The ESP-IDF SNTP service sets the system clock. The benchmark needs the raw
NTP offset and round-trip delay for every exchange, so `ntpQuery()` speaks
NTPv4 over `WiFiUDP` directly:

- `offset = ((T2 - t1) + (T3 - t4)) / 2`, `delay = (t4 - t1) - (T3 - T2)`,
  with local times taken from `esp_timer`.
- A random originate timestamp guards against stale replies.
- A burst of `NTP_BURST` queries keeps the one with the lowest delay. Its
  offset error is bounded by delay/2.
- WiFi modem sleep is disabled while online, because it adds up to one
  beacon interval to the round trip.

### Timing an RTC second edge
The DS1302 copies the time into its read buffer on the rising edge of CE.
`captureEdge()` polls the seconds register. The edge lies between the start of
the last read that saw the old value and the start of the first read that saw
the new one. It reports the midpoint as the edge time and the half-width as its
uncertainty (typically ~37 µs). With a predicted edge time, the task sleeps until
20 ms before the edge and then busy-polls, so the CPU stays free between ticks.

### Writing on a boundary
The datasheet says: "The countdown chain is reset whenever the seconds register
is written. Write transfers occur on the falling edge of CE." `U` and the `D`
restore therefore start the 8-byte clock burst early by the measured length of
a burst transfer (`clockBurstUs()`). CE then falls on the target instant, and
the next tick lands exactly one second later.

### Arduino prototypes
The Arduino builder inserts function prototypes before the first function
in the `.ino`. Structs used in function signatures must be declared before
that point, which is why they live in `bench_types.h`.

## RTC RAM map

The 31 bytes of RTC RAM are battery-backed, so the drift record survives power loss.

| Address | Content |
|---|---|
| 0..3 | epoch (UTC, little endian) of the last `U` |
| 4 | `0xC5` = bytes 0..3 valid |
| 5..7, 9 | free |
| 8 | **not used**: on the SJWCH1302Q-W, writes are ignored and reads return byte 7 |
| 10..13 | RTC − UTC in µs measured right after the `U` (int32) |
| 14 | `0xB5` = bytes 10..13 valid |
| 15..29 | free |
| 30 | boot RAM test (value saved and restored) |

The bus test in `B` overwrites the whole RAM. It saves and restores all 31
bytes and verifies the restore.

## Serial protocol

Single characters at 115200 baud: `U C B D T E W S R L X H` (see `H`). Output
lines start with a tag so logs can be filtered:

| Tag | Meaning |
|---|---|
| `[RTC ]` | once per second: epoch, local time, last update |
| `[RTC*]` | the same, but flagged: halted, not ticking, or not set |
| `[STAT]` | every 60 s: uptime, heap, soak counters, die temperature |
| `[CHECK]` | `C` / `U` precision measurement |
| `[REG] [WP ] [BUS] [TICK] [ACC] [OSC] [CAL] [DSTR]` | benchmark steps |
| `[RESULT]` | suite summaries (`grep '\[RESULT\]'`) |

## Adding a test

1. Put any struct it returns in `bench_types.h`.
2. Write the test as a function that fills that struct and prints one tagged
   line. Call `abortRequested()` in long loops.
3. Call it from `cmdBenchmark()`, or from `cmdDestructive()` if it writes the
   clock, and add a `[RESULT]` line with a PASS/FAIL limit from the
   *Pass limits* block.
4. Document the method and limit in `docs/BENCHMARK.md`.

## ESP32-C5 rules followed

- `esp_wifi_set_protocols()` is called after every `WiFi.mode()` that enables STA.
- One core: tasks use `xTaskCreate()` (not pinned). Handles start as `NULL`
  and the return value is checked.
- `WiFi.RSSI()` is read only while connected.
