#include "ap_wifi.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ws_server.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "apcfg"
#define INDEX_HTML_PATH "/spiffs/apcfg.html"
#define APCFG_BIT (BIT0)
#define ENTER_APCFG_BIT (BIT1)

static char *index_html = NULL;
static EventGroupHandle_t apcfg_event = NULL;
static char current_ssid[33];
static char current_password[64];

static void ws_receive_handle(uint8_t *payload, int len);

static void wifi_connect_failed_handle(void)
{
    ESP_LOGW(TAG, "WiFi connect failed, enter AP config mode");
    if (apcfg_event) {
        xEventGroupSetBits(apcfg_event, ENTER_APCFG_BIT);
    }
}

static void start_ap_config_portal(void)
{
    wifi_manager_ap();
    ws_cfg_t ws = {
        .html_code = index_html,
        .receive_fn = ws_receive_handle,
    };
    web_ws_start(&ws);
}

static char *init_web_page_buffer(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "html",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return NULL;
    }

    struct stat st;
    if (stat(INDEX_HTML_PATH, &st) != 0) {
        ESP_LOGE(TAG, "apcfg.html not found");
        return NULL;
    }

    char *page = calloc(1, st.st_size + 1);
    if (!page) {
        ESP_LOGE(TAG, "No memory for html page");
        return NULL;
    }

    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (!fp) {
        free(page);
        ESP_LOGE(TAG, "Failed to open apcfg.html");
        return NULL;
    }

    size_t read_len = fread(page, 1, st.st_size, fp);
    fclose(fp);
    if (read_len != st.st_size) {
        free(page);
        ESP_LOGE(TAG, "Failed to read apcfg.html");
        return NULL;
    }

    return page;
}

static void wifi_scan_finish_handle(int numbers, wifi_ap_record_t *ap_records)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifilist_js = cJSON_AddArrayToObject(root, "wifi_list");
    if (!root || !wifilist_js) {
        cJSON_Delete(root);
        free(ap_records);
        return;
    }

    for (int i = 0; i < numbers; i++) {
        cJSON *wifi_js = cJSON_CreateObject();
        if (!wifi_js) {
            continue;
        }
        cJSON_AddStringToObject(wifi_js, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(wifi_js, "rssi", ap_records[i].rssi);
        cJSON_AddBoolToObject(wifi_js, "encrypted", ap_records[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(wifilist_js, wifi_js);
    }

    char *data = cJSON_PrintUnformatted(root);
    if (data) {
        ESP_LOGI(TAG, "WS send:%s", data);
        web_ws_send((uint8_t *)data, strlen(data));
        cJSON_free(data);
    }
    cJSON_Delete(root);
    free(ap_records);
}

static void ws_receive_handle(uint8_t *payload, int len)
{
    char *json = calloc(1, len + 1);
    if (!json) {
        ESP_LOGE(TAG, "No memory for websocket payload");
        return;
    }
    memcpy(json, payload, len);

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGE(TAG, "Receive invalid json");
        return;
    }

    cJSON *scan_js = cJSON_GetObjectItem(root, "scan");
    cJSON *ssid_js = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_js = cJSON_GetObjectItem(root, "password");

    if (cJSON_IsString(scan_js) && strcmp(scan_js->valuestring, "start") == 0) {
        wifi_manager_scan(wifi_scan_finish_handle);
    }

    if (cJSON_IsString(ssid_js) && cJSON_IsString(password_js)) {
        snprintf(current_ssid, sizeof(current_ssid), "%s", ssid_js->valuestring);
        snprintf(current_password, sizeof(current_password), "%s", password_js->valuestring);
        wifi_manager_save_credentials(current_ssid, current_password);
        ESP_LOGI(TAG, "Receive ssid:%s, now stop http server", current_ssid);
        xEventGroupSetBits(apcfg_event, APCFG_BIT);
    }

    cJSON_Delete(root);
}

static void ap_wifi_task(void *param)
{
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(apcfg_event, APCFG_BIT | ENTER_APCFG_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (ev & APCFG_BIT) {
            web_ws_stop();
            wifi_manager_connect(current_ssid, current_password);
        }
        if (ev & ENTER_APCFG_BIT) {
            start_ap_config_portal();
        }
    }
}

void ap_wifi_init(p_wifi_state_callback f)
{
    index_html = init_web_page_buffer();
    wifi_manager_init(f);
    wifi_manager_set_connect_failed_callback(wifi_connect_failed_handle);
    apcfg_event = xEventGroupCreate();
    if (!apcfg_event) {
        ESP_LOGE(TAG, "Failed to create apcfg event group");
        return;
    }
    xTaskCreatePinnedToCore(ap_wifi_task, "apcfg", 4096, NULL, 2, NULL, 1);
}

void ap_wifi_set(const char *ssid, const char *password)
{
    wifi_manager_connect(ssid, password);
}

void ap_wifi_apcfg(bool enable)
{
    if (!enable) {
        return;
    }

    start_ap_config_portal();
}

void ap_wifi_start(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    if (wifi_manager_load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        ESP_LOGI(TAG, "Found saved WiFi credentials, connect to ssid:%s", ssid);
        wifi_manager_connect(ssid, password);
        return;
    }

    ESP_LOGW(TAG, "No saved WiFi credentials, enter AP config mode");
    ap_wifi_apcfg(true);
}
