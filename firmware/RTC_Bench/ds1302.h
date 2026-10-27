// Minimal DS1302 3-wire driver (also for pin-compatible parts such as the
// SJWCH1302Q-W) with datasheet bus timing at VCC = 2.0 V, the worst case.
//
// Why not pinMode(): on Arduino-ESP32 3.3.x for the ESP32-C5, pinMode()
// calls gpio_config(), which calls rtc_gpio_deinit() for LP-capable pins
// (GPIO0..6) and takes ~12 ms per call. A driver that turns the IO line
// around with pinMode() therefore blocks ~25 ms per transfer. The direction
// change here uses gpio_set_direction() (a few microseconds).
//
// Register conventions match ErriezDS1302: UTC in 24 h mode, day-of-week
// register 1..7 = Sunday..Saturday, write protect cleared on every write.
#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include <rom/ets_sys.h>
#include <time.h>

class Ds1302 {
public:
  // DS1302 AC characteristics at VCC = 2.0 V, rounded up to whole us
  static constexpr uint32_t T_CC_US = 4;  // CE to CLK setup
  static constexpr uint32_t T_CWH_US = 4; // CE inactive time
  static constexpr uint32_t T_CLK_US = 1; // CLK high / low time (1000 ns), covers tCDD 800 ns

  Ds1302(uint8_t clkPin, uint8_t ioPin, uint8_t cePin)
    : _clk((gpio_num_t)clkPin), _io((gpio_num_t)ioPin), _ce((gpio_num_t)cePin) {}

  // Configures the pins (once) and clears write protect.
  // false when no DS1302 answers (day-of-week register has invalid bits).
  bool begin() {
    gpio_config_t c = {};
    c.pin_bit_mask = (1ULL << _clk) | (1ULL << _io) | (1ULL << _ce);
    c.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&c) != ESP_OK) return false;
    gpio_set_level(_ce, 0);
    gpio_set_level(_clk, 0);
    gpio_set_level(_io, 0);
    esp_rom_delay_us(T_CWH_US);
    if (readRegister(5) & 0xF8) return false;
    writeRegister(7, 0x00);
    return (readRegister(7) & 0x80) == 0;
  }

  // ---------------- Single registers (reg 0..8, RAM addr 0..30) ----------------
  uint8_t readRegister(uint8_t reg) { return readCmd(0x81 | (reg << 1)); }
  void writeRegister(uint8_t reg, uint8_t v) { writeCmd(0x80 | (reg << 1), v); }
  uint8_t readByteRAM(uint8_t addr) { return readCmd(0xC1 | (addr << 1)); }
  void writeByteRAM(uint8_t addr, uint8_t v) { writeCmd(0xC0 | (addr << 1), v); }

  // ---------------- Burst transfers ----------------
  // Clock burst: sec, min, hour, date, month, day, year, WP (8 bytes).
  // The DS1302 copies the clock into its read buffer on the rising edge of CE,
  // so a burst read is always consistent.
  void readBuffer(void *buf, uint8_t len) { readBurst(0xBF, (uint8_t *)buf, len); }
  // A clock burst write must contain all 8 bytes to take effect. Writing the
  // seconds register resets the countdown chain when CE falls.
  void writeBuffer(const void *buf, uint8_t len) { writeBurst(0xBE, (const uint8_t *)buf, len); }
  void readBufferRAM(uint8_t *buf, uint8_t len) { readBurst(0xFF, buf, len); }
  void writeBufferRAM(const uint8_t *buf, uint8_t len) { writeBurst(0xFE, buf, len); }

  // ---------------- Clock ----------------
  bool isRunning() { return (readRegister(0) & 0x80) == 0; }

  bool clockEnable(bool enable = true) {
    uint8_t s = readRegister(0);
    writeRegister(0, enable ? (s & 0x7F) : (s | 0x80));
    return isRunning() == enable;
  }

  // Burst read -> struct tm (UTC). false on invalid BCD, CH set or 12 h mode.
  bool read(struct tm *dt) {
    uint8_t b[8];
    readBuffer(b, sizeof(b));
    memset(dt, 0, sizeof(*dt));
    if ((b[0] & 0x80) || (b[2] & 0x80)) return false;
    if (!bcdOk(b[0], 0, 59) || !bcdOk(b[1], 0, 59) || !bcdOk(b[2], 0, 23) || !bcdOk(b[3], 1, 31) ||
        !bcdOk(b[4], 1, 12) || !bcdOk(b[5], 1, 7) || !bcdOk(b[6], 0, 99))
      return false;
    dt->tm_sec = dec(b[0]);
    dt->tm_min = dec(b[1]);
    dt->tm_hour = dec(b[2]);
    dt->tm_mday = dec(b[3]);
    dt->tm_mon = dec(b[4]) - 1;
    dt->tm_wday = dec(b[5]) - 1;
    dt->tm_year = dec(b[6]) + 100;
    return true;
  }

  // struct tm (UTC) -> burst write, 24 h mode, oscillator enabled, WP = 0
  bool write(const struct tm *dt) {
    uint8_t b[8] = {bcd(dt->tm_sec), bcd(dt->tm_min), bcd(dt->tm_hour), bcd(dt->tm_mday),
                    bcd(dt->tm_mon + 1), bcd(dt->tm_wday + 1), bcd(dt->tm_year % 100), 0};
    writeBuffer(b, sizeof(b));
    return true;
  }

