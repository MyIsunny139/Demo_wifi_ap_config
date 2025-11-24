#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "string.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"


#define WIFI_SSID      "MY_AP"
#define WIFI_PASSWORD  "my666666"

#define WIFI_AP_SSID      "ESP32_AP"
#define WIFI_AP_PASSWORD  "12345678"

static SemaphoreHandle_t scan_semaphore = NULL;

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_CONNECTION_FAILED
} wifi_state_t;

typedef void(*p_wifi_state_cb)(wifi_state_t state);
typedef void(*p_wifi_scan_cb)(uint16_t ap_num, wifi_ap_record_t* ap_records);

void wifi_manager_init(p_wifi_state_cb f);

void wifi_manager_connect(const char* ssid , const char* password);

esp_err_t wifi_manager_ap(void);

esp_err_t wifi_manager_scan(p_wifi_scan_cb f);

#endif