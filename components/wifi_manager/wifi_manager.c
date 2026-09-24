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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "wifi_manager"
#define MAX_CONNECT_RETRY 3
#define WIFI_SCAN_MAX_AP 20
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"
#define WIFI_NVS_COUNT_KEY "count"
#define WIFI_NVS_SSID_KEY_FORMAT "ssid%u"
#define WIFI_NVS_PASSWORD_KEY_FORMAT "pass%u"
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

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "AUTHMODE_BELOW_THRESHOLD";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "AP_FULL";
    default:
        return "OTHER";
    }
}

static esp_err_t wifi_manager_enable_dhcp(void)
{
    esp_netif_dhcp_status_t dhcp_status;
    esp_err_t err = esp_netif_dhcpc_get_status(esp_netif_sta, &dhcp_status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get STA DHCP status failed: %s", esp_err_to_name(err));
        return err;
    }

    if (dhcp_status == ESP_NETIF_DHCP_STOPPED) {
        err = esp_netif_dhcpc_start(esp_netif_sta);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGE(TAG, "start STA DHCP client failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "STA DHCP client enabled; waiting for an address from the AP");
    return ESP_OK;
}

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

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event =
                (const wifi_event_sta_disconnected_t *)event_data;
            if (event) {
                ESP_LOGW(TAG,
                         "STA disconnected: reason=%u (%s), rssi=%d, retry=%d/%d",
                         (unsigned)event->reason,
                         wifi_disconnect_reason_name(event->reason),
                         (int)event->rssi, sta_connect_count, MAX_CONNECT_RETRY);
            }
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
        }

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
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event) {
            ESP_LOGI(TAG, "STA got IP: " IPSTR ", netmask: " IPSTR ", gateway: " IPSTR,
                     IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                     IP2STR(&event->ip_info.gw));
        } else {
            ESP_LOGW(TAG, "STA got IP event without address data");
        }
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
    if (strlen(ssid) > WIFI_CREDENTIAL_SSID_MAX_LEN ||
        (password && strlen(password) > WIFI_CREDENTIAL_PASSWORD_MAX_LEN)) {
        ESP_LOGE(TAG, "WiFi credentials are too long");
        return ESP_ERR_INVALID_SIZE;
    }

    sta_connect_count = 0;
    connect_requested = false;

    esp_err_t err = wifi_manager_enable_dhcp();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
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
        connect_requested = true;
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        connect_requested = true;
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    return ESP_OK;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > WIFI_CREDENTIAL_SSID_MAX_LEN ||
        (password && strlen(password) > WIFI_CREDENTIAL_PASSWORD_MAX_LEN)) {
        return ESP_ERR_INVALID_SIZE;
    }

    wifi_credentials_t existing[WIFI_CREDENTIAL_CAPACITY] = {0};
    size_t existing_count = 0;
    esp_err_t load_err = wifi_manager_load_credentials_list(
        existing, WIFI_CREDENTIAL_CAPACITY, &existing_count);
    if (load_err != ESP_OK && load_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Could not read existing WiFi credentials: %s",
                 esp_err_to_name(load_err));
    }

    wifi_credentials_t updated[WIFI_CREDENTIAL_CAPACITY] = {0};
    snprintf(updated[0].ssid, sizeof(updated[0].ssid), "%s", ssid);
    snprintf(updated[0].password, sizeof(updated[0].password), "%s",
             password ? password : "");

    size_t updated_count = 1;
    for (size_t i = 0;
         i < existing_count && updated_count < WIFI_CREDENTIAL_CAPACITY; ++i) {
        if (strcmp(existing[i].ssid, ssid) == 0) {
            continue;
        }
        updated[updated_count++] = existing[i];
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < WIFI_CREDENTIAL_CAPACITY && err == ESP_OK; ++i) {
        char ssid_key[8];
        char password_key[8];
        snprintf(ssid_key, sizeof(ssid_key), WIFI_NVS_SSID_KEY_FORMAT, (unsigned)i);
        snprintf(password_key, sizeof(password_key), WIFI_NVS_PASSWORD_KEY_FORMAT,
                 (unsigned)i);

        if (i < updated_count) {
            err = nvs_set_str(nvs, ssid_key, updated[i].ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(nvs, password_key, updated[i].password);
            }
        } else {
            esp_err_t erase_err = nvs_erase_key(nvs, ssid_key);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
            erase_err = nvs_erase_key(nvs, password_key);
            if (err == ESP_OK && erase_err != ESP_OK &&
                erase_err != ESP_ERR_NVS_NOT_FOUND) {
                err = erase_err;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, WIFI_NVS_COUNT_KEY, (uint8_t)updated_count);
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(nvs, WIFI_NVS_SSID_KEY);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(nvs, WIFI_NVS_PASSWORD_KEY);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi credentials failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved %u WiFi credential set(s); newest SSID: %s",
                 (unsigned)updated_count, ssid);
    }
    return err;
}

