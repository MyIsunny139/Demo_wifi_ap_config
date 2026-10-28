#include "oled_display.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_SDA_GPIO 1
#define OLED_SCL_GPIO 2
#define OLED_I2C_ADDRESS 0x3C
#define OLED_WIDTH 128
#define OLED_PAGE_COUNT 8

static const char *TAG = "oled_display";
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t device_handle;
static SemaphoreHandle_t display_mutex;
static bool initialized;
static char ip_line[24] = "WIFI DISCONNECTED";
static char temperature_line[24] = "DHT11 WAITING";
static char mq2_line[24] = "MQ2 WAITING";
static char risk_line[24] = "";

static const uint8_t font_digits[11][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31},
    {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e},
    {0x00, 0x60, 0x60, 0x00, 0x00},
};

static const uint8_t font_upper[26][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c},
    {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01},
    {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06},
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f},
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static esp_err_t send_command(uint8_t command)
{
    uint8_t data[2] = {0x00, command};
    return i2c_master_transmit(device_handle, data, sizeof(data), 100);
}

static esp_err_t send_page(const uint8_t *page)
{
    uint8_t data[OLED_WIDTH + 1];
    data[0] = 0x40;
    memcpy(&data[1], page, OLED_WIDTH);
    return i2c_master_transmit(device_handle, data, sizeof(data), 100);
}

static void draw_char(uint8_t *page, size_t *column, char character)
{
    const uint8_t *glyph = NULL;
    if (character >= '0' && character <= '9') glyph = font_digits[character - '0'];
    else if (character == '.') glyph = font_digits[10];
    else if (character >= 'A' && character <= 'Z') glyph = font_upper[character - 'A'];
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
    if (character == ':') glyph = colon;
    else if (character == '-') glyph = dash;
    else if (character == '%') glyph = percent;
    if (*column + 6 > OLED_WIDTH) return;
    if (glyph) memcpy(&page[*column], glyph, 5);
    else memset(&page[*column], 0, 5);
    page[*column + 5] = 0;
    *column += 6;
}

static esp_err_t show_text(const char *line0, const char *line1)
{
    if (!initialized || !line0 || !line1) return ESP_ERR_INVALID_STATE;
    uint8_t page[OLED_WIDTH];
    esp_err_t err = ESP_OK;
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    for (uint8_t page_index = 0; page_index < OLED_PAGE_COUNT && err == ESP_OK; ++page_index) {
        err = send_command((uint8_t)(0xB0 | page_index));
        if (err == ESP_OK) err = send_command(0x00);
        if (err == ESP_OK) err = send_command(0x10);
        memset(page, 0, sizeof(page));
        size_t column = 0;
        const char *text = page_index == 0 ? line0 :
                           (page_index == 2 ? temperature_line :
                            (page_index == 4 ? mq2_line :
                             (page_index == 6 ? line1 : "")));
        while (*text && column < OLED_WIDTH) draw_char(page, &column, *text++);
        if (err == ESP_OK) err = send_page(page);
    }
    xSemaphoreGive(display_mutex);
    return err;
}

static void render_current(void)
{
    show_text(ip_line, risk_line);
}

esp_err_t oled_display_init(void)
{
    if (initialized) return ESP_OK;
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = OLED_I2C_PORT,
        .scl_io_num = OLED_SCL_GPIO,
        .sda_io_num = OLED_SDA_GPIO,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) return err;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus_handle, &device_config, &device_handle);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return err;
    }
    display_mutex = xSemaphoreCreateMutex();
    if (!display_mutex) {
        i2c_master_bus_rm_device(device_handle);
        device_handle = NULL;
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
    };
    for (size_t i = 0; i < sizeof(init_commands); ++i) {
        err = send_command(init_commands[i]);
        if (err != ESP_OK) {
            vSemaphoreDelete(display_mutex);
            display_mutex = NULL;
            i2c_master_bus_rm_device(device_handle);
            device_handle = NULL;
            i2c_del_master_bus(bus_handle);
            bus_handle = NULL;
            return err;
        }
    }
    initialized = true;
    render_current();
    ESP_LOGI(TAG, "SSD1306 OLED ready: SDA=%d SCL=%d address=0x%02X",
             OLED_SDA_GPIO, OLED_SCL_GPIO, OLED_I2C_ADDRESS);
    return ESP_OK;
}

void oled_display_show_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        oled_display_show_status("WIFI DISCONNECTED");
        return;
    }
    snprintf(ip_line, sizeof(ip_line), "IP " IPSTR, IP2STR(&info.ip));
    snprintf(risk_line, sizeof(risk_line), "  CONNECTED");
    render_current();
}

void oled_display_show_status(const char *status)
{
    if (status) {
        snprintf(ip_line, sizeof(ip_line), "%s", status);
        risk_line[0] = '\0';
        render_current();
    }
}

void oled_display_show_environment(bool dht_valid, float temperature_c,
                                   float humidity_pct, int mq2_adc)
{
    if (dht_valid) {
        const int temperature_tenths = (int)(temperature_c * 10.0f);
        snprintf(temperature_line, sizeof(temperature_line),
                 "T:%d.%dC H:%d%%", temperature_tenths / 10,
                 abs(temperature_tenths % 10), (int)humidity_pct);
    } else {
        snprintf(temperature_line, sizeof(temperature_line), "DHT11 ERROR");
    }
    snprintf(mq2_line, sizeof(mq2_line), "MQ2 ADC:%d", mq2_adc);
    render_current();
}

void oled_display_show_risk(const char *level, int people)
{
    if (!level || people < 0) {
        return;
    }
    if (strcmp(level, "low") == 0) {
        level = "LOW";
    } else if (strcmp(level, "medium") == 0) {
        level = "MEDIUM";
    } else if (strcmp(level, "high") == 0) {
        level = "HIGH";
    }
    snprintf(risk_line, sizeof(risk_line), "RISK %s P%d", level, people);
    render_current();
}
