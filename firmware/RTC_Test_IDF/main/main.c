#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_rom_sys.h"

#include "ds1302.h"

#define DS1302_CLK_PIN GPIO_NUM_5
#define DS1302_IO_PIN GPIO_NUM_4
#define DS1302_CE_PIN GPIO_NUM_0

#define WIFI_TIMEOUT_MS 20000
#define NTP_TIMEOUT_MS 15000

#define IST_OFFSET_SEC 19800L
#define MIN_VALID_EPOCH 1700000000UL
#define RAM_TEST_ADDR 30
#define RAM_SYNC_ADDR 0
#define RAM_SYNC_MAGIC 0xC5

#define WIFI_CONNECTED_BIT BIT0

static const ds1302_t rtc = {
    .clk = DS1302_CLK_PIN,
    .io = DS1302_IO_PIN,
    .ce = DS1302_CE_PIN,
};

static EventGroupHandle_t s_wifi_events;
static bool s_wifi_inited;
static volatile bool s_wifi_wanted;
static esp_ip4_addr_t s_ip;

static uint32_t s_last_printed_epoch;
static int64_t s_last_poll_ms;
static int64_t s_last_print_ms;
static uint32_t s_sync_epoch;

static int64_t millis(void) { return esp_timer_get_time() / 1000; }

static void con_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void con_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(50));
}

static int con_getc(void)
{
    uint8_t c;
    return usb_serial_jtag_read_bytes(&c, 1, 0) == 1 ? c : -1;
}

static void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    cfg.rx_buffer_size = 256;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static bool rtc_read_epoch(uint32_t *epoch)
{
    struct tm dt;
    if (!ds1302_get_time(&rtc, &dt)) {
        return false;
    }
    *epoch = (uint32_t)days_from_civil(dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday) * 86400UL +
             dt.tm_hour * 3600UL + dt.tm_min * 60UL + dt.tm_sec;
    return true;
}

static bool rtc_init(void)
{
    con_printf("[RTC] I: Initializing DS1302 RTC...\n");
    esp_err_t err = ds1302_init(&rtc);
    if (err != ESP_OK) {
        con_printf("[RTC] E: ds1302_init() failed (%s) - RTC IC not detected\n", esp_err_to_name(err));
        return false;
    }
    if (!ds1302_is_running(&rtc)) {
        con_printf("[RTC] W: Oscillator was HALTED (CH=1), enabling clock. Time invalid -> press 'U'\n");
        if (!ds1302_clock_enable(&rtc)) {
            con_printf("[RTC] E: RTC still not running after clock enable\n");
            return false;
        }
    }
    con_printf("[RTC] I: RTC initialized successfully\n");
    return true;
}

static bool rtc_ram_test(void)
{
    uint8_t saved = ds1302_read_ram(&rtc, RAM_TEST_ADDR);
    bool ok = true;
    const uint8_t patterns[2] = {0xA5, 0x5A};
    for (int i = 0; i < 2; i++) {
        ds1302_write_ram(&rtc, RAM_TEST_ADDR, patterns[i]);
        if (ds1302_read_ram(&rtc, RAM_TEST_ADDR) != patterns[i]) {
            ok = false;
        }
    }
    ds1302_write_ram(&rtc, RAM_TEST_ADDR, saved);
    return ok;
}

static void save_sync_epoch(uint32_t epoch)
{
    for (uint8_t i = 0; i < 4; i++) {
        ds1302_write_ram(&rtc, RAM_SYNC_ADDR + i, (epoch >> (8 * i)) & 0xFF);
    }
    ds1302_write_ram(&rtc, RAM_SYNC_ADDR + 4, RAM_SYNC_MAGIC);
    s_sync_epoch = epoch;
}

static uint32_t load_sync_epoch(void)
{
    if (ds1302_read_ram(&rtc, RAM_SYNC_ADDR + 4) != RAM_SYNC_MAGIC) {
        return 0;
    }
    uint32_t epoch = 0;
    for (uint8_t i = 0; i < 4; i++) {
        epoch |= (uint32_t)ds1302_read_ram(&rtc, RAM_SYNC_ADDR + i) << (8 * i);
    }
    return epoch;
}