esp_err_t wifi_manager_load_credentials_list(wifi_credentials_t *credentials,
                                             size_t capacity, size_t *count)
{
    if (!credentials || capacity == 0 || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    memset(credentials, 0, capacity * sizeof(*credentials));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t stored_count = 0;
    err = nvs_get_u8(nvs, WIFI_NVS_COUNT_KEY, &stored_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t ssid_required = sizeof(credentials[0].ssid);
        err = nvs_get_str(nvs, WIFI_NVS_SSID_KEY, credentials[0].ssid,
                          &ssid_required);
        if (err == ESP_OK && credentials[0].ssid[0] != '\0') {
            size_t password_required = sizeof(credentials[0].password);
            esp_err_t password_err = nvs_get_str(
                nvs, WIFI_NVS_PASSWORD_KEY, credentials[0].password,
                &password_required);
            if (password_err == ESP_ERR_NVS_NOT_FOUND) {
                credentials[0].password[0] = '\0';
            } else if (password_err != ESP_OK) {
                err = password_err;
            }
            if (err == ESP_OK) {
                *count = 1;
            }
        }
        nvs_close(nvs);
        return *count > 0 ? ESP_OK : err;
    }
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    size_t limit = stored_count;
    if (limit > WIFI_CREDENTIAL_CAPACITY) {
        limit = WIFI_CREDENTIAL_CAPACITY;
    }
    if (limit > capacity) {
        limit = capacity;
    }

    for (size_t i = 0; i < limit; ++i) {
        char ssid_key[8];
        char password_key[8];
        snprintf(ssid_key, sizeof(ssid_key), WIFI_NVS_SSID_KEY_FORMAT, (unsigned)i);
        snprintf(password_key, sizeof(password_key), WIFI_NVS_PASSWORD_KEY_FORMAT,
                 (unsigned)i);

        wifi_credentials_t entry = {0};
        size_t ssid_required = sizeof(entry.ssid);
        esp_err_t entry_err = nvs_get_str(nvs, ssid_key, entry.ssid, &ssid_required);
        if (entry_err != ESP_OK || entry.ssid[0] == '\0') {
            ESP_LOGW(TAG, "Ignoring invalid saved WiFi credential slot %u",
                     (unsigned)i);
            continue;
        }

        size_t password_required = sizeof(entry.password);
        entry_err = nvs_get_str(nvs, password_key, entry.password, &password_required);
        if (entry_err == ESP_ERR_NVS_NOT_FOUND) {
            entry.password[0] = '\0';
        } else if (entry_err != ESP_OK) {
            ESP_LOGW(TAG, "Ignoring invalid password in WiFi credential slot %u",
                     (unsigned)i);
            continue;
        }
        credentials[(*count)++] = entry;
    }

    nvs_close(nvs);
    return *count > 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    if (!ssid || ssid_len == 0 || !password || password_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_credentials_t credentials[1] = {0};
    size_t count = 0;
    esp_err_t err = wifi_manager_load_credentials_list(credentials, 1, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (strlen(credentials[0].ssid) >= ssid_len ||
        strlen(credentials[0].password) >= password_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(ssid, ssid_len, "%s", credentials[0].ssid);
    snprintf(password, password_len, "%s", credentials[0].password);
    return ESP_OK;
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
