#include "Init.h"

#include "ap_wifi.h"
#include "camera_service.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "video_stream.h"
#include <inttypes.h>

#define EXPECTED_CPU_HZ (240 * 1000 * 1000)
#define EXPECTED_FLASH_SIZE (16 * 1024 * 1024)
#define EXPECTED_PSRAM_SIZE (8 * 1024 * 1024)

static const char *TAG = "app_init";

static esp_err_t validate_hardware_profile(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash size: %s", esp_err_to_name(err));
        return err;
    }

    const int cpu_hz = esp_clk_cpu_freq();
    const bool psram_ready = esp_psram_is_initialized();
    const size_t psram_size = psram_ready ? esp_psram_get_size() : 0;
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "Chip: ESP32-S3 rev%d, %d cores", chip_info.revision, chip_info.cores);
    ESP_LOGI(TAG, "CPU: %d MHz", cpu_hz / 1000000);
    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "PSRAM: %u MB, free: %u KB", (unsigned)(psram_size / (1024 * 1024)),
             (unsigned)(psram_free / 1024));
    ESP_LOGI(TAG, "Internal RAM free: %u KB", (unsigned)(internal_free / 1024));

    if (chip_info.model != CHIP_ESP32S3 || chip_info.cores != 2 || cpu_hz != EXPECTED_CPU_HZ) {
        ESP_LOGE(TAG, "Unexpected CPU profile");
        return ESP_ERR_INVALID_STATE;
    }
    if (flash_size != EXPECTED_FLASH_SIZE) {
        ESP_LOGE(TAG, "Expected 16 MB flash, detected %" PRIu32 " bytes", flash_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!psram_ready || psram_size != EXPECTED_PSRAM_SIZE) {
        ESP_LOGE(TAG, "Expected initialized 8 MB PSRAM, detected %u bytes", (unsigned)psram_size);
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase before initialization");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static void wifi_state_handle(WIFI_STATE state)
{
    if (state == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        esp_err_t err = video_stream_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Video service start failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi disconnected");
        video_stream_stop();
        status_led_clear();
    }
}

esp_err_t app_system_init(void)
{
    esp_err_t err = validate_hardware_profile();
    if (err != ESP_OK) {
        return err;
    }

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    err = status_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Status LED unavailable: %s", esp_err_to_name(err));
    }

    err = camera_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera unavailable; Wi-Fi provisioning will remain active");
    } else {
        err = video_stream_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Video service initialization failed: %s", esp_err_to_name(err));
        }
    }

    ap_wifi_init(wifi_state_handle);
    ap_wifi_start();
    return ESP_OK;
}
