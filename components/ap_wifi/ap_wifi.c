#include "ap_wifi.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ws_server.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "apcfg"
#define INDEX_HTML_PATH "/spiffs/apcfg.html"
#define APCFG_BIT (BIT0)
#define ENTER_APCFG_BIT (BIT1)
#define TRY_NEXT_CREDENTIAL_BIT (BIT2)

static char *index_html = NULL;
static EventGroupHandle_t apcfg_event = NULL;
static wifi_credentials_t saved_credentials[WIFI_CREDENTIAL_CAPACITY];
static size_t saved_credential_count;
static size_t active_credential_index = WIFI_CREDENTIAL_CAPACITY;
static size_t next_credential_index;
static size_t pending_credential_index = WIFI_CREDENTIAL_CAPACITY;
static p_wifi_state_callback app_wifi_state_cb;

static void ws_receive_handle(uint8_t *payload, int len);
static void wifi_connect_failed_handle(void);

static esp_err_t load_saved_credentials(void)
{
    active_credential_index = WIFI_CREDENTIAL_CAPACITY;
    next_credential_index = 0;
    pending_credential_index = WIFI_CREDENTIAL_CAPACITY;
    return wifi_manager_load_credentials_list(
        saved_credentials, WIFI_CREDENTIAL_CAPACITY, &saved_credential_count);
}

static void connect_saved_credential(size_t index)
{
    if (index >= saved_credential_count) {
        xEventGroupSetBits(apcfg_event, ENTER_APCFG_BIT);
        return;
    }

    active_credential_index = index;
    ESP_LOGI(TAG, "Trying saved WiFi %u/%u: %s", (unsigned)(index + 1),
             (unsigned)saved_credential_count, saved_credentials[index].ssid);
    esp_err_t err = wifi_manager_connect(saved_credentials[index].ssid,
                                         saved_credentials[index].password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start WiFi connection: %s", esp_err_to_name(err));
        wifi_connect_failed_handle();
    }
}

static void start_saved_connection_cycle(void)
{
    esp_err_t err = load_saved_credentials();
    if (err != ESP_OK || saved_credential_count == 0) {
        ESP_LOGW(TAG, "No saved WiFi credentials, enter AP config mode");
        xEventGroupSetBits(apcfg_event, ENTER_APCFG_BIT);
        return;
    }

    active_credential_index = 0;
    next_credential_index = 1;
    connect_saved_credential(0);
}

static void wifi_state_handle(WIFI_STATE state)
{
    if (state == WIFI_STATE_CONNECTED) {
        next_credential_index = 0;
    }
    if (app_wifi_state_cb) {
        app_wifi_state_cb(state);
    }
}

static void wifi_connect_failed_handle(void)
{
    while (next_credential_index < saved_credential_count &&
           next_credential_index == active_credential_index) {
        next_credential_index++;
    }

    if (next_credential_index < saved_credential_count) {
        pending_credential_index = next_credential_index++;
        ESP_LOGW(TAG, "Saved WiFi failed; scheduling credential %u/%u",
                 (unsigned)(pending_credential_index + 1),
                 (unsigned)saved_credential_count);
        if (apcfg_event) {
            xEventGroupSetBits(apcfg_event, TRY_NEXT_CREDENTIAL_BIT);
        }
        return;
    }

    ESP_LOGW(TAG, "All saved WiFi credentials failed, enter AP config mode");
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
        esp_err_t err = wifi_manager_save_credentials(ssid_js->valuestring,
                                                      password_js->valuestring);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Saved ssid:%s, now stop http server", ssid_js->valuestring);
            xEventGroupSetBits(apcfg_event, APCFG_BIT);
        } else {
            ESP_LOGE(TAG, "Could not save WiFi credentials: %s", esp_err_to_name(err));
        }
    }

    cJSON_Delete(root);
}

static void ap_wifi_task(void *param)
{
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(
            apcfg_event,
            APCFG_BIT | ENTER_APCFG_BIT | TRY_NEXT_CREDENTIAL_BIT,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if (ev & APCFG_BIT) {
            web_ws_stop();
            start_saved_connection_cycle();
        } else if (ev & TRY_NEXT_CREDENTIAL_BIT) {
            connect_saved_credential(pending_credential_index);
        } else if (ev & ENTER_APCFG_BIT) {
            start_ap_config_portal();
        }
    }
}

void ap_wifi_init(p_wifi_state_callback f)
{
    index_html = init_web_page_buffer();
    app_wifi_state_cb = f;
    wifi_manager_init(wifi_state_handle);
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
    esp_err_t err = wifi_manager_save_credentials(ssid, password);
    if (err == ESP_OK) {
        start_saved_connection_cycle();
    } else {
        ESP_LOGE(TAG, "Could not save WiFi credentials: %s", esp_err_to_name(err));
    }
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
    start_saved_connection_cycle();
}
