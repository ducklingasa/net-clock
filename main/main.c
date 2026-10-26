#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "weather_icons.h"

/* Set to 1 to enable serial logging (also set the console/bootloader log in sdkconfig). */
#define ENABLE_SERIAL_LOG 0

#if ENABLE_SERIAL_LOG
/* GPIO 3 (UART0 RX) is used by the console, so the reset line moves elsewhere. */
#define PIN_SCK  18
#define PIN_MOSI 23
#define PIN_DC   26
#define PIN_CS   5
#define PIN_RST  33
#else
#define PIN_SCK  16
#define PIN_MOSI 17
#define PIN_DC   21
#define PIN_CS   22
#define PIN_RST  3

#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#define ESP_LOGE(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#endif

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240
#define CENTER_X       120
#define CENTER_Y       120

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10
#define WEATHER_REFRESH_SECONDS (10 * 60)
#define WEATHER_RETRY_SECONDS   60
#define WEATHER_RESPONSE_MAX    2048

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define SWAP16(value) (uint16_t)((((value) & 0xFF) << 8) | (((value) >> 8) & 0xFF))

static const char *TAG = "net_clock";
static const uint16_t DIAL_COLOR = RGB565(90, 90, 100);
static const uint16_t MIN_COLOR = RGB565(0, 225, 240);
static const uint16_t SECOND_COLOR = RGB565(255, 255, 255);
static const uint16_t FIVE_SECOND_COLOR = RGB565(0, 255, 30);

typedef struct {
    int temperature;
    int humidity;
    char icon[4];
    time_t sunrise;
    time_t sunset;
    bool valid;
} weather_reading_t;

static spi_device_handle_t display_spi;
static uint16_t *framebuffer;
static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;
static weather_reading_t current_weather;
static portMUX_TYPE weather_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t spi_write_selected(bool data_mode, const void *data, size_t length)
{
    gpio_set_level(PIN_DC, data_mode ? 1 : 0);

    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_transmit(display_spi, &transaction);
}

static esp_err_t display_command(uint8_t command, const uint8_t *data, size_t length)
{
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    esp_err_t result = spi_write_selected(false, &command, sizeof(command));
    if (result == ESP_OK && length > 0) {
        result = spi_write_selected(true, data, length);
    }
    gpio_set_level(PIN_CS, 1);
    ESP_LOGI(TAG, "display_command 0x%02X (%u data bytes): %s",
             command, (unsigned)length, esp_err_to_name(result));
    return result;
}

static void display_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t columns[] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    const uint8_t rows[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};

    ESP_ERROR_CHECK(display_command(0x2A, columns, sizeof(columns)));
    ESP_ERROR_CHECK(display_command(0x2B, rows, sizeof(rows)));
}

