/*
  RTC_Bench - bench firmware for a DS1302-compatible RTC (SJWCH1302Q-W)
  with a 32.768 kHz crystal (SJK-7L32768KDY2400A8, 3215, +/-20 ppm, CL 12.5 pF)
  on an ESP32-C5.

  Normal mode : WiFi OFF. Time comes only from the external RTC and is printed
                once per second (Unix epoch + local time + time of last 'U').
                A [STAT] line every 60 s reports tick anomalies and bus errors.
  'U'         : set the RTC from NTP, written on the exact UTC second.
                This is the ONLY command that changes the RTC time from the network.
  'C'         : measure RTC - UTC with millisecond resolution (RTC not written).
  'B'         : benchmark suite (non-destructive), offline + online.
  'D'         : destructive suite (oscillator start-up, calendar rollovers),
                RTC time is restored to sub-millisecond afterwards.

  Method and result interpretation: docs/BENCHMARK.md

  Board : ESP32C5 Dev Module, Arduino-ESP32 3.3.x
          Tools > USB CDC On Boot = Disabled (console uses the ESP-IDF
          USB-Serial-JTAG driver, see UsbJtagConsole)
  Libs  : none (own DS1302 driver in ds1302.h)
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <math.h>
#include <time.h>
#include <driver/usb_serial_jtag.h>
#include <driver/rtc_io.h>
#include "ds1302.h"
#include "bench_types.h"

#if __has_include("credentials.h")
#include "credentials.h"
#else
#warning "credentials.h not found: copy credentials_example.h to credentials.h and set your WiFi"
#include "credentials_example.h"
#endif

#define FW_VERSION "1.0.0"

// ---------------- Pins ----------------
#define RTC_CLK_PIN 5
#define RTC_IO_PIN  4
#define RTC_CE_PIN  0

// ---------------- Time display ----------------
#define TZ_OFFSET_SEC 19800L         // local time = UTC + 05:30
#define TZ_LABEL "IST"
#define MIN_VALID_EPOCH 1700000000UL // 14-Nov-2023, older = RTC not set

// ---------------- Network ----------------
#define NTP_HOST "pool.ntp.org"
#define NTP_PORT 123
#define NTP_LOCAL_PORT 12300
#define NTP_TIMEOUT_MS 400           // per query
#define NTP_BURST 4                  // queries per burst, lowest delay wins
const uint32_t WIFI_TIMEOUT_MS = 20000;

// ---------------- Benchmark parameters ----------------
#define TICK_TEST_S 120              // edges per tick test
#define ACCURACY_TEST_MIN 10         // NTP accuracy test length
#define NTP_INTERVAL_S 30            // NTP burst period during the accuracy test
#define BUS_ROUNDS 10                // RAM pattern rounds per bus test
#define BUS_CLOCK_READS 3000         // clock burst reads per bus test
#define STRESS_PPS 200               // RF stress: UDP packets/s to the gateway
#define STRESS_PAYLOAD 1200          // bytes per stress packet
#define STRESS_PORT 9                // discard
#define EDGE_GUARD_US 20000          // start polling this long before a predicted edge
#define EDGE_MAX_UNC_US 250          // edges with a wider capture window are not fitted
#define STARTUP_RUNS 5
#define STAT_INTERVAL_MS 60000

// ---------------- Pass limits ----------------
#define LIMIT_PPM 20.0               // crystal frequency tolerance at 25 C
#define LIMIT_JITTER_US 1000.0       // max |residual| of tick edges
#define LIMIT_RF_SHIFT_PPM 2.0       // rate change between offline and RF stress
#define LIMIT_STARTUP_MS 1000.0      // DS1302 datasheet: "usually less than one second"
#define LIMIT_RESTORE_US 2000.0      // time restore after the destructive suite

// ---------------- RTC RAM map (31 bytes, battery backed) ----------------
#define RAM_U_EPOCH 0                // 0..3  last 'U' epoch (UTC, little endian)
#define RAM_U_MAGIC 4                // 0xC5 when 0..3 valid
#define RAM_U_PHASE 10               // 10..13 RTC-UTC in us measured right after 'U'
#define RAM_P_MAGIC 14               // 0xB5 when 10..13 valid
                                     // (RAM 8 is not used: on the SJWCH1302 it mirrors RAM 7)
#define RAM_TEST_ADDR 30
#define U_MAGIC 0xC5
#define P_MAGIC 0xB5

// ---------------- Console ----------------
// Arduino's HWCDC (Serial with "USB CDC On Boot" = Enabled) was observed on
// the ESP32-C5 to stop sending after a few seconds and drop all output while
// the sketch kept running. This uses the ESP-IDF USB-Serial-JTAG driver
// instead. Build with "USB CDC On Boot" = Disabled so HWCDC is not started.
class UsbJtagConsole : public Stream {
public:
  bool begin() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK;
  }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t len) override {
    // Short timeout: never blocks the sketch when no monitor is open
    int n = usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(50));
    return n > 0 ? (size_t)n : 0;
  }
  int available() override {
    if (_peek < 0) {
      uint8_t c;
      if (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) _peek = c;
    }
    return _peek >= 0 ? 1 : 0;
  }
  int read() override {
    if (!available()) return -1;
    int c = _peek;
    _peek = -1;
    return c;
  }
  int peek() override {
    available();
    return _peek;
  }

private:
  int _peek = -1;
};
UsbJtagConsole Con;

Ds1302 rtc(RTC_CLK_PIN, RTC_IO_PIN, RTC_CE_PIN);
WiFiUDP ntpUdp;

// ---------------- Globals ----------------
bool g_rtcOk = false;
uint32_t g_syncEpoch = 0;       // last 'U' (cached copy of RTC RAM 0..4), 0 = never
uint32_t g_lastPrintedEpoch = 0;
uint32_t g_lastPollMs = 0;
uint32_t g_lastPrintMs = 0;
uint32_t g_lastStatMs = 0;
bool g_wifiKeep = false;        // 'W': keep WiFi on between commands
IPAddress g_ntpIp;
bool g_ntpResolved = false;

// Soak counters (normal mode)
uint32_t g_readErrors = 0;      // burst read failed / invalid BCD
uint32_t g_tickJumps = 0;       // RTC advanced by != 1 s between ticks
uint32_t g_tickStalls = 0;      // no tick for > 1.1 s
uint32_t g_busGlitches = 0;     // implausible seconds value during edge capture

static inline int64_t nowUs() { return esp_timer_get_time(); }

// =====================================================================
//                              RTC helpers
// =====================================================================
static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static bool bcdValid(uint8_t b, uint8_t lo, uint8_t hi) {
  if ((b & 0x0F) > 9 || (b >> 4) > 9) return false;
  uint8_t d = bcd2dec(b);
  return d >= lo && d <= hi;
}

// Days since 1970-01-01 for a civil date (TZ independent, unlike mktime)
static int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t)(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// One burst read of the clock registers -> UTC epoch. false on bus/data error.
static bool rtcReadEpoch(uint32_t *epoch) {
  struct tm dt;
  if (!rtc.read(&dt)) return false;
  *epoch = (uint32_t)daysFromCivil(dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday) * 86400UL +
           dt.tm_hour * 3600UL + dt.tm_min * 60UL + dt.tm_sec;
  return true;
}

static bool init_rtc() {
  Con.println("[RTC] I: Initializing DS1302 RTC...");
  if (rtc.begin() != true) {
    Con.println("[RTC] E: rtc.begin() failed - RTC IC not detected");
    return false;
  }
  if (!rtc.isRunning()) {
    Con.println("[RTC] W: Oscillator was HALTED (CH=1), enabling clock. Time invalid -> press 'U'");
    if (rtc.clockEnable(true) != true) {
      Con.println("[RTC] E: rtc.clockEnable() failed");
      return false;
    }
  }
  if (rtc.isRunning() != true) {
    Con.println("[RTC] E: RTC still not running after clockEnable");
    return false;
  }
  Con.println("[RTC] I: RTC initialized successfully");
  return true;
}

// Quick boot check: RAM write/read-back with 2 patterns.
// A floating/dead IO line reads 0x00 or 0xFF and fails this.
static bool rtcRamTest() {
  uint8_t saved = rtc.readByteRAM(RAM_TEST_ADDR);
  bool ok = true;
  const uint8_t patterns[2] = {0xA5, 0x5A};
  for (uint8_t p : patterns) {
    rtc.writeByteRAM(RAM_TEST_ADDR, p);
    if (rtc.readByteRAM(RAM_TEST_ADDR) != p) ok = false;
  }
  rtc.writeByteRAM(RAM_TEST_ADDR, saved);
  return ok;
}

static void ramWrite32(uint8_t addr, uint32_t v) {
  for (uint8_t i = 0; i < 4; i++) rtc.writeByteRAM(addr + i, (v >> (8 * i)) & 0xFF);
}
static uint32_t ramRead32(uint8_t addr) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 4; i++) v |= (uint32_t)rtc.readByteRAM(addr + i) << (8 * i);
  return v;
}

// Last 'U' kept in battery-backed RTC RAM -> drift tracking across power loss.
// phaseUs = RTC - UTC measured right after the write (precise 'U' only).
static void saveSync(uint32_t epoch, int32_t phaseUs) {
  ramWrite32(RAM_U_EPOCH, epoch);
  rtc.writeByteRAM(RAM_U_MAGIC, U_MAGIC);
  ramWrite32(RAM_U_PHASE, (uint32_t)phaseUs);
  rtc.writeByteRAM(RAM_P_MAGIC, P_MAGIC);
  g_syncEpoch = epoch;
}

static uint32_t loadSyncEpoch() {
  if (rtc.readByteRAM(RAM_U_MAGIC) != U_MAGIC) return 0;
  return ramRead32(RAM_U_EPOCH);
}

static bool loadSyncPhase(int32_t *phaseUs) {
  if (rtc.readByteRAM(RAM_P_MAGIC) != P_MAGIC) return false;
  *phaseUs = (int32_t)ramRead32(RAM_U_PHASE);
  return true;
}

// =====================================================================
//                      Edge capture / timing helpers
// =====================================================================
// RTC second edge in ESP32 esp_timer time. The DS1302 latches its user
// buffers on the rising edge of CE, so the edge lies between the start of
// the last read that saw the old second and the start of the first read
// that saw the new one; t is the midpoint, unc the half width.

// Waits for the next change of the seconds register. With predict != 0 the
// task sleeps until EDGE_GUARD_US before it, then busy-polls.
static bool captureEdge(Edge *e, int64_t predict = 0, uint32_t timeoutMs = 1500) {
  if (predict) {
    int64_t wait = predict - EDGE_GUARD_US - nowUs();
    if (wait > 2000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
  }
  int64_t deadline = nowUs() + (int64_t)timeoutMs * 1000;
  int64_t tPrev = nowUs();
  uint8_t r0 = rtc.readRegister(0);
  if ((r0 & 0x80) || !bcdValid(r0, 0, 59)) return false;
  uint8_t s0 = bcd2dec(r0);
  while (nowUs() < deadline) {
    int64_t t = nowUs();
    uint8_t r = rtc.readRegister(0);
    if (r == r0) {
      tPrev = t;
      continue;
    }
    if ((r & 0x80) || !bcdValid(r, 0, 59) || bcd2dec(r) != (s0 + 1) % 60) {
      g_busGlitches++; // implausible value: keep polling, window widens
      continue;
    }
    e->t = (tPrev + t) / 2;
    e->unc = (int32_t)((t - tPrev) / 2);
    e->sec = bcd2dec(r);
    return true;
  }
  return false;
}

// Sleep, then spin, until esp_timer reaches t
static void waitUntil(int64_t t) {
  int64_t ms = (t - nowUs()) / 1000 - 3;
  if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
  while (nowUs() < t) {
  }
}

// Duration of an 8-byte clock burst transfer; used to aim writes so that CE
// falls (the moment the DS1302 resets its countdown chain) on target.
static int32_t clockBurstUs() {
  uint8_t tmp[8];
  int64_t t0 = nowUs();
  rtc.readBuffer(tmp, sizeof(tmp));
  return (int32_t)(nowUs() - t0);
}

// Least-squares line y = a + b x

static Fit linfit(const double *x, const double *y, int n) {
  Fit f = {0, 0, 0, 0, 0, n};
  if (n < 2) return f;
  double mx = 0, my = 0;
  for (int i = 0; i < n; i++) {
    mx += x[i];
    my += y[i];
  }
  mx /= n;
  my /= n;
  double sxx = 0, sxy = 0;
  for (int i = 0; i < n; i++) {
    sxx += (x[i] - mx) * (x[i] - mx);
    sxy += (x[i] - mx) * (y[i] - my);
  }
  if (sxx <= 0) return f;
  f.b = sxy / sxx;
  f.a = my - f.b * mx;
  double ss = 0;
  for (int i = 0; i < n; i++) {
    double r = y[i] - (f.a + f.b * x[i]);
    ss += r * r;
    if (fabs(r) > f.maxAbs) f.maxAbs = fabs(r);
  }
  f.rms = sqrt(ss / n);
  f.seB = n > 2 ? sqrt(ss / (n - 2) / sxx) : 0;
  return f;
}

// 'X' typed during a long test aborts it
static bool abortRequested() {
  while (Con.available()) {
    int c = Con.read();
    if (c == 'x' || c == 'X') {
      Con.println("[BENCH] Abort requested");
      return true;
    }
  }
  return false;
}

// =====================================================================
//                               Printing
// =====================================================================
// "dd/mm/yyyy hh:mm:ss" in local time
static void fmtLocal(uint32_t epochUtc, char *buf, size_t len) {
  time_t lt = (time_t)(epochUtc + TZ_OFFSET_SEC);
  struct tm t;
  gmtime_r(&lt, &t);
  snprintf(buf, len, "%02d/%02d/%04d %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1,
           t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
}

static void printTimeLine(uint32_t epochUtc, const char *tag, const char *note = "") {
  char lt[24];
  fmtLocal(epochUtc, lt, sizeof(lt));
  Con.printf("[%s] Unix: %lu | %s: %s%s\n", tag, (unsigned long)epochUtc, TZ_LABEL, lt, note);
}

// " | Last U: dd/mm/yyyy hh:mm:ss (1d 2h 3m ago)" relative to RTC time nowEpoch
static void fmtLastSync(uint32_t nowEpoch, char *buf, size_t len) {
  if (g_syncEpoch < MIN_VALID_EPOCH) {
    snprintf(buf, len, " | Last U: never");
    return;
  }
  char lt[24];
  fmtLocal(g_syncEpoch, lt, sizeof(lt));
  if (nowEpoch < g_syncEpoch) {
    snprintf(buf, len, " | Last U: %s", lt);
    return;
  }
  uint32_t age = nowEpoch - g_syncEpoch;
  snprintf(buf, len, " | Last U: %s (%lud %luh %lum ago)", lt,
           (unsigned long)(age / 86400), (unsigned long)(age % 86400 / 3600),
           (unsigned long)(age % 3600 / 60));
}

static void printLastSync() {
  uint32_t now = 0;
  if (!rtcReadEpoch(&now)) now = 0;
  char buf[72];
  fmtLastSync(now, buf, sizeof(buf));
  Con.printf("[RTC]%s\n", buf + 2); // drop leading " |"
}

static void printHelp() {
  Con.println();
  Con.println("========== RTC BENCH " FW_VERSION " (SJWCH1302 / DS1302 on ESP32-C5) ==========");
  Con.println("  U : Update RTC from NTP (written on the exact UTC second)");
  Con.println("  C : Check RTC vs NTP, ms resolution + ppm since last 'U' (RTC not written)");
  Con.println("  B : Benchmark suite, non-destructive (~16 min, offline + online)");
  Con.println("  D : Destructive suite: osc start-up + calendar rollovers (~1 min, time restored)");
  Con.println("  W : WiFi on/off between commands (default off = offline)");
  Con.println("  S : Status / soak counters now");
  Con.println("  T : Bus integrity test only (RAM patterns + clock reads, ~2 s)");
  Con.println("  E : Bus timing (driver speed, pinMode cost on this chip)");
  Con.println("  R : Raw register / RAM dump");
  Con.println("  L : Show when the RTC was last updated ('U')");
  Con.println("  X : Abort a running benchmark");
  Con.println("  H : This help");
  Con.println("  Otherwise time is read only from the external RTC.");
  Con.println("=====================================================================");
}

static void dumpRegisters() {
  const char *names[] = {"SEC", "MIN", "HOUR", "DATE", "MONTH", "DAY", "YEAR", "WP", "TCS"};
  Con.println("[RTC] Register dump (raw hex):");
  for (uint8_t i = 0; i < 9; i++) {
    Con.printf("  %-5s = 0x%02X\n", names[i], rtc.readRegister(i));
  }
  Con.print("  RAM   : ");
  for (uint8_t i = 0; i < 31; i++) Con.printf("%02X ", rtc.readByteRAM(i));
  Con.println();

  uint8_t sec = rtc.readRegister(0), hour = rtc.readRegister(2), tcs = rtc.readRegister(8);
  Con.printf("  CH=%d (%s), %s mode, trickle charger %s\n", (sec >> 7) & 1,
             (sec & 0x80) ? "HALTED" : "running", (hour & 0x80) ? "12h" : "24h",
             ((tcs & 0xF0) == 0xA0) ? "ENABLED" : "disabled");
}

static void printStat() {
  uint32_t up = millis() / 1000;
  float temp = temperatureRead();
  Con.printf("[STAT] up %lud %02lu:%02lu:%02lu | WiFi %s | heap %lu (min %lu) | rd_err %lu | "
             "jumps %lu | stalls %lu | glitch %lu | die %.1f C\n",
             (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600),
             (unsigned long)(up % 3600 / 60), (unsigned long)(up % 60),
             WiFi.status() == WL_CONNECTED ? "on" : "off",
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
             (unsigned long)g_readErrors, (unsigned long)g_tickJumps,
             (unsigned long)g_tickStalls, (unsigned long)g_busGlitches, temp);
}

// =====================================================================
//                           WiFi + NTP client
// =====================================================================
static void setWifiProtocols() {
  // ESP32-C5: MUST follow every WiFi.mode() call
  wifi_protocols_t protocol = {
    .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
    .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC
  };
  esp_wifi_set_protocols(WIFI_IF_STA, &protocol);
}

static bool netUp() {
  if (WiFi.status() != WL_CONNECTED) {
    Con.println("[WiFi] Connecting ...");
    WiFi.mode(WIFI_STA);
    setWifiProtocols();
    WiFi.setSleep(false); // modem sleep adds up to a beacon interval to NTP round trips
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
      Con.println("[WiFi] E: Connect timeout");
      return false;
    }
    Con.printf("[WiFi] Connected. RSSI=%d dBm CH=%d\n", WiFi.RSSI(), WiFi.channel());
    g_ntpResolved = false;
  }
  if (!g_ntpResolved) {
    if (WiFi.hostByName(NTP_HOST, g_ntpIp) != 1) {
      Con.println("[NTP] E: DNS lookup failed");
      return false;
    }
    g_ntpResolved = true;
    ntpUdp.stop();
    ntpUdp.begin(NTP_LOCAL_PORT);
  }
  return true;
}

// Offline again unless 'W' asked to keep WiFi on
static void netDown(bool force = false) {
  if (g_wifiKeep && !force) return;
  if (WiFi.getMode() == WIFI_OFF) return;
  ntpUdp.stop();
  g_ntpResolved = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Con.println("[WiFi] Off (offline)");
}

// One NTP exchange. off = UTC - esp_timer (us) at local time t.

static int64_t ntpToUnixUs(const uint8_t *p) {
  uint32_t sec = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  uint32_t frac = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
  return ((int64_t)sec - 2208988800LL) * 1000000LL + (int64_t)(((uint64_t)frac * 1000000ULL) >> 32);
}

static bool ntpQuery(NtpSample *s) {
  uint8_t pkt[48] = {0};
  pkt[0] = 0x23; // LI 0, version 4, mode 3 (client)
  uint32_t nonce[2] = {esp_random(), esp_random()};
  memcpy(pkt + 40, nonce, 8); // echoed back as originate timestamp

  while (ntpUdp.parsePacket() > 0) ntpUdp.clear(); // drop late replies
  if (!ntpUdp.beginPacket(g_ntpIp, NTP_PORT)) return false;
  ntpUdp.write(pkt, sizeof(pkt));
  int64_t t1 = nowUs();
  if (!ntpUdp.endPacket()) return false;

  int64_t deadline = t1 + NTP_TIMEOUT_MS * 1000LL;
  while (nowUs() < deadline) {
    if (ntpUdp.parsePacket() < 48) continue;
    int64_t t4 = nowUs();
    uint8_t r[48];
    ntpUdp.read(r, sizeof(r));
    if (memcmp(r + 24, pkt + 40, 8) != 0) continue; // not the reply to this query
    uint8_t li = r[0] >> 6, mode = r[0] & 7, stratum = r[1];
    if (mode != 4 || li == 3 || stratum == 0 || stratum > 15) return false;
    int64_t T2 = ntpToUnixUs(r + 32), T3 = ntpToUnixUs(r + 40);
    s->off = ((T2 - t1) + (T3 - t4)) / 2;
    s->delay = (int32_t)((t4 - t1) - (T3 - T2));
    s->t = (t1 + t4) / 2;
    return true;
  }
  return false;
}

// Lowest round-trip delay of a burst = smallest offset error (<= delay/2)
static bool ntpBurst(NtpSample *best) {
  bool ok = false;
  for (int i = 0; i < NTP_BURST; i++) {
    NtpSample s;
    if (ntpQuery(&s) && (!ok || s.delay < best->delay)) {
      *best = s;
      ok = true;
    }
    if (i + 1 < NTP_BURST) delay(60);
  }
  return ok;
}

// =====================================================================
//                   Precise phase: RTC - UTC at a tick
// =====================================================================

static bool measurePhase(Phase *p) {
  NtpSample a, b;
  Edge e;
  uint32_t ep;
  if (!ntpBurst(&a)) {
    Con.println("[NTP] E: No valid NTP response");
    return false;
  }
  // Retry when the capture window was widened by WiFi interrupts
  bool got = false;
  for (int i = 0; i < 4 && !got; i++) got = captureEdge(&e, 0, 1500) && e.unc <= EDGE_MAX_UNC_US;
  if (!got) {
    Con.println("[RTC] E: No clean RTC tick captured");
    return false;
  }
  if (!rtcReadEpoch(&ep) || ep % 60 != e.sec) {
    Con.println("[RTC] E: Read after tick failed");
    return false;
  }
  if (!ntpBurst(&b)) {
    Con.println("[NTP] E: No valid NTP response");
    return false;
  }
  double f = (double)(e.t - a.t) / (double)(b.t - a.t);
  int64_t off = a.off + (int64_t)((double)(b.off - a.off) * f);
  p->epoch = ep;
  p->utcUs = e.t + off;
  p->errUs = (int64_t)ep * 1000000LL - p->utcUs;
  p->delayUs = max(a.delay, b.delay);
  p->uncUs = e.unc + p->delayUs / 2;
  return true;
}

// Phase report + drift since the last 'U'
static void reportPhase(const Phase &p) {
  char lt[24];
  fmtLocal(p.epoch, lt, sizeof(lt));
  Con.printf("[NTP ] server %s, round-trip %.1f ms\n", NTP_HOST, p.delayUs / 1000.0);
  Con.printf("[RTC ] Unix: %lu | %s: %s at the measured tick\n", (unsigned long)p.epoch, TZ_LABEL, lt);
  Con.printf("[CHECK] RTC - UTC = %+.1f ms (+/- %.1f ms)\n", p.errUs / 1000.0, p.uncUs / 1000.0);

  uint32_t syncEpoch = loadSyncEpoch();
  if (syncEpoch < MIN_VALID_EPOCH || p.utcUs / 1000000 <= (int64_t)syncEpoch) return;
  int32_t phaseU = 0;
  bool precise = loadSyncPhase(&phaseU);
  // A legacy 'U' (no stored phase) was aligned by SNTP: assume 0 +/- 100 ms
  double uncU = precise ? 1000.0 : 100000.0;
  double elapsed = (p.utcUs - (int64_t)syncEpoch * 1000000LL) / 1e6;
  double drift = (double)(p.errUs - phaseU);
  double ppm = drift / elapsed;
  double ppmUnc = (p.uncUs + uncU) / elapsed;
  Con.printf("[CHECK] Since last 'U' (%.2f h): %+.1f ms -> %+.2f ppm (+/- %.2f), %+.2f s/day%s\n",
             elapsed / 3600.0, drift / 1000.0, ppm, ppmUnc, ppm * 0.0864,
             precise ? "" : "  [legacy U: phase assumed 0 +/- 100 ms]");
}

// 'C': measure only
static void cmdCheck() {
  Con.println("\n>>> C: Check RTC vs NTP (RTC not written)");
  if (!rtc.isRunning()) {
    Con.println("[CHECK] RTC halted, nothing to compare");
    return;
  }
  if (netUp()) {
    Phase p;
    if (measurePhase(&p)) reportPhase(p);
  }
  netDown();
  g_lastPrintedEpoch = 0;
}

// 'U': report drift, then write the RTC so that CE falls on the UTC second
static void cmdUpdate() {
  Con.println("\n>>> U: Update RTC from NTP");
  if (!netUp()) {
    netDown();
    return;
  }

  uint32_t ep;
  if (rtc.isRunning() && rtcReadEpoch(&ep) && ep >= MIN_VALID_EPOCH) {
    Phase p;
    if (measurePhase(&p)) reportPhase(p);
  }

  NtpSample s;
  if (!ntpBurst(&s)) {
    Con.println("[NTP] E: No valid NTP response, RTC not changed");
    netDown();
    return;
  }
  int32_t xfer = clockBurstUs();
  int64_t utcNow = nowUs() + s.off;
  uint32_t S = (uint32_t)(utcNow / 1000000) + 1;
  if ((int64_t)S * 1000000 - utcNow < 300000) S++;
  int64_t tTarget = (int64_t)S * 1000000 - s.off;
  time_t tt = S;
  struct tm tmv;
  gmtime_r(&tt, &tmv);

  waitUntil(tTarget - xfer);
  bool wrote = rtc.write(&tmv);
  int64_t tEnd = nowUs();

  Edge e;
  if (!wrote || !captureEdge(&e, tEnd + 1000000, 1500)) {
    Con.println("[RTC] E: write or verify failed");
    netDown();
    return;
  }
  uint32_t rb = 0;
  rtcReadEpoch(&rb);
  int64_t phase = (int64_t)(S + 1) * 1000000LL - (e.t + s.off);
  printTimeLine(rb, "RTC ");
  Con.printf("[RTC] %s: written %+.3f ms from the UTC second, first tick RTC - UTC = %+.2f ms "
             "(+/- %.1f ms NTP)\n",
             (rb == S + 1 && llabs(phase) < 50000) ? "OK: RTC updated + verified" : "E: VERIFY FAILED",
             (tEnd - tTarget) / 1000.0, phase / 1000.0, s.delay / 2000.0);
  if (rb == S + 1) saveSync(S, (int32_t)phase);
  netDown();
  g_lastPrintedEpoch = 0;
}

// =====================================================================
//                         Benchmark building blocks
// =====================================================================

// RAM patterns (single byte + burst), WP-free; clock burst reads checked for
// valid BCD and continuity. RAM content is saved and restored.
static void runBusTest(BusResult *r) {
  memset(r, 0, sizeof(*r));
  uint8_t saved[31];
  rtc.readBufferRAM(saved, sizeof(saved));

  const uint8_t base[4] = {0x00, 0xFF, 0x55, 0xAA};
  for (int round = 0; round < BUS_ROUNDS; round++) {
    for (int p = 0; p < 20; p++) {
      uint8_t pat = p < 4 ? base[p] : (p < 12 ? (uint8_t)(1 << (p - 4)) : (uint8_t)~(1 << (p - 12)));
      // value differs per address so address aliasing shows up
      for (uint8_t a = 0; a < 31; a++) rtc.writeByteRAM(a, pat ^ a);
      for (uint8_t a = 0; a < 31; a++) {
        r->ramOps++;
        uint8_t got = rtc.readByteRAM(a), exp = pat ^ a;
        if (got != exp) {
          r->ramErr++;
          r->errAddrMask |= 1UL << a;
          r->errBitsOr |= got ^ exp;
          if (r->ramErr == 1) {
            r->firstAddr = a;
            r->firstExp = exp;
            r->firstGot = got;
          }
        }
      }
    }
    uint8_t wbuf[31], rbuf[31];
    for (int i = 0; i < 31; i++) wbuf[i] = (uint8_t)esp_random();
    rtc.writeBufferRAM(wbuf, sizeof(wbuf));
    rtc.readBufferRAM(rbuf, sizeof(rbuf));
    r->burstOps++;
    if (memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
      r->burstErr++;
      for (int i = 0; i < 31; i++)
        if (wbuf[i] != rbuf[i]) r->burstAddrMask |= 1UL << i;
    }
  }

  rtc.writeBufferRAM(saved, sizeof(saved));
  uint8_t check[31];
  rtc.readBufferRAM(check, sizeof(check));
  if (memcmp(saved, check, sizeof(saved)) != 0) {
    r->ramErr++;
    Con.println("[BUS] E: RAM restore verify failed");
  }

  uint32_t prev = 0;
  for (int i = 0; i < BUS_CLOCK_READS; i++) {
    uint8_t b[8];
    rtc.readBuffer(b, sizeof(b));
    r->clkReads++;
    bool ok = !(b[0] & 0x80) && bcdValid(b[0], 0, 59) && bcdValid(b[1], 0, 59) &&
              !(b[2] & 0x80) && bcdValid(b[2], 0, 23) && bcdValid(b[3], 1, 31) &&
              bcdValid(b[4], 1, 12) && bcdValid(b[5], 1, 7) && bcdValid(b[6], 0, 99) &&
              (b[7] & 0x7F) == 0;
    uint32_t ep = 0;
    if (ok) {
      ep = (uint32_t)daysFromCivil(2000 + bcd2dec(b[6]), bcd2dec(b[4]), bcd2dec(b[3])) * 86400UL +
           bcd2dec(b[2]) * 3600UL + bcd2dec(b[1]) * 60UL + bcd2dec(b[0]);
      if (prev && (ep < prev || ep - prev > 1)) ok = false;
    }
    if (!ok) r->clkErr++;
    else prev = ep;
  }

  int64_t t0 = nowUs();
  for (int i = 0; i < 1000; i++) rtc.readRegister(0);
  r->usPerRead = (nowUs() - t0) / 1000.0f;
}

// Which RAM addresses are not independent: write one address at a time
// over a known background and see what else changes. Prints one line per
// coupled address; returns the number of addresses that are not independent.
static int ramAliasProbe(const char *tag) {
  uint8_t saved[31], bg[31], before[31], got[31];
  rtc.readBufferRAM(saved, sizeof(saved));
  int bad = 0;
  for (uint8_t a = 0; a < 31; a++) {
    for (uint8_t i = 0; i < 31; i++) bg[i] = 0x40 | i;
    rtc.writeBufferRAM(bg, sizeof(bg));
    rtc.readBufferRAM(before, sizeof(before)); // background as actually stored
    uint8_t v = 0xA0 | (a & 0x1F);
    rtc.writeByteRAM(a, v);
    rtc.readBufferRAM(got, sizeof(got));
    uint32_t changed = 0;
    for (uint8_t i = 0; i < 31; i++)
      if (i != a && got[i] != before[i]) changed |= 1UL << i;
    bool own = got[a] == v;
    if (changed || !own) {
      bad++;
      Con.printf("[%s]   write RAM %2u = 0x%02X: reads back 0x%02X (%s), also changed mask 0x%08lX\n", tag,
                 a, v, got[a], own ? "ok" : "NOT STORED", (unsigned long)changed);
    }
  }
  rtc.writeBufferRAM(saved, sizeof(saved));
  Con.printf("[%s] RAM alias probe: %d of 31 addresses not independent\n", tag, bad);
  return bad;
}

static bool busPass(const BusResult &r) { return r.ramErr == 0 && r.burstErr == 0 && r.clkErr == 0; }

static void printBus(const char *tag, const BusResult &r) {
  Con.printf("[%s] RAM byte %lu ops %lu err | RAM burst %lu ops %lu err | clock burst %lu reads %lu err"
             " | %.1f us per 1-byte read -> %s\n",
             tag, (unsigned long)r.ramOps, (unsigned long)r.ramErr, (unsigned long)r.burstOps,
             (unsigned long)r.burstErr, (unsigned long)r.clkReads, (unsigned long)r.clkErr,
             r.usPerRead, busPass(r) ? "PASS" : "FAIL");
  if (r.ramErr)
    Con.printf("[%s]   byte errors at RAM addr mask 0x%08lX, bits 0x%02X, first: addr %u wrote 0x%02X read 0x%02X\n",
               tag, (unsigned long)r.errAddrMask, r.errBitsOr, r.firstAddr, r.firstExp, r.firstGot);
  if (r.burstErr)
    Con.printf("[%s]   burst errors at RAM addr mask 0x%08lX\n", tag, (unsigned long)r.burstAddrMask);
}


// Captures n RTC ticks; rate vs the ESP32 crystal from a straight-line fit
// of edge time vs second index; jitter from the fit residuals.
static bool runTickTest(int n, TickResult *r, const char *tag) {
  memset(r, 0, sizeof(*r));
  double *x = (double *)malloc(sizeof(double) * (n + 2));
  double *y = (double *)malloc(sizeof(double) * (n + 2));
  if (!x || !y) {
    free(x);
    free(y);
    return false;
  }
  r->minIntUs = 1e12;
  Edge e, last;
  if (!captureEdge(&e, 0, 1500)) {
    free(x);
    free(y);
    Con.printf("[%s] E: no tick\n", tag);
    return false;
  }
  int64_t t0 = e.t;
  int idx = 0, nfit = 0;
  bool lastGood = e.unc <= EDGE_MAX_UNC_US;
  if (lastGood) {
    x[nfit] = 0;
    y[nfit++] = 0;
  } else {
    r->noisy++;
  }
  last = e;
  double estUs = 1000000.0;
  while (idx < n) {
    if (abortRequested()) break;
    if (!captureEdge(&e, last.t + (int64_t)estUs, 2500)) {
      if (++r->fails >= 3) break;
      continue;
    }
    int dn = (e.sec - last.sec + 60) % 60;
    if (dn != 1) r->missed += dn - 1;
    idx += dn;
    bool good = e.unc <= EDGE_MAX_UNC_US;
    if (good) {
      x[nfit] = idx;
      y[nfit++] = (double)(e.t - t0);
    } else {
      r->noisy++;
    }
    if (good && lastGood && dn == 1) {
      double iv = (double)(e.t - last.t);
      if (iv < r->minIntUs) r->minIntUs = iv;
      if (iv > r->maxIntUs) r->maxIntUs = iv;
    }
    if (nfit >= 2) estUs = (y[nfit - 1] - y[0]) / (x[nfit - 1] - x[0]);
    last = e;
    lastGood = good;
    if (idx % 30 == 0) Con.printf("[%s] %d/%d ticks\n", tag, idx, n);
  }
  Fit f = linfit(x, y, nfit);
  free(x);
  free(y);
  r->ticks = idx;
  r->valid = nfit >= 10 && r->fails < 3;
  r->usPerSec = f.b;
  r->ppmVsXtal = (1e6 / f.b - 1.0) * 1e6;
  r->seppm = f.seB; // 1 us per second = 1 ppm
  r->rmsUs = f.rms;
  r->maxAbsUs = f.maxAbs;
  return r->valid;
}

static bool tickPass(const TickResult &r) {
  return r.valid && r.missed == 0 && r.maxAbsUs <= LIMIT_JITTER_US;
}

static void printTick(const char *tag, const TickResult &r) {
  Con.printf("[%s] %d ticks, %d missed, %d noisy captures | RTC vs ESP32 XTAL %+.2f ppm (+/- %.2f) | "
             "jitter rms %.1f us, max %.1f us | interval %.1f..%.1f us -> %s\n",
             tag, r.ticks, r.missed, r.noisy, r.ppmVsXtal, 2 * r.seppm, r.rmsUs, r.maxAbsUs,
             r.minIntUs, r.maxIntUs, tickPass(r) ? "PASS" : "FAIL");
}

// ---------------- RF stress: WiFi TX in a separate task ----------------
volatile bool g_stressRun = false;
volatile bool g_stressDone = true;
volatile uint32_t g_stressPkts = 0, g_stressFail = 0;

static void stressTask(void *) {
  WiFiUDP u;
  static uint8_t buf[STRESS_PAYLOAD];
  memset(buf, 0xA5, sizeof(buf));
  IPAddress gw = WiFi.gatewayIP();
  while (g_stressRun) {
    if (u.beginPacket(gw, STRESS_PORT) && u.write(buf, sizeof(buf)) && u.endPacket()) g_stressPkts = g_stressPkts + 1;
    else g_stressFail = g_stressFail + 1;
    vTaskDelay(pdMS_TO_TICKS(1000 / STRESS_PPS));
  }
  g_stressDone = true;
  vTaskDelete(NULL);
}

static bool stressStart() {
  g_stressPkts = 0;
  g_stressFail = 0;
  g_stressRun = true;
  g_stressDone = false;
  TaskHandle_t h = NULL;
  if (xTaskCreate(stressTask, "rf_stress", 4096, NULL, 1, &h) != pdPASS) {
    g_stressRun = false;
    g_stressDone = true;
    return false;
  }
  return true;
}

static void stressStop() {
  g_stressRun = false;
  for (int i = 0; i < 100 && !g_stressDone; i++) delay(10);
}

// ---------------- Accuracy vs NTP ----------------

// Every RTC tick is timed on the ESP32 crystal; NTP bursts every
// NTP_INTERVAL_S map ESP32 time to UTC (straight-line fit, so the ESP32
// crystal error is measured, not assumed). RTC - UTC per tick, fitted -> ppm.
static bool runAccuracyTest(int minutes, AccResult *r) {
  memset(r, 0, sizeof(*r));
  int n = minutes * 60;
  int nNtpMax = n / NTP_INTERVAL_S + 4;
  int64_t *et = (int64_t *)malloc(sizeof(int64_t) * (n + 2));
  int32_t *en = (int32_t *)malloc(sizeof(int32_t) * (n + 2));
  NtpSample *ns = (NtpSample *)malloc(sizeof(NtpSample) * nNtpMax);
  double *x = (double *)malloc(sizeof(double) * (n + 2));
  double *y = (double *)malloc(sizeof(double) * (n + 2));
  bool ok = et && en && ns && x && y;
  int ne = 0, nn = 0;
  Edge e, last;
  uint32_t E0 = 0;

  if (ok && captureEdge(&e, 0, 1500) && rtcReadEpoch(&E0) && E0 % 60 == e.sec) {
    et[ne] = e.t;
    en[ne++] = 0;
    last = e;
    int idx = 0;
    double estUs = 1000000.0;
    if (ntpBurst(&ns[nn])) nn++;
    r->ntpTotal++;
    while (idx < n) {
      if (abortRequested()) break;
      if (!captureEdge(&e, last.t + (int64_t)estUs, 2500)) {
        r->missed++;
        if (r->missed > 20) break;
        continue;
      }
      int dn = (e.sec - last.sec + 60) % 60;
      if (dn != 1) r->missed += dn - 1;
      idx += dn;
      if (e.unc <= EDGE_MAX_UNC_US) {
        et[ne] = e.t;
        en[ne++] = idx;
      } else {
        r->noisy++;
      }
      if (ne >= 2) estUs = (double)(et[ne - 1] - et[0]) / (en[ne - 1] - en[0]);
      last = e;
      if (idx % NTP_INTERVAL_S == 0 || idx >= n) {
        r->ntpTotal++;
        if (nn < nNtpMax && ntpBurst(&ns[nn])) nn++;
        Con.printf("[ACC] %d/%d s, %d NTP samples\n", idx, n, nn);
      }
    }
    r->ticks = idx;
  } else {
    ok = false;
  }

  if (ok && ne >= 60 && nn >= 3) {
    // 1) ESP32 time -> UTC: fit NTP offset vs local time, drop high-delay samples
    int32_t dmin = ns[0].delay;
    for (int i = 1; i < nn; i++) dmin = min(dmin, ns[i].delay);
    int k = 0;
    int64_t tBase = et[0];
    for (int i = 0; i < nn; i++) {
      if (ns[i].delay > 2 * dmin + 2000) continue;
      x[k] = (ns[i].t - tBase) / 1e6;
      y[k++] = (double)(ns[i].off - ns[0].off);
      r->ntpUsed++;
    }
    Fit fo = linfit(x, y, k);
    r->bestDelayUs = dmin;
    r->ntpRmsUs = fo.rms;
    r->xtalPpm = -fo.b; // offset grows when the ESP32 clock runs slow

    // 2) RTC - UTC at every tick, fitted against UTC. Large terms are
    //    subtracted in int64 first so the doubles keep sub-us resolution.
    int64_t baseUs = (int64_t)E0 * 1000000LL;
    for (int i = 0; i < ne; i++) {
      double ts = (et[i] - tBase) / 1e6;
      double utcRel = (double)(et[i] + ns[0].off - baseUs) + fo.a + fo.b * ts;
      x[i] = ts;
      y[i] = en[i] * 1e6 - utcRel;
    }
    Fit fe = linfit(x, y, ne);
    r->rtcPpm = fe.b;
    r->rtcPpmUnc = 2.0 * sqrt(fe.seB * fe.seB + fo.seB * fo.seB);
    r->phaseEndMs = (fe.a + fe.b * x[ne - 1]) / 1000.0;

    // 3) Cross-check: RTC vs ESP32 crystal only
    for (int i = 0; i < ne; i++) {
      x[i] = en[i];
      y[i] = (double)(et[i] - tBase);
    }
    Fit fr = linfit(x, y, ne);
    r->relPpm = (1e6 / fr.b - 1.0) * 1e6;
    r->minutes = (et[ne - 1] - et[0]) / 60e6;
    r->valid = true;
  }
  free(et);
  free(en);
  free(ns);
  free(x);
  free(y);
  return r->valid;
}

// ---------------- Register sanity / write protect ----------------
static bool regSanity() {
  uint8_t b[8];
  rtc.readBuffer(b, sizeof(b));
  uint8_t tcs = rtc.readRegister(8);
  bool ch = b[0] & 0x80, h12 = b[2] & 0x80;
  bool bcd = bcdValid(b[0] & 0x7F, 0, 59) && bcdValid(b[1], 0, 59) && bcdValid(b[2] & 0x3F, 0, 23) &&
             bcdValid(b[3], 1, 31) && bcdValid(b[4], 1, 12) && bcdValid(b[5], 1, 7) &&
             bcdValid(b[6], 0, 99);
  bool ok = !ch && !h12 && bcd && (b[7] & 0x7F) == 0;
  Con.printf("[REG] CH=%d %s BCD %s DOW=%d WP=0x%02X TCS=0x%02X (%s) -> %s\n", ch, h12 ? "12h" : "24h",
             bcd ? "valid" : "INVALID", b[5], b[7], tcs,
             tcs == 0x5C ? "power-on default, charger off" : ((tcs & 0xF0) == 0xA0 ? "charger ON" : "charger off"),
             ok ? "PASS" : "FAIL");
  return ok;
}

static bool wpTest() {
  uint8_t orig = rtc.readByteRAM(RAM_TEST_ADDR);
  rtc.writeRegister(7, 0x80); // WP = 1
  uint8_t wpSet = rtc.readRegister(7);
  rtc.writeByteRAM(RAM_TEST_ADDR, orig ^ 0xFF);
  bool blocked = rtc.readByteRAM(RAM_TEST_ADDR) == orig;
  rtc.writeRegister(7, 0x00); // WP = 0
  uint8_t wpClr = rtc.readRegister(7);
  rtc.writeByteRAM(RAM_TEST_ADDR, orig ^ 0xFF);
  bool writable = rtc.readByteRAM(RAM_TEST_ADDR) == (uint8_t)(orig ^ 0xFF);
  rtc.writeByteRAM(RAM_TEST_ADDR, orig);
  bool ok = wpSet == 0x80 && blocked && wpClr == 0x00 && writable;
  Con.printf("[WP ] WP=1 reads 0x%02X, RAM write %s | WP=0 reads 0x%02X, RAM write %s -> %s\n", wpSet,
             blocked ? "blocked" : "NOT BLOCKED", wpClr, writable ? "works" : "FAILS", ok ? "PASS" : "FAIL");
  return ok;
}

// =====================================================================
//                       'B': non-destructive benchmark
// =====================================================================
static const char *pf(bool ok) { return ok ? "PASS" : "FAIL"; }

static void cmdBenchmark() {
  Con.println("\n>>> B: Benchmark suite (non-destructive). 'X' aborts.");
  if (!rtc.isRunning()) {
    Con.println("[BENCH] RTC halted - press 'U' first");
    return;
  }
  float tempStart = temperatureRead();
  Con.printf("[BENCH] FW %s | die %.1f C | ticks/test %d | accuracy %d min\n", FW_VERSION, tempStart,
             TICK_TEST_S, ACCURACY_TEST_MIN);

  // ---- Offline (radio off) ----
  netDown(true);
  Con.println("[BENCH] --- OFFLINE (WiFi off) ---");
  bool regOk = regSanity();
  bool wpOk = wpTest();
  BusResult busOff, busRf;
  runBusTest(&busOff);
  printBus("BUS off", busOff);
  int aliasN = ramAliasProbe("BUS off");
  TickResult tickOff, tickRf;
  memset(&tickRf, 0, sizeof(tickRf));
  memset(&busRf, 0, sizeof(busRf));
  runTickTest(TICK_TEST_S, &tickOff, "TICK off");
  printTick("TICK off", tickOff);

  // ---- Online ----
  bool online = netUp();
  AccResult acc;
  memset(&acc, 0, sizeof(acc));
  bool stressRan = false;
  if (online) {
    Con.printf("[BENCH] --- ONLINE + RF STRESS (%d x %d B UDP/s to gateway) ---\n", STRESS_PPS, STRESS_PAYLOAD);
    stressRan = stressStart();
    if (!stressRan) Con.println("[BENCH] E: stress task not started");
    runBusTest(&busRf);
    printBus("BUS rf", busRf);
    runTickTest(TICK_TEST_S, &tickRf, "TICK rf");
    stressStop();
    printTick("TICK rf", tickRf);
    Con.printf("[BENCH] stress packets sent %lu, failed %lu\n", (unsigned long)g_stressPkts,
               (unsigned long)g_stressFail);

    Con.printf("[BENCH] --- ONLINE ACCURACY vs NTP (%d min) ---\n", ACCURACY_TEST_MIN);
    if (runAccuracyTest(ACCURACY_TEST_MIN, &acc)) {
      Con.printf("[ACC] %.1f min, %d ticks (%d not captured while NTP ran, %d noisy), NTP %d/%d samples used, best RTT %.1f ms, "
                 "offset fit rms %.2f ms\n",
                 acc.minutes, acc.ticks, acc.missed, acc.noisy, acc.ntpUsed, acc.ntpTotal,
                 acc.bestDelayUs / 1000.0, acc.ntpRmsUs / 1000.0);
    } else {
      Con.println("[ACC] E: accuracy test failed (NTP or tick capture)");
    }
  } else {
    Con.println("[BENCH] Online tests SKIPPED (no WiFi)");
  }
  netDown();

  // ---- Summary ----
  float tempEnd = temperatureRead();
  double rfShift = tickRf.valid && tickOff.valid ? tickRf.ppmVsXtal - tickOff.ppmVsXtal : NAN;
  Con.println("[RESULT] ====================== BENCHMARK SUMMARY ======================");
  Con.printf("[RESULT] Register sanity             : %s\n", pf(regOk));
  Con.printf("[RESULT] Write protect               : %s\n", pf(wpOk));
  Con.printf("[RESULT] Bus integrity, offline      : %s (%lu errors)\n", pf(busPass(busOff)),
             (unsigned long)(busOff.ramErr + busOff.burstErr + busOff.clkErr));
  Con.printf("[RESULT] RAM addresses independent    : %s (%d of 31 coupled)\n", pf(aliasN == 0), aliasN);
  Con.printf("[RESULT] Tick test, offline          : %s (%d ticks, %d missed, max jitter %.0f us)\n",
             pf(tickPass(tickOff)), tickOff.ticks, tickOff.missed, tickOff.maxAbsUs);
  Con.printf("[RESULT] RTC vs ESP32 XTAL, offline  : %+.2f ppm\n", tickOff.ppmVsXtal);
  if (online) {
    Con.printf("[RESULT] Bus integrity, RF stress    : %s (%lu errors)\n", pf(busPass(busRf)),
               (unsigned long)(busRf.ramErr + busRf.burstErr + busRf.clkErr));
    Con.printf("[RESULT] Tick test, RF stress        : %s (%d ticks, %d missed, max jitter %.0f us)\n",
               pf(tickPass(tickRf)), tickRf.ticks, tickRf.missed, tickRf.maxAbsUs);
    Con.printf("[RESULT] RF stress rate shift        : %s (%+.2f ppm, limit +/-%.1f)\n",
               pf(!isnan(rfShift) && fabs(rfShift) <= LIMIT_RF_SHIFT_PPM), rfShift, LIMIT_RF_SHIFT_PPM);
    if (acc.valid) {
      Con.printf("[RESULT] ESP32 XTAL vs NTP           : %+.2f ppm\n", acc.xtalPpm);
      Con.printf("[RESULT] RTC accuracy vs NTP         : %s (%+.2f ppm +/- %.2f, %+.2f s/day, "
                 "%+.1f s/30 days, limit +/-%.0f ppm)\n",
                 pf(fabs(acc.rtcPpm) <= LIMIT_PPM), acc.rtcPpm, acc.rtcPpmUnc, acc.rtcPpm * 0.0864,
                 acc.rtcPpm * 2.592, LIMIT_PPM);
      Con.printf("[RESULT] Cross-check (RTC/XTAL+XTAL/NTP): %+.2f ppm (should match RTC accuracy)\n",
                 acc.relPpm + acc.xtalPpm);
      Con.printf("[RESULT] RTC - UTC now               : %+.1f ms\n", acc.phaseEndMs);
    } else {
      Con.println("[RESULT] RTC accuracy vs NTP         : NOT MEASURED");
    }
  } else {
    Con.println("[RESULT] Online tests                : SKIPPED (no WiFi)");
  }
  Con.printf("[RESULT] Die temperature             : %.1f C -> %.1f C\n", tempStart, tempEnd);
  Con.println("[RESULT] ===============================================================");
  g_lastPrintedEpoch = 0;
}

// =====================================================================
//                       'D': destructive suite
// =====================================================================
// RTC time model while the clock is being rewritten: tick k after the
// reference occurs at a + b*k (esp_timer us) and reads e0 + k.

static bool captureRef(RtcRef *ref, int edges) {
  Edge e, last;
  if (!captureEdge(&e, 0, 1500)) return false;
  uint32_t ep;
  if (!rtcReadEpoch(&ep) || ep % 60 != e.sec) return false;
  double x[32], y[32];
  int n = 0, idx = 0;
  int64_t t0 = e.t;
  x[n] = 0;
  y[n++] = 0;
  last = e;
  while (n < edges && n < 32) {
    if (!captureEdge(&e, last.t + 1000000, 2500)) return false;
    idx += (e.sec - last.sec + 60) % 60;
    if (e.unc <= EDGE_MAX_UNC_US) {
      x[n] = idx;
      y[n++] = (double)(e.t - t0);
    }
    last = e;
  }
  Fit f = linfit(x, y, n);
  ref->e0 = ep;
  ref->a = (double)t0 + f.a;
  ref->b = f.b;
  return true;
}

// Rewrites the RTC so its ticks continue the reference time line
static bool restoreFromRef(const RtcRef &ref, double *errUs) {
  int32_t xfer = clockBurstUs();
  double k = ceil(((double)nowUs() - ref.a) / ref.b + 0.3);
  int64_t tT = (int64_t)(ref.a + k * ref.b);
  time_t tt = (time_t)(ref.e0 + (uint32_t)k);
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  waitUntil(tT - xfer);
  rtc.write(&tmv);
  int64_t tEnd = nowUs();
  Edge e;
  if (!captureEdge(&e, tEnd + 1000000, 1500)) return false;
  double expected = ref.a + (k + 1) * ref.b;
  *errUs = expected - (double)e.t; // RTC - reference (edge late = RTC behind)
  return true;
}


// Each case is written at :58 and read after two ticks. Day-of-week is
// user-defined 1..7 on the DS1302 and must wrap 7 -> 1 at midnight.
static const RollCase ROLL_CASES[] = {
  {"Midnight + DOW 7->1      ", {0x58, 0x59, 0x23, 0x27, 0x10, 0x07, 0x26}, {0x00, 0x00, 0x00, 0x28, 0x10, 0x01, 0x26}},
  {"Hour carry 09:59:59      ", {0x58, 0x59, 0x09, 0x15, 0x06, 0x02, 0x26}, {0x00, 0x00, 0x10, 0x15, 0x06, 0x02, 0x26}},
  {"30-day month 30/04       ", {0x58, 0x59, 0x23, 0x30, 0x04, 0x03, 0x26}, {0x00, 0x00, 0x00, 0x01, 0x05, 0x04, 0x26}},
  {"Feb non-leap 28/02/2027  ", {0x58, 0x59, 0x23, 0x28, 0x02, 0x07, 0x27}, {0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x27}},
  {"Feb leap 28/02/2028      ", {0x58, 0x59, 0x23, 0x28, 0x02, 0x02, 0x28}, {0x00, 0x00, 0x00, 0x29, 0x02, 0x03, 0x28}},
  {"Leap day 29/02/2028      ", {0x58, 0x59, 0x23, 0x29, 0x02, 0x03, 0x28}, {0x00, 0x00, 0x00, 0x01, 0x03, 0x04, 0x28}},
  {"Year end 31/12/2026      ", {0x58, 0x59, 0x23, 0x31, 0x12, 0x05, 0x26}, {0x00, 0x00, 0x00, 0x01, 0x01, 0x06, 0x27}},
  {"Register wrap 2099->2000 ", {0x58, 0x59, 0x23, 0x31, 0x12, 0x04, 0x99}, {0x00, 0x00, 0x00, 0x01, 0x01, 0x05, 0x00}},
  {"12h 11:59:59 AM -> 12 PM ", {0x58, 0x59, 0x91, 0x15, 0x06, 0x02, 0x26}, {0x00, 0x00, 0xB2, 0x15, 0x06, 0x02, 0x26}},
  {"12h 11:59:59 PM -> 12 AM ", {0x58, 0x59, 0xB1, 0x15, 0x06, 0x02, 0x26}, {0x00, 0x00, 0x92, 0x16, 0x06, 0x03, 0x26}},
};

static void cmdDestructive() {
  Con.println("\n>>> D: Destructive suite - halts the oscillator and rewrites the calendar.");
  Con.println("    The RTC time is restored from an ESP32 timing reference afterwards.");
  Con.println("    Type Y within 20 s to continue.");
  uint32_t start = millis();
  int c = -1;
  while (millis() - start < 20000) {
    if (Con.available()) {
      c = toupper(Con.read());
      if (c != '\r' && c != '\n') break;
    }
    delay(10);
  }
  if (c != 'Y') {
    Con.println("[DSTR] Cancelled");
    return;
  }
  if (!rtc.isRunning()) {
    Con.println("[DSTR] RTC halted - press 'U' first");
    return;
  }
  netDown(true); // offline: no radio activity during timing

  RtcRef ref;
  Con.println("[DSTR] Capturing time reference (10 ticks) ...");
  if (!captureRef(&ref, 10)) {
    Con.println("[DSTR] E: reference capture failed, nothing changed");
    return;
  }
  Con.printf("[DSTR] Reference: RTC second = %.2f us of ESP32 time\n", ref.b);

  // ---- Oscillator start-up ----
  double suMin = 1e12, suMax = 0, suSum = 0;
  int suOk = 0;
  for (int i = 0; i < STARTUP_RUNS; i++) {
    rtc.writeRegister(0, 0x80); // CH = 1, seconds 00: oscillator stopped
    delay(1500);
    bool halted = rtc.readRegister(0) == 0x80;
    rtc.writeRegister(0, 0x00); // CH = 0: oscillator restarts, countdown reset
    int64_t ts = nowUs();
    int64_t deadline = ts + 5000000;
    int64_t tPrev = ts, tTick = 0;
    while (nowUs() < deadline) {
      int64_t t = nowUs();
      if (rtc.readRegister(0) == 0x01) {
        tTick = (tPrev + t) / 2;
        break;
      }
      tPrev = t;
      if (t - ts < 900000) vTaskDelay(1);
    }
    if (tTick) {
      double su = (tTick - ts) / 1000.0 - ref.b / 1000.0;
      suMin = min(suMin, su);
      suMax = max(suMax, su);
      suSum += su;
      suOk++;
      Con.printf("[OSC] run %d: halted %s, first tick after %.1f ms -> start-up %.1f ms\n", i + 1,
                 halted ? "yes" : "NO", (tTick - ts) / 1000.0, su);
    } else {
      Con.printf("[OSC] run %d: halted %s, NO TICK within 5 s\n", i + 1, halted ? "yes" : "NO");
    }
  }

  // ---- Calendar rollovers ----
  int rollPass = 0, rollN = sizeof(ROLL_CASES) / sizeof(ROLL_CASES[0]);
  for (int i = 0; i < rollN; i++) {
    const RollCase &rc = ROLL_CASES[i];
    uint8_t w[8];
    memcpy(w, rc.set, 7);
    w[7] = 0; // WP
    rtc.writeBuffer(w, sizeof(w));
    int64_t tw = nowUs();
    Edge e1, e2;
    bool ticks = captureEdge(&e1, tw + 1000000, 2500) && captureEdge(&e2, e1.t + 1000000, 2500);
    uint8_t g[8];
    rtc.readBuffer(g, sizeof(g));
    g[0] &= 0x7F;
    bool ok = ticks && memcmp(g, rc.exp, 7) == 0;
    if (ok) rollPass++;
    Con.printf("[CAL] %s got %02X:%02X:%02X %02X/%02X/%02X dow %X | expected %02X:%02X:%02X %02X/%02X/%02X dow %X -> %s\n",
               rc.name, g[2], g[1], g[0], g[3], g[4], g[6], g[5], rc.exp[2], rc.exp[1], rc.exp[0],
               rc.exp[3], rc.exp[4], rc.exp[6], rc.exp[5], pf(ok));
  }

  // ---- Restore ----
  double restoreUs = NAN;
  bool restored = restoreFromRef(ref, &restoreUs);
  uint32_t now = 0;
  rtcReadEpoch(&now);
  printTimeLine(now, "RTC ", "  <- restored");

  Con.println("[RESULT] ==================== DESTRUCTIVE SUMMARY ====================");
  if (suOk)
    Con.printf("[RESULT] Oscillator start-up (%dx)   : %s (min %.1f / avg %.1f / max %.1f ms, limit %.0f ms)\n",
               STARTUP_RUNS, pf(suOk == STARTUP_RUNS && suMax <= LIMIT_STARTUP_MS), suMin, suSum / suOk, suMax,
               LIMIT_STARTUP_MS);
  else
    Con.println("[RESULT] Oscillator start-up         : FAIL (no restart)");
  Con.printf("[RESULT] Calendar rollovers          : %s (%d/%d)\n", pf(rollPass == rollN), rollPass, rollN);
  Con.printf("[RESULT] Time restore                : %s (RTC - reference %+.1f us)\n",
             pf(restored && fabs(restoreUs) <= LIMIT_RESTORE_US), restoreUs);
  Con.println("[RESULT] ===============================================================");
  g_lastPrintedEpoch = 0;
}

// 'E': bus timing, and why pinMode()-based drivers are slow on this chip
static void cmdBusTiming() {
  Con.println("\n>>> E: Bus timing");
  int64_t t0 = nowUs();
  for (int i = 0; i < 100; i++) rtc.readRegister(0);
  int64_t t1 = nowUs();
  uint8_t b[8];
  for (int i = 0; i < 100; i++) rtc.readBuffer(b, sizeof(b));
  int64_t t2 = nowUs();
  for (int i = 0; i < 100; i++) {
    gpio_set_direction((gpio_num_t)RTC_IO_PIN, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)RTC_IO_PIN, GPIO_MODE_OUTPUT);
  }
  int64_t t3 = nowUs();
  gpio_config_t gc = {};
  gc.pin_bit_mask = 1ULL << RTC_IO_PIN;
  gc.mode = GPIO_MODE_OUTPUT;
  for (int i = 0; i < 4; i++) gpio_config(&gc);
  int64_t t4 = nowUs();
  bool lp = rtc_gpio_is_valid_gpio((gpio_num_t)RTC_IO_PIN);
  for (int i = 0; i < 4; i++) rtc_gpio_deinit((gpio_num_t)RTC_IO_PIN);
  int64_t t5 = nowUs();
  gpio_set_direction((gpio_num_t)RTC_IO_PIN, GPIO_MODE_OUTPUT);
  Con.printf("[BUS] 1-byte register read      : %.1f us\n", (t1 - t0) / 100.0);
  Con.printf("[BUS] 8-byte clock burst read   : %.1f us\n", (t2 - t1) / 100.0);
  Con.printf("[BUS] gpio_set_direction in+out : %.2f us\n", (t3 - t2) / 100.0);
  Con.printf("[BUS] gpio_config() (pinMode)   : %.1f us\n", (t4 - t3) / 4.0);
  Con.printf("[BUS] rtc_gpio_deinit() GPIO%d   : %.1f us (LP-capable pin: %s)\n", RTC_IO_PIN,
             (t5 - t4) / 4.0, lp ? "yes" : "no");
  Con.printf("[BUS] -> a pinMode()-based driver needs ~%.1f ms per transfer (2 pinMode calls)\n",
             2 * (t4 - t3) / 4000.0);
}

// =====================================================================
//                              Setup / Loop
// =====================================================================
void setup() {
  Con.begin();
  delay(1500);
  WiFi.mode(WIFI_OFF); // offline unless a command needs the network

  printHelp();

  g_rtcOk = init_rtc();
  if (g_rtcOk) {
    bool ram = rtcRamTest();
    Con.printf("[RTC] RAM write/read-back test: %s\n", ram ? "PASS" : "FAIL");
    g_syncEpoch = loadSyncEpoch();
    printLastSync();
    dumpRegisters();
  } else {
    Con.println("[RTC] Check: CE=GPIO0  IO=GPIO4  CLK=GPIO5, VCC, crystal");
    dumpRegisters(); // begin() already set pin modes; raw values help diagnose
  }
  g_lastStatMs = millis();
}

void loop() {
  // ---- Serial commands ----
  while (Con.available()) {
    char c = toupper(Con.read());
    if (c == 'U') cmdUpdate();
    else if (c == 'C') cmdCheck();
    else if (c == 'B') cmdBenchmark();
    else if (c == 'D') cmdDestructive();
    else if (c == 'W') {
      g_wifiKeep = !g_wifiKeep;
      Con.printf("[WiFi] Keep on between commands: %s\n", g_wifiKeep ? "yes" : "no");
      if (g_wifiKeep) netUp();
      else netDown(true);
    }
    else if (c == 'S') printStat();
    else if (c == 'E') cmdBusTiming();
    else if (c == 'T') {
      Con.println("\n>>> T: Bus integrity test");
      BusResult r;
      runBusTest(&r);
      printBus("BUS", r);
      ramAliasProbe("BUS");
      g_lastPrintedEpoch = 0;
    }
    else if (c == 'R') dumpRegisters();
    else if (c == 'L') printLastSync();
    else if (c == 'H') printHelp();
  }

  if (millis() - g_lastStatMs >= STAT_INTERVAL_MS) {
    g_lastStatMs = millis();
    printStat();
  }

  // ---- External RTC time: continuous print, one line per second ----
  // Printed on every RTC second change; if the RTC has not ticked for 1.1 s
  // the line is printed anyway and flagged, so the output never stops.
  if (millis() - g_lastPollMs < 100) return;
  g_lastPollMs = millis();

  uint32_t epoch;
  bool readOk = rtcReadEpoch(&epoch);
  bool ticked = readOk && epoch != g_lastPrintedEpoch;
  if (!ticked && millis() - g_lastPrintMs < 1100) return;
  g_lastPrintMs = millis();

  if (!readOk) {
    g_readErrors++;
    Con.println("[RTC ] E: Read failed / invalid data (check IC & wiring)");
    return;
  }
  if (ticked && g_lastPrintedEpoch != 0 && epoch - g_lastPrintedEpoch != 1) g_tickJumps++;
  g_lastPrintedEpoch = epoch;

  if (!rtc.isRunning()) {
    printTimeLine(epoch, "RTC*", "  <- HALTED (CH=1), press 'U'");
  } else if (!ticked) {
    g_tickStalls++;
    printTimeLine(epoch, "RTC*", "  <- NOT TICKING (check crystal)");
  } else if (epoch < MIN_VALID_EPOCH) {
    printTimeLine(epoch, "RTC*", "  <- time not set, press 'U'");
  } else {
    char last[72];
    fmtLastSync(epoch, last, sizeof(last));
    printTimeLine(epoch, "RTC ", last);
  }
}