private:
  gpio_num_t _clk, _io, _ce;

  static uint8_t dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
  static uint8_t bcd(int d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }
  static bool bcdOk(uint8_t b, uint8_t lo, uint8_t hi) {
    return (b & 0x0F) <= 9 && (b >> 4) <= 9 && dec(b) >= lo && dec(b) <= hi;
  }

  void start() {
    gpio_set_level(_clk, 0);
    gpio_set_direction(_io, GPIO_MODE_OUTPUT);
    gpio_set_level(_ce, 1);
    esp_rom_delay_us(T_CC_US);
  }

  void stop() {
    gpio_set_level(_ce, 0);
    gpio_set_direction(_io, GPIO_MODE_OUTPUT);
    gpio_set_level(_io, 0);
    esp_rom_delay_us(T_CWH_US);
  }

  // LSB first; the DS1302 samples IO on the rising CLK edge. For a read
  // command the IO line is released before the 8th falling edge, after
  // which the DS1302 drives the first data bit.
  void tx(uint8_t v, bool thenRead = false) {
    for (int i = 0; i < 8; i++) {
      gpio_set_level(_io, (v >> i) & 1);
      esp_rom_delay_us(T_CLK_US); // data setup + CLK low time
      gpio_set_level(_clk, 1);
      esp_rom_delay_us(T_CLK_US); // CLK high time + hold
      if (thenRead && i == 7) gpio_set_direction(_io, GPIO_MODE_INPUT);
      gpio_set_level(_clk, 0);
    }
    esp_rom_delay_us(T_CLK_US); // CLK low time, first read bit valid (tCDD)
  }

  // Data bits are driven on falling CLK edges, LSB first
  uint8_t rx() {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
      if (gpio_get_level(_io)) v |= 1 << i;
      gpio_set_level(_clk, 1);
      esp_rom_delay_us(T_CLK_US);
      gpio_set_level(_clk, 0);
      esp_rom_delay_us(T_CLK_US);
    }
    return v;
  }

  uint8_t readCmd(uint8_t cmd) {
    start();
    tx(cmd, true);
    uint8_t v = rx();
    stop();
    return v;
  }

  void writeCmd(uint8_t cmd, uint8_t v) {
    start();
    tx(cmd);
    tx(v);
    stop();
  }

  void readBurst(uint8_t cmd, uint8_t *buf, uint8_t len) {
    start();
    tx(cmd, true);
    for (uint8_t i = 0; i < len; i++) buf[i] = rx();
    stop();
  }

  void writeBurst(uint8_t cmd, const uint8_t *buf, uint8_t len) {
    start();
    tx(cmd);
    for (uint8_t i = 0; i < len; i++) tx(buf[i]);
    stop();
  }
};
