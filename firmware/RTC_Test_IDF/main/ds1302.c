#include "ds1302.h"

#include <string.h>
#include "esp_rom_sys.h"

#define T_CC_US 4
#define T_CWH_US 4
#define T_CLK_US 1

static uint8_t bcd_to_dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec_to_bcd(int d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static bool bcd_ok(uint8_t b, uint8_t lo, uint8_t hi)
{
    if ((b & 0x0F) > 9 || (b >> 4) > 9) {
        return false;
    }
    uint8_t d = bcd_to_dec(b);
    return d >= lo && d <= hi;
}

static void xfer_start(const ds1302_t *dev)
{
    gpio_set_level(dev->clk, 0);
    gpio_set_direction(dev->io, GPIO_MODE_OUTPUT);
    gpio_set_level(dev->ce, 1);
    esp_rom_delay_us(T_CC_US);
}

static void xfer_stop(const ds1302_t *dev)
{
    gpio_set_level(dev->ce, 0);
    gpio_set_direction(dev->io, GPIO_MODE_OUTPUT);
    gpio_set_level(dev->io, 0);
    esp_rom_delay_us(T_CWH_US);
}

static void tx(const ds1302_t *dev, uint8_t v, bool then_read)
{
    for (int i = 0; i < 8; i++) {
        gpio_set_level(dev->io, (v >> i) & 1);
        esp_rom_delay_us(T_CLK_US);
        gpio_set_level(dev->clk, 1);
        esp_rom_delay_us(T_CLK_US);
        if (then_read && i == 7) {
            gpio_set_direction(dev->io, GPIO_MODE_INPUT);
        }
        gpio_set_level(dev->clk, 0);
    }
    esp_rom_delay_us(T_CLK_US);
}

static uint8_t rx(const ds1302_t *dev)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (gpio_get_level(dev->io)) {
            v |= 1 << i;
        }
        gpio_set_level(dev->clk, 1);
        esp_rom_delay_us(T_CLK_US);
        gpio_set_level(dev->clk, 0);
        esp_rom_delay_us(T_CLK_US);
    }
    return v;
}

static uint8_t read_cmd(const ds1302_t *dev, uint8_t cmd)
{
    xfer_start(dev);
    tx(dev, cmd, true);
    uint8_t v = rx(dev);
    xfer_stop(dev);
    return v;
}

static void write_cmd(const ds1302_t *dev, uint8_t cmd, uint8_t v)
{
    xfer_start(dev);
    tx(dev, cmd, false);
    tx(dev, v, false);
    xfer_stop(dev);
}

esp_err_t ds1302_init(const ds1302_t *dev)
{
    gpio_config_t c = {
        .pin_bit_mask = (1ULL << dev->clk) | (1ULL << dev->io) | (1ULL << dev->ce),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&c);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(dev->ce, 0);
    gpio_set_level(dev->clk, 0);
    gpio_set_level(dev->io, 0);
    esp_rom_delay_us(T_CWH_US);

    if (ds1302_read_reg(dev, 5) & 0xF8) {
        return ESP_ERR_NOT_FOUND;
    }
    ds1302_write_reg(dev, 7, 0x00);
    return (ds1302_read_reg(dev, 7) & 0x80) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

uint8_t ds1302_read_reg(const ds1302_t *dev, uint8_t reg)
{
    return read_cmd(dev, 0x81 | (reg << 1));
}

void ds1302_write_reg(const ds1302_t *dev, uint8_t reg, uint8_t value)
{
    write_cmd(dev, 0x80 | (reg << 1), value);
}

uint8_t ds1302_read_ram(const ds1302_t *dev, uint8_t addr)
{
    return read_cmd(dev, 0xC1 | (addr << 1));
}

void ds1302_write_ram(const ds1302_t *dev, uint8_t addr, uint8_t value)
{
    write_cmd(dev, 0xC0 | (addr << 1), value);
}

bool ds1302_is_running(const ds1302_t *dev)
{
    return (ds1302_read_reg(dev, 0) & 0x80) == 0;
}

bool ds1302_clock_enable(const ds1302_t *dev)
{
    ds1302_write_reg(dev, 0, ds1302_read_reg(dev, 0) & 0x7F);
    return ds1302_is_running(dev);
}

bool ds1302_get_time(const ds1302_t *dev, struct tm *t)
{
    uint8_t b[8];
    xfer_start(dev);
    tx(dev, 0xBF, true);
    for (int i = 0; i < 8; i++) {
        b[i] = rx(dev);
    }
    xfer_stop(dev);

    memset(t, 0, sizeof(*t));
    if ((b[0] & 0x80) || (b[2] & 0x80)) {
        return false;
    }
    if (!bcd_ok(b[0], 0, 59) || !bcd_ok(b[1], 0, 59) || !bcd_ok(b[2], 0, 23) || !bcd_ok(b[3], 1, 31) ||
        !bcd_ok(b[4], 1, 12) || !bcd_ok(b[5], 1, 7) || !bcd_ok(b[6], 0, 99)) {
        return false;
    }
    t->tm_sec = bcd_to_dec(b[0]);
    t->tm_min = bcd_to_dec(b[1]);
    t->tm_hour = bcd_to_dec(b[2]);
    t->tm_mday = bcd_to_dec(b[3]);
    t->tm_mon = bcd_to_dec(b[4]) - 1;
    t->tm_wday = bcd_to_dec(b[5]) - 1;
    t->tm_year = bcd_to_dec(b[6]) + 100;
    return true;
}

void ds1302_set_time(const ds1302_t *dev, const struct tm *t)
{
    const uint8_t b[8] = {
        dec_to_bcd(t->tm_sec), dec_to_bcd(t->tm_min), dec_to_bcd(t->tm_hour), dec_to_bcd(t->tm_mday),
        dec_to_bcd(t->tm_mon + 1), dec_to_bcd(t->tm_wday + 1), dec_to_bcd(t->tm_year % 100), 0,
    };
    xfer_start(dev);
    tx(dev, 0xBE, false);
    for (int i = 0; i < 8; i++) {
        tx(dev, b[i], false);
    }
    xfer_stop(dev);
}
