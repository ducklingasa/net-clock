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
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "weather_icons.h"

#define PIN_SCL 1
#define PIN_SDA 2
#define PIN_DC  3
#define PIN_CS  4
#define PIN_RST 5
#define PIN_BOOT 0

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240
#define DISPLAY_SPI_MAX_TRANSFER_BYTES 32768
#define CENTER_X       120
#define CENTER_Y       120

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10
#define WEATHER_REFRESH_SECONDS (10 * 60)
#define WEATHER_RETRY_SECONDS   60
#define WEATHER_RESPONSE_MAX    (32 * 1024)
#define WEATHER_FORECAST_HOURS  12
#define ONE_CALL_DAILY_LIMIT    500
#define CLOCK_STATE_NVS_NAMESPACE "clock_state"
#define CLOCK_STATE_NVS_KEY       "forecast_face"
#define CURRENT_ICON_CACHE_CAPACITY 3
#define WEATHER_ICON_PIXELS (WEATHER_ICON_SIZE * WEATHER_ICON_SIZE)
#define FORECAST_ICON_SIZE 32
#define FORECAST_ICON_PIXELS (FORECAST_ICON_SIZE * FORECAST_ICON_SIZE)

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define SWAP16(value) (uint16_t)((((value) & 0xFF) << 8) | (((value) >> 8) & 0xFF))

static const char *TAG = "net_clock";
static const uint16_t DIAL_COLOR = RGB565(90, 90, 100);
static const uint16_t MIN_COLOR = RGB565(0, 225, 240);
static const uint16_t SECOND_COLOR = RGB565(255, 255, 255);
static const uint16_t FIVE_SECOND_COLOR = RGB565(0, 255, 30);
static const uint16_t FORECAST_HOUR_COLOR = RGB565(190, 190, 195);

typedef struct {
    int temperature;
    int humidity;
    char icon[4];
    time_t sunrise;
    time_t sunset;
    bool valid;
} weather_reading_t;

typedef struct {
    time_t timestamp;
    char icon[4];
    bool solar_event;
    bool sunrise_event;
} forecast_hour_t;

typedef struct {
    weather_reading_t current;
    forecast_hour_t hours[WEATHER_FORECAST_HOURS];
    uint8_t hour_count;
    uint32_t generation;
} weather_snapshot_t;

typedef struct {
    const weather_icon_t *icon;
    uint16_t decoded[WEATHER_ICON_PIXELS];
} decoded_weather_icon_t;

typedef struct {
    const weather_icon_t *icon;
    uint16_t scaled[FORECAST_ICON_PIXELS];
} forecast_weather_icon_t;

static spi_device_handle_t display_spi;
static uint16_t *framebuffer;
static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;
static weather_snapshot_t current_weather;
static portMUX_TYPE weather_mux = portMUX_INITIALIZER_UNLOCKED;
static decoded_weather_icon_t current_icon_cache[CURRENT_ICON_CACHE_CAPACITY];
static size_t current_icon_cache_count;
static forecast_weather_icon_t forecast_icon_cache[WEATHER_FORECAST_HOURS];
static size_t forecast_icon_cache_count;
static uint16_t weather_icon_decode_scratch[WEATHER_ICON_PIXELS];
static uint32_t weather_icon_cache_generation = UINT32_MAX;

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
    ESP_LOGI(TAG, "Display init: SCL=%d SDA=%d DC=%d CS=%d RST=%d, %dx%d",
             PIN_SCL, PIN_SDA, PIN_DC, PIN_CS, PIN_RST,
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
        .mosi_io_num = PIN_SDA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_SPI_MAX_TRANSFER_BYTES,
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

static const char forecast_font4x7_characters[] = "0123456789:";
static const uint8_t forecast_font4x7[][7] = {
    {0x6, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6},
    {0x2, 0x6, 0x2, 0x2, 0x2, 0x2, 0x7},
    {0x6, 0x9, 0x1, 0x2, 0x4, 0x8, 0xF},
    {0xE, 0x1, 0x1, 0x6, 0x1, 0x1, 0xE},
    {0x9, 0x9, 0x9, 0xF, 0x1, 0x1, 0x1},
    {0xF, 0x8, 0x8, 0xE, 0x1, 0x9, 0x6},
    {0x7, 0x8, 0x8, 0xE, 0x9, 0x9, 0x6},
    {0xF, 0x1, 0x2, 0x2, 0x4, 0x4, 0x4},
    {0x6, 0x9, 0x9, 0x6, 0x9, 0x9, 0x6},
    {0x6, 0x9, 0x9, 0x7, 0x1, 0x1, 0xE},
    {0x0, 0x6, 0x6, 0x0, 0x6, 0x6, 0x0},
};

static int forecast_time_text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 5 - 1) * scale;
}

