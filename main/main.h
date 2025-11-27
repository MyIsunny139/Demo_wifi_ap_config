#ifndef _MAIN_H_
#define _MAIN_H_

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "string.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "lwip/ip4_addr.h"
#include "string.h"
#include "esp_spiffs.h"
#include <sys/stat.h>
#include "nvs_flash.h"
#include "cJSON.h"


#include "ap_wifi.h"
#include "wifi_manager.h"
#include "ws_server.h"

#endif // _MAIN_H_