static void display_initialize(void)
{
    ESP_LOGI(TAG, "Display init: SCK=%d MOSI=%d DC=%d CS=%d RST=%d, %dx%d",
             PIN_SCK, PIN_MOSI, PIN_DC, PIN_CS, PIN_RST,
             DISPLAY_WIDTH, DISPLAY_HEIGHT);
    gpio_config_t output_pins = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_CS) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_pins));
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_RST, 1);

    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY, /* write-only device; allows 40 MHz on GPIO-matrix pins */
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device_config, &display_spi));
    ESP_LOGI(TAG, "SPI bus initialized and device added at %d Hz",
             device_config.clock_speed_hz);

    ESP_LOGI(TAG, "Pulsing reset pin %d", PIN_RST);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_ERROR_CHECK(display_command(0x01, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    static const struct {
        uint8_t command;
        uint8_t length;
        uint8_t data[12];
    } gc9a01_init[] = {
        {0xEF, 0, {0}},
        {0xEB, 1, {0x14}},
        {0xFE, 0, {0}},
        {0xEF, 0, {0}},
        {0xEB, 1, {0x14}},
        {0x84, 1, {0x40}},
        {0x85, 1, {0xFF}},
        {0x86, 1, {0xFF}},
        {0x87, 1, {0xFF}},
        {0x88, 1, {0x0A}},
        {0x89, 1, {0x21}},
        {0x8A, 1, {0x00}},
        {0x8B, 1, {0x80}},
        {0x8C, 1, {0x01}},
        {0x8D, 1, {0x01}},
        {0x8E, 1, {0xFF}},
        {0x8F, 1, {0xFF}},
        {0xB6, 2, {0x00, 0x00}},
        {0x36, 1, {0x48}},
        {0x3A, 1, {0x55}},
        {0x90, 4, {0x08, 0x08, 0x08, 0x08}},
        {0xBD, 1, {0x06}},
        {0xBC, 1, {0x00}},
        {0xFF, 3, {0x60, 0x01, 0x04}},
        {0xC3, 1, {0x13}},
        {0xC4, 1, {0x13}},
        {0xC9, 1, {0x22}},
        {0xBE, 1, {0x11}},
        {0xE1, 2, {0x10, 0x0E}},
        {0xDF, 3, {0x21, 0x0C, 0x02}},
        {0xF0, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
        {0xF1, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}},
        {0xF2, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}},
        {0xF3, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}},
        {0xED, 2, {0x1B, 0x0B}},
        {0xAE, 1, {0x77}},
        {0xCD, 1, {0x63}},
        {0x70, 9, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}},
        {0xE8, 1, {0x34}},
        {0x62, 12, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}},
        {0x63, 12, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}},
        {0x64, 7, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}},
        {0x66, 10, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}},
        {0x67, 10, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}},
        {0x74, 7, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}},
        {0x98, 2, {0x3E, 0x07}},
        {0x35, 0, {0}},
        {0x21, 0, {0}},
    };
    for (size_t i = 0; i < sizeof(gc9a01_init) / sizeof(gc9a01_init[0]); ++i) {
        ESP_ERROR_CHECK(display_command(gc9a01_init[i].command, gc9a01_init[i].data,
                                        gc9a01_init[i].length));
    }

    ESP_ERROR_CHECK(display_command(0x11, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(display_command(0x29, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "Display initialization sequence finished");
}

static void set_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT) {
        framebuffer[y * DISPLAY_WIDTH + x] = SWAP16(color);
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    const int dx = abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled_error = 2 * error;
        if (doubled_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void fill_rectangle(int x, int y, int width, int height, uint16_t color)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            set_pixel(column, row, color);
        }
    }
}

static void draw_circle(int center_x, int center_y, int radius, uint16_t color)
{
    int x = radius;
    int y = 0;
    int error = 1 - radius;

    while (x >= y) {
        set_pixel(center_x + x, center_y + y, color);
        set_pixel(center_x + y, center_y + x, color);
        set_pixel(center_x - y, center_y + x, color);
        set_pixel(center_x - x, center_y + y, color);
        set_pixel(center_x - x, center_y - y, color);
        set_pixel(center_x - y, center_y - x, color);
        set_pixel(center_x + y, center_y - x, color);
        set_pixel(center_x + x, center_y - y, color);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}

static void fill_circle(int center_x, int center_y, int radius, uint16_t color)
{
    for (int y = -radius; y <= radius; ++y) {
        const int half_width = (int)sqrtf((float)(radius * radius - y * y));
        draw_line(center_x - half_width, center_y + y,
                  center_x + half_width, center_y + y, color);
    }
}

static const char font_characters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.%";
static const uint8_t font5x7[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x63, 0x13, 0x08, 0x64, 0x63},
};

static const uint8_t *find_glyph(char character)
{
    const char *match = strchr(font_characters, character);
    return match == NULL ? NULL : font5x7[match - font_characters];
}

static int text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 - 1) * scale;
}

static void draw_text(const char *text, int x, int y, int scale, uint16_t color)
{
    for (; *text != '\0'; ++text, x += 6 * scale) {
        const uint8_t *glyph = find_glyph(*text);
        if (glyph == NULL) {
            continue;
        }
        for (int column = 0; column < 5; ++column) {
            for (int row = 0; row < 7; ++row) {
                if (glyph[column] & (1U << row)) {
                    fill_rectangle(x + column * scale, y + row * scale,
                                   scale, scale, color);
                }
            }
        }
    }
}