static void draw_forecast_time_text(const char *text, int x, int y, int scale,
                                    uint16_t color)
{
    for (; *text != '\0'; ++text, x += 5 * scale) {
        const char *match = strchr(forecast_font4x7_characters, *text);
        if (match == NULL) {
            continue;
        }
        const uint8_t *glyph = forecast_font4x7[match - forecast_font4x7_characters];
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (glyph[row] & (1U << (3 - column))) {
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

static const weather_icon_t *weather_icon_for_forecast(const forecast_hour_t *forecast)
{
    if (forecast->solar_event) {
        return forecast->sunrise_event ? &weather_icon_sunrise : &weather_icon_sunset;
    }
    return weather_icon_for_code(forecast->icon);
}

static void decode_weather_icon(const weather_icon_t *icon, uint16_t *pixels)
{
    if (icon == NULL) {
        return;
    }
    memset(pixels, 0, WEATHER_ICON_PIXELS * sizeof(*pixels));
    const uint8_t *encoded = icon->rle;
    for (int row = 0; row < WEATHER_ICON_SIZE; ++row) {
        const uint8_t span_count = *encoded++;
        for (uint8_t span = 0; span < span_count; ++span) {
            const int start = *encoded++;
            const uint8_t length = *encoded++;
            for (uint8_t offset = 0; offset < length; ++offset) {
                const uint8_t color_index = *encoded++;
                if (color_index != 0 && start + offset < WEATHER_ICON_SIZE) {
                    pixels[row * WEATHER_ICON_SIZE + start + offset] =
                        weather_icon_palette[color_index - 1];
                }
            }
        }
    }
}

static uint16_t interpolate_rgb565(uint16_t top_left, uint16_t top_right,
                                   uint16_t bottom_left, uint16_t bottom_right,
                                   float x_weight, float y_weight)
{
    const float top_left_weight = (1.0f - x_weight) * (1.0f - y_weight);
    const float top_right_weight = x_weight * (1.0f - y_weight);
    const float bottom_left_weight = (1.0f - x_weight) * y_weight;
    const float bottom_right_weight = x_weight * y_weight;
    const int red = (int)lroundf(((top_left >> 11) & 0x1F) * top_left_weight +
                                 ((top_right >> 11) & 0x1F) * top_right_weight +
                                 ((bottom_left >> 11) & 0x1F) * bottom_left_weight +
                                 ((bottom_right >> 11) & 0x1F) * bottom_right_weight);
    const int green = (int)lroundf(((top_left >> 5) & 0x3F) * top_left_weight +
                                   ((top_right >> 5) & 0x3F) * top_right_weight +
                                   ((bottom_left >> 5) & 0x3F) * bottom_left_weight +
                                   ((bottom_right >> 5) & 0x3F) * bottom_right_weight);
    const int blue = (int)lroundf((top_left & 0x1F) * top_left_weight +
                                  (top_right & 0x1F) * top_right_weight +
                                  (bottom_left & 0x1F) * bottom_left_weight +
                                  (bottom_right & 0x1F) * bottom_right_weight);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static void scale_weather_icon(const uint16_t *source, uint16_t *destination)
{
    for (int y = 0; y < FORECAST_ICON_SIZE; ++y) {
        const float source_y = (float)y * (WEATHER_ICON_SIZE - 1) /
                               (FORECAST_ICON_SIZE - 1);
        const int y0 = (int)source_y;
        const int y1 = y0 < WEATHER_ICON_SIZE - 1 ? y0 + 1 : y0;
        const float y_weight = source_y - y0;
        for (int x = 0; x < FORECAST_ICON_SIZE; ++x) {
            const float source_x = (float)x * (WEATHER_ICON_SIZE - 1) /
                                   (FORECAST_ICON_SIZE - 1);
            const int x0 = (int)source_x;
            const int x1 = x0 < WEATHER_ICON_SIZE - 1 ? x0 + 1 : x0;
            const float x_weight = source_x - x0;
            destination[y * FORECAST_ICON_SIZE + x] = interpolate_rgb565(
                source[y0 * WEATHER_ICON_SIZE + x0],
                source[y0 * WEATHER_ICON_SIZE + x1],
                source[y1 * WEATHER_ICON_SIZE + x0],
                source[y1 * WEATHER_ICON_SIZE + x1], x_weight, y_weight);
        }
    }
}

static decoded_weather_icon_t *find_cached_current_icon(const weather_icon_t *icon)
{
    if (icon == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < current_icon_cache_count; ++index) {
        if (current_icon_cache[index].icon == icon) {
            return &current_icon_cache[index];
        }
    }
    return NULL;
}

static forecast_weather_icon_t *find_cached_forecast_icon(const weather_icon_t *icon)
{
    if (icon == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < forecast_icon_cache_count; ++index) {
        if (forecast_icon_cache[index].icon == icon) {
            return &forecast_icon_cache[index];
        }
    }
    return NULL;
}

static void cache_current_icon(const weather_icon_t *icon)
{
    if (icon == NULL || find_cached_current_icon(icon) != NULL ||
        current_icon_cache_count >= CURRENT_ICON_CACHE_CAPACITY) {
        return;
    }
    decoded_weather_icon_t *bitmap = &current_icon_cache[current_icon_cache_count++];
    bitmap->icon = icon;
    decode_weather_icon(icon, bitmap->decoded);
}

static void cache_forecast_icon(const weather_icon_t *icon)
{
    if (icon == NULL || find_cached_forecast_icon(icon) != NULL ||
        forecast_icon_cache_count >= WEATHER_FORECAST_HOURS) {
        return;
    }
    forecast_weather_icon_t *bitmap = &forecast_icon_cache[forecast_icon_cache_count++];
    bitmap->icon = icon;
    decoded_weather_icon_t *current_bitmap = find_cached_current_icon(icon);
    const uint16_t *source = weather_icon_decode_scratch;
    if (current_bitmap != NULL) {
        source = current_bitmap->decoded;
    } else {
        decode_weather_icon(icon, weather_icon_decode_scratch);
    }
    scale_weather_icon(source, bitmap->scaled);
}

static void rebuild_weather_icon_cache(const weather_snapshot_t *weather)
{
    current_icon_cache_count = 0;
    forecast_icon_cache_count = 0;
    if (weather->current.valid) {
        cache_current_icon(weather_icon_for_code(weather->current.icon));
        const bool clear_or_few = strncmp(weather->current.icon, "01", 2) == 0 ||
                                  strncmp(weather->current.icon, "02", 2) == 0;
        if (clear_or_few && weather->current.sunrise > 0) {
            cache_current_icon(&weather_icon_sunrise);
        }
        if (clear_or_few && weather->current.sunset > 0) {
            cache_current_icon(&weather_icon_sunset);
        }
    }
    for (uint8_t index = 0; index < weather->hour_count; ++index) {
        cache_forecast_icon(weather_icon_for_forecast(&weather->hours[index]));
    }
    weather_icon_cache_generation = weather->generation;
}

static void draw_cached_weather_icon(const weather_icon_t *icon, int x, int y,
                                     bool forecast_size)
{
    const int size = forecast_size ? FORECAST_ICON_SIZE : WEATHER_ICON_SIZE;
    const uint16_t *pixels;
    if (forecast_size) {
        forecast_weather_icon_t *bitmap = find_cached_forecast_icon(icon);
        if (bitmap == NULL) {
            return;
        }
        pixels = bitmap->scaled;
    } else {
        decoded_weather_icon_t *bitmap = find_cached_current_icon(icon);
        if (bitmap == NULL) {
            return;
        }
        pixels = bitmap->decoded;
    }
    for (int row = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column) {
            const uint16_t color = pixels[row * size + column];
            if (color != 0) {
                set_pixel(x + column, y + row, color);
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

static void draw_forecast_tick(int index)
{
    draw_hour_marker(index * 5, FIVE_SECOND_COLOR);
}

static void draw_clock_face(const struct tm *time_info, const weather_snapshot_t *weather,
                            time_t now, bool colon_white, bool forecast_face)
{
    memset(framebuffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer));
    draw_circle(CENTER_X, CENTER_Y, 118, DIAL_COLOR);
    draw_circle(CENTER_X, CENTER_Y, 117, DIAL_COLOR);

    if (forecast_face) {
        if (weather != NULL) {
            for (uint8_t index = 0; index < weather->hour_count; ++index) {
                const float angle = (index * 30.0f - 90.0f) *
                                    3.14159265f / 180.0f;
                struct tm forecast_time;
                if (localtime_r(&weather->hours[index].timestamp, &forecast_time) != NULL) {
                    char hour_text[3];
                    snprintf(hour_text, sizeof(hour_text), "%02d", forecast_time.tm_hour);
                    const int label_center_x = CENTER_X +
                        (int)lroundf(cosf(angle) * 84);
                    const int label_center_y = CENTER_Y +
                        (int)lroundf(sinf(angle) * 84);
                    draw_text(hour_text,
                              label_center_x - text_width(hour_text, 1) / 2,
                              label_center_y - 3, 1, FORECAST_HOUR_COLOR);
                }
                const float icon_angle = (index * 30.0f + 15.0f - 90.0f) *
                                         3.14159265f / 180.0f;
                const int icon_center_x = CENTER_X + (int)lroundf(cosf(icon_angle) * 97);
                const int icon_center_y = CENTER_Y + (int)lroundf(sinf(icon_angle) * 97);
                draw_cached_weather_icon(
                    weather_icon_for_forecast(&weather->hours[index]),
                    icon_center_x - FORECAST_ICON_SIZE / 2,
                    icon_center_y - FORECAST_ICON_SIZE / 2, true);
                draw_forecast_tick(index);
            }
        }
    } else {
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
    }

    if (time_info != NULL) {
        char time_text[6];
        strftime(time_text, sizeof(time_text), "%H:%M", time_info);
        if (forecast_face) {
            const int scale = 6;
            const int time_x =
                (DISPLAY_WIDTH - forecast_time_text_width(time_text, scale)) / 2;
            const int time_y = (DISPLAY_HEIGHT - 7 * scale) / 2;
            const int date_scale = 3;
            const int date_gap = 12;
            char weekday_text[4];
            char date_text[8];
            strftime(weekday_text, sizeof(weekday_text), "%a", time_info);
            strftime(date_text, sizeof(date_text), "%d %b", time_info);
            for (char *character = weekday_text; *character != '\0'; ++character) {
                if (*character >= 'a' && *character <= 'z') {
                    *character = (char)(*character - 'a' + 'A');
                }
            }
            for (char *character = date_text; *character != '\0'; ++character) {
                if (*character >= 'a' && *character <= 'z') {
                    *character = (char)(*character - 'a' + 'A');
                }
            }
            draw_text(weekday_text,
                      (DISPLAY_WIDTH - text_width(weekday_text, date_scale)) / 2,
                      time_y - 7 * date_scale - date_gap, date_scale, MIN_COLOR);
            draw_forecast_time_text(time_text, time_x, time_y, scale,
                                    RGB565(255, 255, 255));
            if (!colon_white) {
                const int colon_x = time_x + 2 * 5 * scale;
                draw_forecast_time_text(":", colon_x, time_y, scale, DIAL_COLOR);
            }
            draw_text(date_text,
                      (DISPLAY_WIDTH - text_width(date_text, date_scale)) / 2,
                      time_y + 7 * scale + date_gap, date_scale, MIN_COLOR);
        } else {
            char date_text[16];
            strftime(date_text, sizeof(date_text), "%a %d", time_info);
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
                const int colon_x = (DISPLAY_WIDTH - text_width(time_text, 6)) / 2 +
                                    2 * 6 * 6;
                draw_text(":", colon_x, 78, 6, DIAL_COLOR);
            }
        }
    }

    if (weather != NULL && weather->current.valid && !forecast_face) {
        draw_cached_weather_icon(
            weather_icon_for_reading(&weather->current, now), 54, 128, false);
    }

    if (!forecast_face) {
        char temperature_text[8];
        char humidity_text[8];
        if (weather != NULL && weather->current.valid) {
            snprintf(temperature_text, sizeof(temperature_text), "%d",
                     weather->current.temperature);
            snprintf(humidity_text, sizeof(humidity_text), "%d%%",
                     weather->current.humidity);
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
}

static void flush_display(void)
{
    display_set_window(0, 0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1);
    const uint8_t memory_write = 0x2C;
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_CS, 0);
    esp_err_t result = spi_write_selected(false, &memory_write, sizeof(memory_write));
    const uint8_t *pixel_bytes = (const uint8_t *)framebuffer;
    const size_t frame_size = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(*framebuffer);
    for (size_t offset = 0; result == ESP_OK && offset < frame_size;) {
        size_t chunk_size = frame_size - offset;
        if (chunk_size > DISPLAY_SPI_MAX_TRANSFER_BYTES) {
            chunk_size = DISPLAY_SPI_MAX_TRANSFER_BYTES;
        }
        result = spi_write_selected(true, pixel_bytes + offset, chunk_size);
        offset += chunk_size;
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
    char *body;
    size_t length;
    size_t capacity;
} weather_http_response_t;

static esp_err_t weather_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    weather_http_response_t *response = event->user_data;
    const size_t data_length = (size_t)event->data_len;
    if (response->length >= response->capacity ||
        data_length > response->capacity - response->length - 1) {
        ESP_LOGE(TAG, "OpenWeatherMap response exceeds %u bytes",
                 (unsigned)response->capacity - 1);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response->body + response->length, event->data, data_length);
    response->length += data_length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t weather_http_get(const char *url, cJSON **root)
{
    *root = NULL;
    weather_http_response_t response = {
        .body = heap_caps_calloc(1, WEATHER_RESPONSE_MAX,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .capacity = WEATHER_RESPONSE_MAX,
    };
    if (response.body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t configuration = {
        .url = url,
        .event_handler = weather_http_event,
        .user_data = &response,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&configuration);
    if (client == NULL) {
        free(response.body);
        ESP_LOGE(TAG, "Could not initialize the OpenWeatherMap HTTP client");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        free(response.body);
        ESP_LOGE(TAG, "OpenWeatherMap request failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status_code < 200 || status_code >= 300) {
        free(response.body);
        ESP_LOGE(TAG, "OpenWeatherMap returned HTTP status %d", status_code);
        return ESP_FAIL;
    }

    *root = cJSON_Parse(response.body);
    free(response.body);
    if (*root == NULL) {
        ESP_LOGE(TAG, "OpenWeatherMap returned invalid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t make_weather_url(char *url, size_t url_size, const char *path,
                                  const char *extra_query)
{
    if (CONFIG_CLOCK_OPENWEATHERMAP_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "Set the OpenWeatherMap API key in menuconfig");
        return ESP_ERR_INVALID_ARG;
    }
    const int length = snprintf(
        url, url_size,
        "https://api.openweathermap.org%s?lat=%s&lon=%s&units=metric&%sappid=%s",
        path, CONFIG_CLOCK_LATITUDE, CONFIG_CLOCK_LONGITUDE, extra_query,
        CONFIG_CLOCK_OPENWEATHERMAP_API_KEY);
    if (length < 0 || (size_t)length >= url_size) {
        ESP_LOGE(TAG, "OpenWeatherMap request URL exceeds its configured buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool parse_weather_icon(cJSON *weather_array, char icon[4])
{
    if (!cJSON_IsArray(weather_array)) {
        return false;
    }
    cJSON *condition = cJSON_GetArrayItem(weather_array, 0);
    cJSON *icon_item = cJSON_GetObjectItemCaseSensitive(condition, "icon");
    if (!cJSON_IsString(icon_item) || icon_item->valuestring == NULL ||
        strlen(icon_item->valuestring) != 3) {
        return false;
    }
    strlcpy(icon, icon_item->valuestring, 4);
    return true;
}

static bool parse_current_weather(cJSON *current, weather_reading_t *reading)
{
    if (!cJSON_IsObject(current)) {
        return false;
    }
    cJSON *main_data = cJSON_GetObjectItemCaseSensitive(current, "main");
    if (main_data == NULL) {
        main_data = current;
    }
    cJSON *temperature = cJSON_GetObjectItemCaseSensitive(main_data, "temp");
    cJSON *humidity = cJSON_GetObjectItemCaseSensitive(main_data, "humidity");
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(humidity) ||
        !parse_weather_icon(cJSON_GetObjectItemCaseSensitive(current, "weather"),
                            reading->icon)) {
        return false;
    }

    reading->temperature = (int)lround(temperature->valuedouble);
    reading->humidity = humidity->valueint;
    cJSON *sys = cJSON_GetObjectItemCaseSensitive(current, "sys");
    if (sys == NULL) {
        sys = current;
    }
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
    return true;
}

static time_t next_local_hour(time_t now)
{
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) {
        return now + 3600;
    }
    local_time.tm_min = 0;
    local_time.tm_sec = 0;
    ++local_time.tm_hour;
    return mktime(&local_time);
}

static void add_current_forecast_hour(weather_snapshot_t *snapshot, time_t now)
{
    struct tm current_hour;
    snapshot->hours[0].timestamp = now;
    if (localtime_r(&now, &current_hour) != NULL) {
        current_hour.tm_min = 0;
        current_hour.tm_sec = 0;
        snapshot->hours[0].timestamp = mktime(&current_hour);
    }
    strlcpy(snapshot->hours[0].icon, snapshot->current.icon,
            sizeof(snapshot->hours[0].icon));
    snapshot->hour_count = 1;
}

static bool parse_forecast_hour(cJSON *item, forecast_hour_t *hour)
{
    if (!cJSON_IsObject(item)) {
        return false;
    }
    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(item, "dt");
    if (!cJSON_IsNumber(timestamp) || !isfinite(timestamp->valuedouble) ||
        timestamp->valuedouble <= 0 || timestamp->valuedouble >= 4102444800.0 ||
        !parse_weather_icon(cJSON_GetObjectItemCaseSensitive(item, "weather"),
                            hour->icon)) {
        return false;
    }
    hour->timestamp = (time_t)timestamp->valuedouble;
    return true;
}

static esp_err_t reserve_one_call_request(time_t now)
{
    struct tm local_time;
    if (localtime_r(&now, &local_time) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t local_day = (uint32_t)(local_time.tm_year + 1900) * 1000U +
                               (uint32_t)local_time.tm_yday;

    nvs_handle_t handle;
    esp_err_t result = nvs_open("weather_quota", NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    uint32_t stored_day = 0;
    uint16_t request_count = 0;
    const esp_err_t day_result = nvs_get_u32(handle, "onecall_day", &stored_day);
    if (day_result != ESP_OK && day_result != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return day_result;
    }
    if (day_result == ESP_OK && stored_day == local_day) {
        const esp_err_t count_result = nvs_get_u16(handle, "onecall_count", &request_count);
        if (count_result != ESP_OK && count_result != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return count_result;
        }
    }

    if (request_count >= ONE_CALL_DAILY_LIMIT) {
        ESP_LOGI(TAG, "One Call daily usage: %u/%u",
                 (unsigned)request_count, (unsigned)ONE_CALL_DAILY_LIMIT);
        nvs_close(handle);
        return ESP_ERR_INVALID_STATE;
    }

    result = nvs_set_u32(handle, "onecall_day", local_day);
    if (result == ESP_OK) {
        result = nvs_set_u16(handle, "onecall_count", request_count + 1);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "One Call daily usage: %u/%u",
                 (unsigned)(request_count + 1), (unsigned)ONE_CALL_DAILY_LIMIT);
    }
    return result;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const int adjusted_month = (int)month + (month > 2 ? -3 : 9);
    const unsigned day_of_year = (153 * (unsigned)adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                                year_of_era / 100 + day_of_year;
    return (int64_t)era * 146097 + day_of_era - 719468;
}

static double normalize_degrees(double degrees)
{
    degrees = fmod(degrees, 360.0);
    return degrees < 0 ? degrees + 360.0 : degrees;
}

static bool calculate_solar_event(const struct tm *local_date, double latitude,
                                  double longitude, bool sunrise, time_t *event_time)
{
    const double radians = 3.14159265358979323846 / 180.0;
    const double longitude_hour = longitude / 15.0;
    const double day_number = local_date->tm_yday + 1.0;
    const double approximate_time = day_number +
        ((sunrise ? 6.0 : 18.0) - longitude_hour) / 24.0;
    const double mean_anomaly = 0.9856 * approximate_time - 3.289;
    const double true_longitude = normalize_degrees(
        mean_anomaly + 1.916 * sin(mean_anomaly * radians) +
        0.020 * sin(2.0 * mean_anomaly * radians) + 282.634);
    double right_ascension = normalize_degrees(
        atan(0.91764 * tan(true_longitude * radians)) / radians);
    right_ascension += floor(true_longitude / 90.0) * 90.0 -
                       floor(right_ascension / 90.0) * 90.0;
    right_ascension /= 15.0;

    const double sin_declination = 0.39782 * sin(true_longitude * radians);
    const double cos_declination = cos(asin(sin_declination));
    const double latitude_radians = latitude * radians;
    const double denominator = cos_declination * cos(latitude_radians);
    if (fabs(denominator) < 1e-12) {
        return false;
    }
    const double cos_hour_angle =
        (cos(90.833 * radians) - sin_declination * sin(latitude_radians)) /
        denominator;
    if (cos_hour_angle < -1.0 || cos_hour_angle > 1.0) {
        return false;
    }

    const double hour_angle_degrees = acos(cos_hour_angle) / radians;
    const double hour_angle = (sunrise ? 360.0 - hour_angle_degrees
                                      : hour_angle_degrees) / 15.0;
    const double local_mean_time = hour_angle + right_ascension -
                                   0.06571 * approximate_time - 6.622;
    double utc_hour = local_mean_time - longitude_hour;
    int day_adjustment = 0;
    while (utc_hour < 0.0) {
        utc_hour += 24.0;
        --day_adjustment;
    }
    while (utc_hour >= 24.0) {
        utc_hour -= 24.0;
        ++day_adjustment;
    }

    const int64_t utc_day = days_from_civil(1900 + local_date->tm_year,
                                            (unsigned)local_date->tm_mon + 1,
                                            (unsigned)local_date->tm_mday) +
                            day_adjustment;
    *event_time = (time_t)(utc_day * 86400 + (int64_t)llround(utc_hour * 3600.0));
    return true;
}

static bool same_local_hour(time_t first, time_t second)
{
    struct tm first_local;
    struct tm second_local;
    return localtime_r(&first, &first_local) != NULL &&
           localtime_r(&second, &second_local) != NULL &&
           first_local.tm_year == second_local.tm_year &&
           first_local.tm_yday == second_local.tm_yday &&
           first_local.tm_hour == second_local.tm_hour;
}

static void insert_solar_event(weather_snapshot_t *snapshot, time_t event_time,
                               bool sunrise, bool allow_missing_hour)
{
    if (event_time < snapshot->hours[0].timestamp ||
        event_time > snapshot->hours[snapshot->hour_count - 1].timestamp) {
        return;
    }

    for (uint8_t index = 0; index < snapshot->hour_count; ++index) {
        if (same_local_hour(snapshot->hours[index].timestamp, event_time)) {
            snapshot->hours[index].solar_event = true;
            snapshot->hours[index].sunrise_event = sunrise;
            return;
        }
    }
    if (!allow_missing_hour) {
        return;
    }

    uint8_t insert_index = 0;
    while (insert_index < snapshot->hour_count &&
           snapshot->hours[insert_index].timestamp < event_time) {
        ++insert_index;
    }
    if (insert_index >= snapshot->hour_count) {
        return;
    }
    if (snapshot->hour_count < WEATHER_FORECAST_HOURS) {
        memmove(&snapshot->hours[insert_index + 1], &snapshot->hours[insert_index],
                (snapshot->hour_count - insert_index) * sizeof(snapshot->hours[0]));
        ++snapshot->hour_count;
    } else {
        memmove(&snapshot->hours[insert_index + 1], &snapshot->hours[insert_index],
                (snapshot->hour_count - insert_index - 1) * sizeof(snapshot->hours[0]));
    }
    snapshot->hours[insert_index] = (forecast_hour_t){
        .timestamp = event_time,
        .solar_event = true,
        .sunrise_event = sunrise,
    };
}

static void add_solar_forecast_events(weather_snapshot_t *snapshot, time_t now,
                                      bool allow_missing_hour)
{
    if (snapshot->hour_count == 0) {
        return;
    }
    char *latitude_end;
    char *longitude_end;
    const double latitude = strtod(CONFIG_CLOCK_LATITUDE, &latitude_end);
    const double longitude = strtod(CONFIG_CLOCK_LONGITUDE, &longitude_end);
    if (CONFIG_CLOCK_LATITUDE[0] == '\0' || CONFIG_CLOCK_LONGITUDE[0] == '\0' ||
        *latitude_end != '\0' || *longitude_end != '\0' ||
        !isfinite(latitude) || !isfinite(longitude) ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0) {
        return;
    }

    struct tm current_date;
    if (localtime_r(&now, &current_date) == NULL) {
        return;
    }
    for (int day_offset = 0; day_offset <= 2; ++day_offset) {
        struct tm local_date = current_date;
        local_date.tm_mday += day_offset;
        local_date.tm_hour = 12;
        local_date.tm_min = 0;
        local_date.tm_sec = 0;
        local_date.tm_isdst = -1;
        const time_t local_midday = mktime(&local_date);
        if (local_midday < 0 || localtime_r(&local_midday, &local_date) == NULL) {
            continue;
        }

        time_t sunrise = 0;
        time_t sunset = 0;
        const bool has_sunrise = day_offset == 0 && snapshot->current.sunrise > 0
            ? (sunrise = snapshot->current.sunrise, true)
            : calculate_solar_event(&local_date, latitude, longitude, true, &sunrise);
        const bool has_sunset = day_offset == 0 && snapshot->current.sunset > 0
            ? (sunset = snapshot->current.sunset, true)
            : calculate_solar_event(&local_date, latitude, longitude, false, &sunset);
        if (has_sunrise) {
            insert_solar_event(snapshot, sunrise, true, allow_missing_hour);
        }
        if (has_sunset) {
            insert_solar_event(snapshot, sunset, false, allow_missing_hour);
        }
    }
}

static esp_err_t fetch_one_call(weather_snapshot_t *snapshot, time_t now)
{
    char url[512];
    esp_err_t result = make_weather_url(url, sizeof(url), "/data/3.0/onecall",
                                        "exclude=minutely,daily,alerts&");
    if (result != ESP_OK) {
        return result;
    }
    result = reserve_one_call_request(now);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Skipping One Call request: %s", esp_err_to_name(result));
        return result;
    }
    cJSON *root = NULL;
    result = weather_http_get(url, &root);
    if (result != ESP_OK) {
        return result;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!parse_current_weather(current, &snapshot->current)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    add_current_forecast_hour(snapshot, now);

    const time_t first_forecast = next_local_hour(now);
    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const int count = cJSON_GetArraySize(hourly);
    for (int index = 0; index < count &&
         snapshot->hour_count < WEATHER_FORECAST_HOURS; ++index) {
        forecast_hour_t hour = {0};
        cJSON *item = cJSON_GetArrayItem(hourly, index);
        if (parse_forecast_hour(item, &hour) && hour.timestamp >= first_forecast) {
            snapshot->hours[snapshot->hour_count++] = hour;
        }
    }
    cJSON_Delete(root);
    if (snapshot->hour_count == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    add_solar_forecast_events(snapshot, now, false);
    return ESP_OK;
}

static esp_err_t fetch_openweather_25(weather_snapshot_t *snapshot, time_t now)
{
    char url[512];
    esp_err_t result = make_weather_url(url, sizeof(url), "/data/2.5/weather", "");
    if (result != ESP_OK) {
        return result;
    }
    cJSON *root = NULL;
    result = weather_http_get(url, &root);
    if (result != ESP_OK) {
        return result;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!parse_current_weather(root, &snapshot->current)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    add_current_forecast_hour(snapshot, now);
    cJSON_Delete(root);

    result = make_weather_url(url, sizeof(url), "/data/2.5/forecast", "");
    if (result != ESP_OK) {
        return result;
    }
    root = NULL;
    result = weather_http_get(url, &root);
    if (result != ESP_OK) {
        return result;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const time_t first_forecast = next_local_hour(now);
    cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    const int count = cJSON_GetArraySize(list);
    for (int index = 0; index < count &&
         snapshot->hour_count < WEATHER_FORECAST_HOURS; ++index) {
        forecast_hour_t hour = {0};
        cJSON *item = cJSON_GetArrayItem(list, index);
        if (parse_forecast_hour(item, &hour) && hour.timestamp >= first_forecast) {
            snapshot->hours[snapshot->hour_count++] = hour;
        }
    }
    cJSON_Delete(root);
    if (snapshot->hour_count == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    add_solar_forecast_events(snapshot, now, true);
    return ESP_OK;
}

static esp_err_t fetch_weather_snapshot(weather_snapshot_t *snapshot, time_t now)
{
    esp_err_t result = fetch_one_call(snapshot, now);
    if (result == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "One Call 3.0 unavailable (%s); trying 2.5 forecast",
             esp_err_to_name(result));
    memset(snapshot, 0, sizeof(*snapshot));
    result = fetch_openweather_25(snapshot, now);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Using OpenWeatherMap 2.5 current and 3-hour forecasts");
    }
    return result;
}

static bool weather_data_equal(const weather_snapshot_t *left,
                               const weather_snapshot_t *right)
{
    if (left->current.temperature != right->current.temperature ||
        left->current.humidity != right->current.humidity ||
        strcmp(left->current.icon, right->current.icon) != 0 ||
        left->current.sunrise != right->current.sunrise ||
        left->current.sunset != right->current.sunset ||
        left->current.valid != right->current.valid ||
        left->hour_count != right->hour_count) {
        return false;
    }
    for (uint8_t index = 0; index < left->hour_count; ++index) {
        if (left->hours[index].timestamp != right->hours[index].timestamp ||
            strcmp(left->hours[index].icon, right->hours[index].icon) != 0 ||
            left->hours[index].solar_event != right->hours[index].solar_event ||
            left->hours[index].sunrise_event != right->hours[index].sunrise_event) {
            return false;
        }
    }
    return true;
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

        weather_snapshot_t updated = {0};
        const esp_err_t result = fetch_weather_snapshot(&updated, now);
        if (result == ESP_OK) {
            portENTER_CRITICAL(&weather_mux);
            if (!weather_data_equal(&current_weather, &updated)) {
                updated.generation = current_weather.generation + 1;
                current_weather = updated;
            }
            portEXIT_CRITICAL(&weather_mux);
            ESP_LOGI(TAG, "Weather updated: %d C, humidity %d%%, %u forecast hours",
                     updated.current.temperature, updated.current.humidity,
                     (unsigned)updated.hour_count);
            vTaskDelay(pdMS_TO_TICKS(WEATHER_REFRESH_SECONDS * 1000));
        } else {
            ESP_LOGE(TAG, "Weather update failed: %s", esp_err_to_name(result));
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

typedef struct {
    bool stable_pressed;
    bool candidate_pressed;
    int64_t candidate_since_us;
} boot_button_state_t;

static void configure_boot_button(void)
{
    gpio_config_t input_pin = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
}

static bool update_boot_button(boot_button_state_t *state, bool *forecast_face)
{
    const bool pressed = gpio_get_level(PIN_BOOT) == 0;
    const int64_t now_us = esp_timer_get_time();
    if (pressed != state->candidate_pressed) {
        state->candidate_pressed = pressed;
        state->candidate_since_us = now_us;
    } else if (pressed != state->stable_pressed &&
               now_us - state->candidate_since_us >= 40000) {
        state->stable_pressed = pressed;
        if (pressed) {
            *forecast_face = !*forecast_face;
            return true;
        }
    }
    return false;
}

static bool load_forecast_face(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CLOCK_STATE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not open clockface state: %s", esp_err_to_name(result));
        return false;
    }

    uint8_t stored_face = 0;
    result = nvs_get_u8(handle, CLOCK_STATE_NVS_KEY, &stored_face);
    nvs_close(handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Could not read clockface state: %s", esp_err_to_name(result));
        return false;
    }
    return stored_face != 0;
}

static void save_forecast_face(bool forecast_face)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CLOCK_STATE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not open clockface state: %s", esp_err_to_name(result));
        return;
    }

    result = nvs_set_u8(handle, CLOCK_STATE_NVS_KEY, forecast_face ? 1 : 0);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not save clockface state: %s", esp_err_to_name(result));
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    bool forecast_face = load_forecast_face();
    ESP_LOGI(TAG, "Restored %s clockface",
             forecast_face ? "forecast" : "current");

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
    configure_boot_button();
    draw_clock_face(NULL, NULL, 0, false, forecast_face);
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

    const bool initially_pressed = gpio_get_level(PIN_BOOT) == 0;
    boot_button_state_t button = {
        .stable_pressed = initially_pressed,
        .candidate_pressed = initially_pressed,
        .candidate_since_us = esp_timer_get_time(),
    };
    int64_t last_render_us = 0;
    while (true) {
        const bool face_changed = update_boot_button(&button, &forecast_face);
        if (face_changed) {
            save_forecast_face(forecast_face);
        }
        struct tm time_info;
        const time_t now = time(NULL);
        if (now < 1577836800 || localtime_r(&now, &time_info) == NULL) {
            ESP_LOGW(TAG, "Waiting for NTP time synchronization");
            if (face_changed) {
                draw_clock_face(NULL, NULL, now, false, forecast_face);
                flush_display();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        weather_snapshot_t weather;
        portENTER_CRITICAL(&weather_mux);
        weather = current_weather;
        portEXIT_CRITICAL(&weather_mux);
        if (weather_icon_cache_generation != weather.generation) {
            rebuild_weather_icon_cache(&weather);
        }
        const bool colon_white = (esp_timer_get_time() / 500000) % 2 == 0;
        const int64_t render_time_us = esp_timer_get_time();
        if (face_changed || render_time_us - last_render_us >= 500000) {
            draw_clock_face(&time_info, &weather, now, colon_white, forecast_face);
            flush_display();
            last_render_us = render_time_us;
            ESP_LOGI(TAG, "Clock frame drawn for %02d:%02d:%02d (%s)",
                     time_info.tm_hour, time_info.tm_min, time_info.tm_sec,
                     forecast_face ? "forecast" : "current");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