static void fmt_ist(uint32_t epoch_utc, char *buf, size_t len)
{
    time_t ist = (time_t)(epoch_utc + IST_OFFSET_SEC);
    struct tm t;
    gmtime_r(&ist, &t);
    snprintf(buf, len, "%02d/%02d/%04d %02d:%02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec);
}

static void print_time_line(uint32_t epoch_utc, const char *tag, const char *note)
{
    char ist[24];
    fmt_ist(epoch_utc, ist, sizeof(ist));
    con_printf("[%s] Unix: %lu | IST: %s%s\n", tag, (unsigned long)epoch_utc, ist, note);
}

static void fmt_last_sync(uint32_t now_epoch, char *buf, size_t len)
{
    if (s_sync_epoch < MIN_VALID_EPOCH) {
        snprintf(buf, len, " | Last U: never");
        return;
    }
    char ist[24];
    fmt_ist(s_sync_epoch, ist, sizeof(ist));
    if (now_epoch < s_sync_epoch) {
        snprintf(buf, len, " | Last U: %s", ist);
        return;
    }
    uint32_t age = now_epoch - s_sync_epoch;
    snprintf(buf, len, " | Last U: %s (%lud %luh %lum ago)", ist, (unsigned long)(age / 86400),
             (unsigned long)(age % 86400 / 3600), (unsigned long)(age % 3600 / 60));
}

static void print_last_sync(void)
{
    uint32_t now = 0;
    if (!rtc_read_epoch(&now)) {
        now = 0;
    }
    char buf[72];
    fmt_last_sync(now, buf, sizeof(buf));
    con_printf("[RTC]%s\n", buf + 2);
}

static void print_help(void)
{
    con_printf("\n========== ESP32-C5 RTC TEST (SJWCH1302 / DS1302) ==========\n");
    con_printf("  U : Update RTC from WiFi NTP (SSID \"%s\")\n", CONFIG_RTC_WIFI_SSID);
    con_printf("  C : Check RTC drift vs NTP, s + ppm since last 'U' (RTC not written)\n");
    con_printf("  R : Raw register / RAM dump\n");
    con_printf("  L : Show when the RTC was last updated ('U')\n");
    con_printf("  H : This help\n");
    con_printf("  Otherwise time is read only from the external RTC.\n");
    con_printf("=============================================================\n");
}

static void dump_registers(void)
{
    static const char *names[] = {"SEC", "MIN", "HOUR", "DATE", "MONTH", "DAY", "YEAR", "WP", "TCS"};
    con_printf("[RTC] Register dump (raw hex):\n");
    for (uint8_t i = 0; i < 9; i++) {
        con_printf("  %-5s = 0x%02X\n", names[i], ds1302_read_reg(&rtc, i));
    }
    char line[8 + 31 * 3 + 2];
    int n = snprintf(line, sizeof(line), "  RAM   : ");
    for (uint8_t i = 0; i < 31; i++) {
        n += snprintf(line + n, sizeof(line) - n, "%02X ", ds1302_read_ram(&rtc, i));
    }
    con_printf("%s\n", line);

    uint8_t sec = ds1302_read_reg(&rtc, 0), hour = ds1302_read_reg(&rtc, 2), tcs = ds1302_read_reg(&rtc, 8);
    con_printf("  CH=%d (%s), %s mode, trickle charger %s\n", (sec >> 7) & 1, (sec & 0x80) ? "HALTED" : "running",
               (hour & 0x80) ? "12h" : "24h", ((tcs & 0xF0) == 0xA0) ? "ENABLED" : "disabled");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_wifi_wanted) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ip = ((ip_event_got_ip_t *)data)->ip_info.ip;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_once(void)
{
    if (s_wifi_inited) {
        return;
    }
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    s_wifi_inited = true;
}

static bool wifi_connect(void)
{
    wifi_init_once();
    if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) {
        return true;
    }
    con_printf("[WiFi] Connecting to \"%s\" ...\n", CONFIG_RTC_WIFI_SSID);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_RTC_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_RTC_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC,
    };
    esp_err_t err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols);
    if (err != ESP_OK) {
        con_printf("[WiFi] W: esp_wifi_set_protocols: %s\n", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_wifi_wanted = true;
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        con_printf("[WiFi] E: Connect timeout\n");
        return false;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        con_printf("[WiFi] Connected. IP=" IPSTR " RSSI=%d dBm CH=%d\n", IP2STR(&s_ip), ap.rssi, ap.primary);
    }
    return true;
}

static void wifi_off(void)
{
    if (!s_wifi_inited) {
        return;
    }
    s_wifi_wanted = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
}

static bool ntp_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.nist.gov", "time.google.com"));
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        con_printf("[NTP] E: SNTP init failed\n");
        return false;
    }
    con_printf("[NTP] Waiting for sync ...\n");
    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_TIMEOUT_MS)) == ESP_OK &&
              time(NULL) >= (time_t)MIN_VALID_EPOCH;
    esp_netif_sntp_deinit();
    if (!ok) {
        con_printf("[NTP] E: No valid NTP response\n");
    }
    return ok;
}

