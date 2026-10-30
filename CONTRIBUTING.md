# Contributing

## Measurements

Every number in this repository is measured, quoted from a datasheet, or
calculated from those, and is labelled as such (**M**, **D**, **C** in
[results/RESULTS.md](results/RESULTS.md)). Please keep to that rule: write
"not published" or "not measured" rather than an estimate.

To add a result from your own board, open an issue with the
**RTC measurement** template and attach `results/daily/hourly.csv` or a log
excerpt.

## Code

- Arduino sketch: `firmware/RTC_Bench/`, ESP-IDF project:
  `firmware/RTC_Test_IDF/`. Build both before sending a change
  ([docs/DEVELOPER.md](docs/DEVELOPER.md#toolchain)).
- Keep the bus timing at or above the DS1302 datasheet worst case, and do
  not use `pinMode()` on the IO line (12.2 ms per call on ESP32-C5 GPIO0..6).
- Scripts: Python 3, standard library plus pyserial (logger) and
  matplotlib/numpy (chart only).
- Add a line under a new version at the top of [CHANGELOG.md](CHANGELOG.md).

## Never commit

WiFi names or passwords, IP or MAC addresses, or raw serial logs.
Credentials live in git-ignored files (`credentials.h`,
`sdkconfig.defaults.local`), and `logs/` is ignored.
