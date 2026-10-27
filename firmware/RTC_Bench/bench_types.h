// Data types shared by RTC_Bench.ino. Kept in a header so the Arduino
// builder's generated function prototypes can see them.
#pragma once
#include <stdint.h>

struct Edge {
  int64_t t;
  int32_t unc;
  uint8_t sec;
};

struct Fit {
  double a, b, rms, maxAbs, seB;
  int n;
};

struct NtpSample {
  int64_t t;
  int64_t off;
  int32_t delay;
};

struct Phase {
  uint32_t epoch;  // RTC value right after the edge
  int64_t utcUs;   // true UTC of the edge
  int64_t errUs;   // RTC - UTC
  int32_t uncUs;   // bound: edge window + NTP delay/2
  int32_t delayUs; // NTP round-trip delay used
};

struct BusResult {
  uint32_t ramOps, ramErr, burstOps, burstErr, clkReads, clkErr;
  uint32_t errAddrMask, burstAddrMask; // bit n = RAM address n failed
  uint8_t errBitsOr, firstAddr, firstExp, firstGot;
  float usPerRead;
};

struct TickResult {
  int ticks, missed, noisy, fails;
  double usPerSec, ppmVsXtal, seppm, rmsUs, maxAbsUs, minIntUs, maxIntUs;
  bool valid;
};

struct AccResult {
  int ticks, ntpUsed, ntpTotal, missed, noisy;
  double minutes, rtcPpm, rtcPpmUnc, xtalPpm, relPpm, phaseEndMs, ntpRmsUs;
  int32_t bestDelayUs;
  bool valid;
};

struct RtcRef {
  uint32_t e0;
  double a, b;
};

struct RollCase {
  const char *name;
  uint8_t set[7]; // sec, min, hour, date, month, day, year (BCD, raw)
  uint8_t exp[7];
};