static void handle_ntp_command(bool write_rtc)
{
    con_printf(write_rtc ? "\n>>> U: Update RTC from NTP\n" : "\n>>> C: Check RTC drift vs NTP\n");

    if (!wifi_connect() || !ntp_sync()) {
        wifi_off();
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t wait_us = 1000000UL - tv.tv_usec;
    vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
    esp_rom_delay_us(wait_us % 1000);
    gettimeofday(&tv, NULL);
    uint32_t ntp_epoch = (uint32_t)tv.tv_sec;
    if (tv.tv_usec > 500000) {
        ntp_epoch++;
    }

    uint32_t rtc_epoch;
    if (ds1302_is_running(&rtc) && rtc_read_epoch(&rtc_epoch) && rtc_epoch >= MIN_VALID_EPOCH) {
        print_time_line(ntp_epoch, "NTP ", "");
        print_time_line(rtc_epoch, "RTC ", "");
        int32_t drift = (int32_t)(rtc_epoch - ntp_epoch);
        con_printf("[DRIFT] RTC - NTP = %+ld s (resolution 1 s)\n", (long)drift);
        uint32_t sync_epoch = load_sync_epoch();
        if (sync_epoch >= MIN_VALID_EPOCH && ntp_epoch > sync_epoch) {
            uint32_t elapsed = ntp_epoch - sync_epoch;
            con_printf("[DRIFT] Since last 'U': %.2f h -> %+.1f ppm (%+.2f s/day)%s\n", elapsed / 3600.0,
                       drift * 1e6 / elapsed, drift * 86400.0 / elapsed,
                       elapsed < 6 * 3600UL ? "  [<6 h: +/-1 s resolution, not reliable yet]" : "");
        }
    } else {
        con_printf("[DRIFT] RTC time not valid, drift not computed\n");
    }

    if (write_rtc) {
        time_t tt = ntp_epoch;
        struct tm t;
        gmtime_r(&tt, &t);
        ds1302_set_time(&rtc, &t);
        if (rtc_read_epoch(&rtc_epoch)) {
            int32_t diff = (int32_t)(rtc_epoch - ntp_epoch);
            print_time_line(rtc_epoch, "RTC ", "");
            con_printf("[RTC] %s (read-back diff %+ld s)\n",
                       (diff >= 0 && diff <= 1) ? "OK: RTC updated + verified" : "E: VERIFY FAILED", (long)diff);
            save_sync_epoch(ntp_epoch);
        } else {
            con_printf("[RTC] E: Read-back after write failed\n");
        }
    }

    wifi_off();
    s_last_printed_epoch = 0;
}

static void handle_command(int c)
{
    switch (toupper(c)) {
    case 'U':
        handle_ntp_command(true);
        break;
    case 'C':
        handle_ntp_command(false);
        break;
    case 'R':
        dump_registers();
        break;
    case 'L':
        print_last_sync();
        break;
    case 'H':
        print_help();
        break;
    default:
        break;
    }
}

static void poll_rtc(void)
{
    if (millis() - s_last_poll_ms < 100) {
        return;
    }
    s_last_poll_ms = millis();

    uint32_t epoch;
    bool read_ok = rtc_read_epoch(&epoch);
    bool ticked = read_ok && epoch != s_last_printed_epoch;
    if (!ticked && millis() - s_last_print_ms < 1100) {
        return;
    }
    s_last_print_ms = millis();

    if (!read_ok) {
        con_printf("[RTC ] E: Read failed / invalid data (check IC & wiring)\n");
        return;
    }
    s_last_printed_epoch = epoch;

    if (!ds1302_is_running(&rtc)) {
        print_time_line(epoch, "RTC*", "  <- HALTED (CH=1), press 'U'");
    } else if (!ticked) {
        print_time_line(epoch, "RTC*", "  <- NOT TICKING (check crystal)");
    } else if (epoch < MIN_VALID_EPOCH) {
        print_time_line(epoch, "RTC*", "  <- time not set, press 'U'");
    } else {
        char last[72];
        fmt_last_sync(epoch, last, sizeof(last));
        print_time_line(epoch, "RTC ", last);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    console_init();
    vTaskDelay(pdMS_TO_TICKS(1500));

    print_help();

    if (rtc_init()) {
        con_printf("[RTC] RAM write/read-back test: %s\n", rtc_ram_test() ? "PASS" : "FAIL");
        s_sync_epoch = load_sync_epoch();
        print_last_sync();
        dump_registers();
    } else {
        con_printf("[RTC] Check: CE=GPIO0  IO=GPIO4  CLK=GPIO5, VCC, crystal\n");
        dump_registers();
    }

    while (true) {
        int c;
        while ((c = con_getc()) >= 0) {
            handle_command(c);
        }
        poll_rtc();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
