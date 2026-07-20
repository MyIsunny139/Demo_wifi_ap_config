#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include "esp_err.h"
#include "esp_wifi.h"
#include <stddef.h>

typedef enum
{
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
} WIFI_STATE;

typedef void (*p_wifi_scan_callback)(int numbers, wifi_ap_record_t *ap_records);
typedef void (*p_wifi_state_callback)(WIFI_STATE state);
typedef void (*p_wifi_connect_failed_callback)(void);

void wifi_manager_init(p_wifi_state_callback f);
void wifi_manager_set_connect_failed_callback(p_wifi_connect_failed_callback f);
esp_err_t wifi_manager_ap(void);
esp_err_t wifi_manager_scan(p_wifi_scan_callback f);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len);
bool wifi_manager_has_credentials(void);
bool wifi_manager_is_connect(void);

#endif
