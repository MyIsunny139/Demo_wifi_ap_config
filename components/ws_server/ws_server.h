#ifndef WS_SERVER_H
#define WS_SERVER_H

#include "main.h"

typedef void (*ws_receive_cb)(uint8_t *data, size_t len);

typedef struct {
    const char* html_code;
    ws_receive_cb receive_fn;
} ws_server_config_t;

esp_err_t web_ws_server_start(ws_server_config_t* config);
esp_err_t web_ws_server_stop(void);
esp_err_t web_ws_server_send_text(uint8_t* data, int len);

#endif
