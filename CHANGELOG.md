# Changelog

## 1.2.0 — 2026-10-28

- Daily log: `scripts/serial_log.py` writes one log per day when given a
  directory, stamps each line with the full host date and time when its first
  byte arrives, resets the ESP32 through RTS if the console is silent for
  30 s, and with `--summary` refreshes the daily summary every hour.
- `scripts/daily_summary.py`: per-day table (`results/daily/DAILY.md`) and
  hourly RTC − PC points (`hourly.csv`), using the NTP-disciplined PC clock as
  reference. Also collects `C` results (`manual_checks.csv`).

## 1.1.0 — 2026-10-27

- `firmware/RTC_Test_IDF/`: RTC test for ESP-IDF v6.1. It prints the RTC
  time once per second and supports `U`, `C`, `R`, `L` and `H`. It uses a C
  DS1302 driver with the same bus timing as `ds1302.h`, a USB-Serial-JTAG
  console with a 50 ms write timeout, and `esp_netif_sntp` for NTP. WiFi runs
  only during `U` and `C`. The WiFi network and password come from
  `menuconfig` or a git-ignored `sdkconfig.defaults.local`.

## 1.0.0 — 2026-10-27

First release.

- Normal mode: offline RTC clock, printed once per second as Unix epoch and
  local time, with the time since the last network update.
- `U`: set the RTC from NTP, written so CE falls on the UTC second, then
  verified on the first tick. The RTC − UTC phase is stored in battery-backed
  RTC RAM.
- `C`: RTC − UTC at millisecond resolution, plus drift since the last `U` in ppm.
- `B`: non-destructive suite: register sanity, write protect, bus
  integrity, tick stability offline and under WiFi RF stress, and a 10-minute
  absolute accuracy test against NTP.
- `D`: destructive suite: oscillator start-up time and ten calendar rollover
  cases, with the RTC time restored to sub-millisecond afterwards.
- `T`, `E`, `S`, `R`, `L`, `W`: bus test, bus timing, status, register dump,
  last update, WiFi keep-alive.
- Own DS1302 driver (`ds1302.h`) with worst-case datasheet timing. It turns
  the IO line around with `gpio_set_direction()`, because `pinMode()` costs
  12.2 ms per call on ESP32-C5 LP-capable pins.
- Own SNTP client that reports the raw offset and round-trip delay.
