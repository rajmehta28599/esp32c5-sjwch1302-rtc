#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t clk;
    gpio_num_t io;
    gpio_num_t ce;
} ds1302_t;

esp_err_t ds1302_init(const ds1302_t *dev);
uint8_t ds1302_read_reg(const ds1302_t *dev, uint8_t reg);
void ds1302_write_reg(const ds1302_t *dev, uint8_t reg, uint8_t value);
uint8_t ds1302_read_ram(const ds1302_t *dev, uint8_t addr);
void ds1302_write_ram(const ds1302_t *dev, uint8_t addr, uint8_t value);
bool ds1302_is_running(const ds1302_t *dev);
bool ds1302_clock_enable(const ds1302_t *dev);
bool ds1302_get_time(const ds1302_t *dev, struct tm *t);
void ds1302_set_time(const ds1302_t *dev, const struct tm *t);
