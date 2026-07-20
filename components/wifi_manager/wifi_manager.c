#include "wifi_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wifi_manager"
#define MAX_CONNECT_RETRY 3
#define WIFI_SCAN_MAX_AP 20
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"

static int sta_connect_count = 0;
static esp_netif_t *esp_netif_sta = NULL;
static esp_netif_t *esp_netif_ap = NULL;
static const char *ap_ssid_name = "ESP32-xiaoyun";
static const char *ap_password = "12345678";
static p_wifi_state_callback wifi_state_cb = NULL;
static p_wifi_connect_failed_callback connect_failed_cb = NULL;
static bool is_sta_connected = false;
static bool connect_requested = false;
static SemaphoreHandle_t scan_sem = NULL;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            if (mode == WIFI_MODE_STA && connect_requested) {
                esp_wifi_connect();
            }
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Connected to AP");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (is_sta_connected) {
                is_sta_connected = false;
                if (wifi_state_cb) {
                    wifi_state_cb(WIFI_STATE_DISCONNECTED);
                }
            }

            if (connect_requested && sta_connect_count < MAX_CONNECT_RETRY) {
                wifi_mode_t mode;
                esp_wifi_get_mode(&mode);
                if (mode == WIFI_MODE_STA) {
                    esp_wifi_connect();
                }
                sta_connect_count++;
                ESP_LOGI(TAG, "connect to the AP fail, retry now");
            } else if (connect_requested) {
                connect_requested = false;
                ESP_LOGW(TAG, "connect to the AP failed after %d retries", sta_connect_count);
                if (connect_failed_cb) {
                    connect_failed_cb();
                }
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Get ip address");
        is_sta_connected = true;
        sta_connect_count = 0;
        if (wifi_state_cb) {
            wifi_state_cb(WIFI_STATE_CONNECTED);
        }
    }
}

void wifi_manager_init(p_wifi_state_callback f)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_sta = esp_netif_create_default_wifi_sta();
    esp_netif_ap = esp_netif_create_default_wifi_ap();
    assert(esp_netif_sta);
    assert(esp_netif_ap);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_state_cb = f;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "wifi_init finished.");
}

void wifi_manager_set_connect_failed_callback(p_wifi_connect_failed_callback f)
{
    connect_failed_cb = f;
}

esp_err_t wifi_manager_ap(void)
{
    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    if (mode == WIFI_MODE_APSTA) {
        return ESP_OK;
    }

    connect_requested = false;
    is_sta_connected = false;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 5,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", ap_ssid_name);
    wifi_config.ap.ssid_len = strlen(ap_ssid_name);
    snprintf((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), "%s", ap_password);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(esp_netif_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(esp_netif_ap, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(esp_netif_ap));

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static void scan_task(void *param)
{
    p_wifi_scan_callback callback = (p_wifi_scan_callback)param;
    uint16_t number = WIFI_SCAN_MAX_AP;
    wifi_ap_record_t *ap_info = calloc(number, sizeof(wifi_ap_record_t));
    if (!ap_info) {
        xSemaphoreGive(scan_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Start wifi scan");
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
        ESP_LOGI(TAG, "Total APs returned = %u", number);
        if (callback) {
            callback(number, ap_info);
            ap_info = NULL;
        }
    } else {
        ESP_LOGE(TAG, "wifi scan failed: %s", esp_err_to_name(ret));
    }

    free(ap_info);
    xSemaphoreGive(scan_sem);
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_scan(p_wifi_scan_callback f)
{
    if (!scan_sem) {
        scan_sem = xSemaphoreCreateBinary();
        if (!scan_sem) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(scan_sem);
    }

    if (xSemaphoreTake(scan_sem, 0) == pdTRUE) {
        esp_wifi_clear_ap_list();
        if (xTaskCreatePinnedToCore(scan_task, "scan", 8192, f, 3, NULL, 0) == pdTRUE) {
            return ESP_OK;
        }
        xSemaphoreGive(scan_sem);
    }

    return ESP_FAIL;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "ssid is empty");
        return ESP_ERR_INVALID_ARG;
    }

    sta_connect_count = 0;
    connect_requested = true;

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", password ? password : "");
    if (password && password[0] != '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_disconnect();

    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));
    if (mode != WIFI_MODE_STA) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    return ESP_OK;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_NVS_PASSWORD_KEY, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi credentials failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "wifi credentials saved");
    }
    return err;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (!ssid || ssid_len == 0 || !password || password_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = ssid_len;
    err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, ssid, &required);
    if (err == ESP_OK) {
        required = password_len;
        err = nvs_get_str(nvs, WIFI_NVS_PASSWORD_KEY, password, &required);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(nvs);

    if (err == ESP_OK && ssid[0] == '\0') {
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    return err;
}

bool wifi_manager_has_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    return wifi_manager_load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK;
}

bool wifi_manager_is_connect(void)
{
    return is_sta_connected;
}