static const weather_icon_t *weather_icon_for_code(const char *code)
{
    const bool is_day = code[2] == 'd';
    if (strncmp(code, "01", 2) == 0) {
        return is_day ? &weather_icon_clear_day : &weather_icon_clear_night;
    }
    if (strncmp(code, "02", 2) == 0) {
        return is_day ? &weather_icon_few_day : &weather_icon_few_night;
    }
    if (strncmp(code, "03", 2) == 0) {
        return is_day ? &weather_icon_scattered_day : &weather_icon_few_night;
    }
    if (strncmp(code, "04", 2) == 0) {
        return is_day ? &weather_icon_broken_day : &weather_icon_few_night;
    }
    if (strncmp(code, "09", 2) == 0) {
        return is_day ? &weather_icon_shower_day : &weather_icon_shower_night;
    }
    if (strncmp(code, "10", 2) == 0) {
        return is_day ? &weather_icon_rain_day : &weather_icon_rain_night;
    }
    if (strncmp(code, "11", 2) == 0) {
        return is_day ? &weather_icon_thunder_day : &weather_icon_thunder_night;
    }
    if (strncmp(code, "13", 2) == 0) {
        return is_day ? &weather_icon_snow_day : &weather_icon_snow_night;
    }
    if (strncmp(code, "50", 2) == 0) {
        return is_day ? &weather_icon_mist_day : &weather_icon_mist_night;
    }
    return NULL;
}

static const weather_icon_t *weather_icon_for_reading(const weather_reading_t *weather,
                                                     time_t now)
{
    const bool clear_or_few = strncmp(weather->icon, "01", 2) == 0 ||
                              strncmp(weather->icon, "02", 2) == 0;
    if (clear_or_few) {
        if (weather->sunrise > 0 && fabs(difftime(now, weather->sunrise)) <= 30 * 60) {
            return &weather_icon_sunrise;
        }
        if (weather->sunset > 0 && fabs(difftime(now, weather->sunset)) <= 30 * 60) {
            return &weather_icon_sunset;
        }
    }
    return weather_icon_for_code(weather->icon);
}

static void draw_weather_icon(const weather_icon_t *icon, int x, int y)
{
    if (icon == NULL) {
        return;
    }
    const uint8_t *encoded = icon->rle;
    for (int row = 0; row < WEATHER_ICON_SIZE; ++row) {
        const uint8_t span_count = *encoded++;
        for (uint8_t span = 0; span < span_count; ++span) {
            const int start = *encoded++;
            const uint8_t length = *encoded++;
            for (uint8_t offset = 0; offset < length; ++offset) {
                const uint8_t color_index = *encoded++;
                const uint16_t color = weather_icon_palette[color_index - 1];
                set_pixel(x + start + offset, y + row, color);
            }
        }
    }
}

static void draw_droplet(int x, int y, uint16_t color)
{
    for (int row = 0; row <= 24; ++row) {
        const float half_width = row <= 15
            ? 9.0f * sinf(row * 3.14159265f / 30.0f)
            : sqrtf(81.0f - (row - 15) * (row - 15));
        const int width = (int)lroundf(half_width);
        draw_line(x - width, y + row, x + width, y + row, color);
    }
    draw_line(x - 5, y + 15, x - 5, y + 18, RGB565(100, 180, 255));
    set_pixel(x - 4, y + 19, RGB565(100, 180, 255));
}

static void draw_hour_marker(int tick, uint16_t color)
{
    const float angle = (tick * 6.0f - 90.0f) * 3.14159265f / 180.0f;
    const float dx = cosf(angle);
    const float dy = sinf(angle);
    const int left = CENTER_X + (int)floorf(fminf(101 * dx, 112 * dx)) - 2;
    const int right = CENTER_X + (int)ceilf(fmaxf(101 * dx, 112 * dx)) + 2;
    const int top = CENTER_Y + (int)floorf(fminf(101 * dy, 112 * dy)) - 2;
    const int bottom = CENTER_Y + (int)ceilf(fmaxf(101 * dy, 112 * dy)) + 2;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const float along = (x - CENTER_X) * dx + (y - CENTER_Y) * dy;
            const float across = -(x - CENTER_X) * dy + (y - CENTER_Y) * dx;
            if (along >= 101.0f && along <= 112.0f && fabsf(across) < 1.5f) {
                set_pixel(x, y, color);
            }
        }
    }
}

