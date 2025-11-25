#include <stdio.h>
#include "ws_server.h"

static const char* TAG = "ws_server";

static const char* http_html = NULL;

static ws_receive_cb ws_receive_callback = NULL;

static httpd_handle_t ws_httpd_server_handle = NULL;

static int client_fds =  -1;
esp_err_t ws_get_http_req(httpd_req_t *r)
{
    return httpd_resp_send(r, http_html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_ws_request(httpd_req_t *r)
{

    if(r->method == HTTP_GET) 
    {
        client_fds = httpd_req_to_sockfd(r); //完成握手
        ESP_LOGE(TAG, "Invalid HTTP GET for WebSocket");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    if(ESP_OK != httpd_ws_recv_frame(r, &ws_pkt, 0))//获取帧长度
    { 
        ESP_LOGE(TAG, "Failed to get ws frame length");
        return ESP_FAIL;
    }
    uint8_t* data_buf = (uint8_t*)malloc(ws_pkt.len+1); //为数据分配内存
    if(data_buf == NULL) 
    {
        ESP_LOGE(TAG, "Failed to allocate memory for ws data");
        return ESP_FAIL;
    }
    if(ESP_OK == httpd_ws_recv_frame(r, &ws_pkt, ws_pkt.len)) //接收数据
    { 
        if(ws_pkt.type == HTTPD_WS_TYPE_TEXT) 
        {
            ESP_LOGI(TAG, "Get Websocket message:%s, len=%d", ws_pkt.payload, ws_pkt.len);
            if(ws_receive_callback)
            {
                ws_receive_callback(ws_pkt.payload, ws_pkt.len);
            }
        }
    }
    free(data_buf);
    return ESP_OK;
}


esp_err_t web_ws_server_start(ws_server_config_t* config)
{
    if(config == NULL) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_FAIL;
    }
    http_html = config->html_code;
    ws_receive_callback = config->receive_fn;
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG(); //使用默认配置
    httpd_start(&ws_httpd_server_handle, &http_config);
    httpd_uri_t uri_get =
    {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = ws_get_http_req,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(ws_httpd_server_handle, &uri_get);

    httpd_uri_t uri_ws =
    {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = handle_ws_request,
        .is_websocket = true
    };
    httpd_register_uri_handler(ws_httpd_server_handle, &uri_ws);
    return ESP_OK;
}

esp_err_t web_ws_server_stop(void)
{
    // Implementation of WebSocket server stop
    if(ws_httpd_server_handle)
    {
        httpd_stop(ws_httpd_server_handle);
        ws_httpd_server_handle = NULL;
    }
    return ESP_OK;
}
esp_err_t web_ws_server_send_text(uint8_t* data, int len)
{
    // Implementation of sending text data over WebSocket
    if(ws_httpd_server_handle == NULL)
    {
        ESP_LOGE(TAG, "WebSocket server not started");
        return ESP_FAIL;
    }
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = data;
    ws_pkt.len = len;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_data(ws_httpd_server_handle, 0, &ws_pkt); //0表示发送到所有连接的客户端
}