static void draw_minute_marker(int tick, uint16_t color, int thickness)
{
    const float angle = (tick * 6.0f - 90.0f) * 3.14159265f / 180.0f;
    const int x1 = CENTER_X + (int)(cosf(angle) * 107);
    const int y1 = CENTER_Y + (int)(sinf(angle) * 107);
    const int x2 = CENTER_X + (int)(cosf(angle) * 112);
    const int y2 = CENTER_Y + (int)(sinf(angle) * 112);

    if (thickness == 1) {
        draw_line(x1, y1, x2, y2, color);
        return;
    }

    const int offset_x = (int)lroundf(-sinf(angle));
    const int offset_y = (int)lroundf(cosf(angle));
    for (int offset = -1; offset <= 1; ++offset) {
        draw_line(x1 + offset * offset_x, y1 + offset * offset_y,
                  x2 + offset * offset_x, y2 + offset * offset_y, color);
    }
}

static void draw_clock_face(const struct tm *time_info, const weather_reading_t *weather,
                            time_t now, bool colon_white)
{
    memset(framebuffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer));
    draw_circle(CENTER_X, CENTER_Y, 118, DIAL_COLOR);
    draw_circle(CENTER_X, CENTER_Y, 117, DIAL_COLOR);

    for (int tick = 0; tick < 60; ++tick) {
        const bool major = tick % 5 == 0;
        const bool elapsed = time_info != NULL &&
            (time_info->tm_sec == 0 || (tick > 0 && tick <= time_info->tm_sec));
        const uint16_t color = elapsed
            ? (major ? FIVE_SECOND_COLOR : SECOND_COLOR)
            : DIAL_COLOR;
        if (major) {
            draw_hour_marker(tick, color);
            continue;
        }
        draw_minute_marker(tick, color, elapsed ? 3 : 1);
    }

    if (time_info != NULL) {
        char date_text[16];
        char time_text[6];
        strftime(date_text, sizeof(date_text), "%a %d", time_info);
        strftime(time_text, sizeof(time_text), "%H:%M", time_info);
        for (char *character = date_text; *character != '\0'; ++character) {
            if (*character >= 'a' && *character <= 'z') {
                *character = (char)(*character - 'a' + 'A');
            }
        }
        draw_text(date_text, (DISPLAY_WIDTH - text_width(date_text, 3)) / 2,
                  43, 3, MIN_COLOR);
        draw_text(time_text, (DISPLAY_WIDTH - text_width(time_text, 6)) / 2,
                  78, 6, RGB565(255, 255, 255));
        if (!colon_white) {
            const int colon_x = (DISPLAY_WIDTH - text_width(time_text, 6)) / 2 + 2 * 6 * 6;
            draw_text(":", colon_x, 78, 6, DIAL_COLOR);
        }
    }

    if (weather != NULL && weather->valid) {
        draw_weather_icon(weather_icon_for_reading(weather, now), 54, 128);
    }

    char temperature_text[8];
    char humidity_text[8];
    if (weather != NULL && weather->valid) {
        snprintf(temperature_text, sizeof(temperature_text), "%d", weather->temperature);
        snprintf(humidity_text, sizeof(humidity_text), "%d%%", weather->humidity);
    } else {
        strlcpy(temperature_text, "--", sizeof(temperature_text));
        strlcpy(humidity_text, "--%", sizeof(humidity_text));
    }
    const int temperature_scale = strlen(temperature_text) > 3 ? 3 : 4;
    const int temperature_x = 119;
    const int temperature_y = 139;
    const int temperature_width = text_width(temperature_text, temperature_scale);
    draw_text(temperature_text, temperature_x, temperature_y, temperature_scale,
              RGB565(255, 20, 20));
    const int degree_x = temperature_x + temperature_width + 5;
    fill_circle(degree_x, temperature_y + 4, 4, RGB565(255, 20, 20));
    fill_circle(degree_x, temperature_y + 4, 2, RGB565(0, 0, 0));
    draw_text("C", degree_x + 8, temperature_y + 7, 3, RGB565(255, 20, 20));

    const int humidity_scale = 3;
    const int humidity_width = text_width(humidity_text, humidity_scale);
    const int humidity_start = (DISPLAY_WIDTH - (humidity_width + 27)) / 2;
    draw_droplet(humidity_start + 9, 180, RGB565(0, 90, 255));
    draw_text(humidity_text, humidity_start + 27, 183, humidity_scale,
              RGB565(0, 90, 255));
}

static void flush_display(void)
{
    display_set_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    const uint8_t memory_write = 0x2C;
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    esp_err_t result = spi_write_selected(false, &memory_write, sizeof(memory_write));
    if (result == ESP_OK) {
        result = spi_write_selected(true, framebuffer,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer));
    }
    gpio_set_level(PIN_CS, 1);
    ESP_LOGI(TAG, "flush_display: %s", esp_err_to_name(result));
    ESP_ERROR_CHECK(result);
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            ESP_ERROR_CHECK(esp_wifi_connect());
            ++wifi_retry_count;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)",
                     wifi_retry_count, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Connected; IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t connect_wifi(void)
{
    if (CONFIG_CLOCK_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Set the Wi-Fi SSID and password in menuconfig before building");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initialization));
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_CLOCK_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_CLOCK_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Could not connect to Wi-Fi after %d attempts", WIFI_MAX_RETRIES);
    return ESP_FAIL;
}

typedef struct {
    char body[WEATHER_RESPONSE_MAX];
    size_t length;
} weather_http_response_t;

static esp_err_t weather_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    weather_http_response_t *response = event->user_data;
    const size_t data_length = (size_t)event->data_len;
    if (data_length >= sizeof(response->body) - response->length) {
        ESP_LOGE(TAG, "OpenWeatherMap response exceeds %u bytes",
                 (unsigned)sizeof(response->body) - 1);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response->body + response->length, event->data, data_length);
    response->length += data_length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_weather(weather_reading_t *reading)
{
    if (CONFIG_CLOCK_OPENWEATHERMAP_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Set the OpenWeatherMap API key in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }

    char url[384];
    const int url_length = snprintf(
        url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&units=metric&appid=%s",
        CONFIG_CLOCK_LATITUDE, CONFIG_CLOCK_LONGITUDE,
        CONFIG_CLOCK_OPENWEATHERMAP_API_KEY);
    if (url_length < 0 || (size_t)url_length >= sizeof(url)) {
        ESP_LOGE(TAG, "OpenWeatherMap request URL exceeds its configured buffer");
        return ESP_ERR_INVALID_SIZE;
    }

    weather_http_response_t response = {0};
    esp_http_client_config_t configuration = {
        .url = url,
        .event_handler = weather_http_event,
        .user_data = &response,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&configuration);
    if (client == NULL) {
        ESP_LOGE(TAG, "Could not initialize the OpenWeatherMap HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "OpenWeatherMap request failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "OpenWeatherMap returned HTTP status %d", status_code);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(response.body);
    if (root == NULL) {
        ESP_LOGE(TAG, "OpenWeatherMap returned invalid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *main_data = cJSON_GetObjectItemCaseSensitive(root, "main");
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(main_data, "temp");
    cJSON *humidity = cJSON_GetObjectItemCaseSensitive(main_data, "humidity");
    cJSON *conditions = cJSON_GetObjectItemCaseSensitive(root, "weather");
    cJSON *condition = cJSON_GetArrayItem(conditions, 0);
    cJSON *icon = cJSON_GetObjectItemCaseSensitive(condition, "icon");
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(humidity) ||
        !cJSON_IsString(icon) || icon->valuestring == NULL ||
        strlen(icon->valuestring) != 3) {
        ESP_LOGE(TAG, "OpenWeatherMap response is missing required weather fields");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    reading->temperature = (int)lround(temperature->valuedouble);
    reading->humidity = humidity->valueint;
    strlcpy(reading->icon, icon->valuestring, sizeof(reading->icon));
    cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "sys");
    cJSON *sunrise = cJSON_GetObjectItemCaseSensitive(sys, "sunrise");
    cJSON *sunset = cJSON_GetObjectItemCaseSensitive(sys, "sunset");
    if (cJSON_IsNumber(sunrise) && cJSON_IsNumber(sunset) &&
        isfinite(sunrise->valuedouble) && isfinite(sunset->valuedouble) &&
        sunrise->valuedouble > 0 && sunrise->valuedouble < 4102444800.0 &&
        sunset->valuedouble > sunrise->valuedouble && sunset->valuedouble < 4102444800.0) {
        reading->sunrise = (time_t)sunrise->valuedouble;
        reading->sunset = (time_t)sunset->valuedouble;
    } else {
        reading->sunrise = 0;
        reading->sunset = 0;
        ESP_LOGW(TAG, "Weather response has no valid sunrise/sunset times");
    }
    reading->valid = true;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Weather updated: %d C, humidity %d%%",
             reading->temperature, reading->humidity);
    return ESP_OK;
}

static void weather_task(void *argument)
{
    (void)argument;
    while (true) {
        const time_t now = time(NULL);
        if (now < 1577836800) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        weather_reading_t updated = {0};
        const esp_err_t result = fetch_weather(&updated);
        if (result == ESP_OK) {
            portENTER_CRITICAL(&weather_mux);
            current_weather = updated;
            portEXIT_CRITICAL(&weather_mux);
            vTaskDelay(pdMS_TO_TICKS(WEATHER_REFRESH_SECONDS * 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(WEATHER_RETRY_SECONDS * 1000));
        }
    }
}

static void start_time_sync(void)
{
    setenv("TZ", CONFIG_CLOCK_TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_CLOCK_NTP_SERVER);
    esp_sntp_init();
    ESP_LOGI(TAG, "Synchronizing time with %s", CONFIG_CLOCK_NTP_SERVER);
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    framebuffer = heap_caps_malloc(
        DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer),
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate the display framebuffer");
        return;
    }

    ESP_LOGI(TAG, "Framebuffer allocated: %p (%u bytes), free heap %u",
             (void *)framebuffer,
             (unsigned)(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer)),
             (unsigned)esp_get_free_heap_size());

    display_initialize();
    draw_clock_face(NULL, NULL, 0, false);
    ESP_LOGI(TAG, "Clock face drawn; first pixel=0x%04X, center=0x%04X",
             framebuffer[0], framebuffer[CENTER_Y * DISPLAY_WIDTH + CENTER_X]);
    flush_display();

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID '%s'", CONFIG_CLOCK_WIFI_SSID);
    result = connect_wifi();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(result));
        return;
    }
    start_time_sync();
    if (xTaskCreate(weather_task, "weather", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the OpenWeatherMap task");
    }

    while (true) {
        struct tm time_info;
        const time_t now = time(NULL);
        if (now < 1577836800 || localtime_r(&now, &time_info) == NULL) {
            ESP_LOGW(TAG, "Waiting for NTP time synchronization");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        weather_reading_t weather;
        portENTER_CRITICAL(&weather_mux);
        weather = current_weather;
        portEXIT_CRITICAL(&weather_mux);
        const bool colon_white = (esp_timer_get_time() / 500000) % 2 == 0;
        draw_clock_face(&time_info, &weather, now, colon_white);
        flush_display();

        ESP_LOGI(TAG, "Clock frame drawn for %02d:%02d:%02d",
                 time_info.tm_hour, time_info.tm_min, time_info.tm_sec);
        const int delay_ms = 500 - (int)((esp_timer_get_time() / 1000) % 500);
        const TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
    }
}
