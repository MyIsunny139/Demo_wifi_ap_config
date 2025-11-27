
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ws_server.h"

static const char *TAG = "WebSocket Server";
//html页面
static const char* http_html = NULL;
//接收回调函数
static ws_receive_cb  ws_receive_fn = NULL;
//http服务器句柄
httpd_handle_t http_ws_server = NULL;
//连接的客户端fds
static int client_sockfd = -1;


/** 当其他设备WS访问时触发此回调函数
 * @param req http请求
 * @return ESP_OK or ESP_FAIL
*/
static esp_err_t handle_ws_req(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WebSocket handshake done, connection opened");
        //把套接字描述符保存下来，方便后续发送数据用
        client_sockfd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG,"WebSocket client connected, fd: %d",client_sockfd);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "WebSocket data frame received");
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (ws_pkt.len)
    {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL)
        {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        ESP_LOGI(TAG, "Text frame, calling receive callback");
        if(ws_receive_fn) {
            ws_receive_fn(ws_pkt.payload,ws_pkt.len);
        } else {
            ESP_LOGW(TAG, "No receive callback registered!");
        }
        free(buf);
    }
    return ESP_OK;
}

/** 当其他设备http HTTP_GET 访问时，返回html页面
 * @param req http请求
 * @return ESP_OK or ESP_FAIL
*/
esp_err_t get_req_handler(httpd_req_t *req)
{
    esp_err_t response = ESP_FAIL;
    if(http_html)
    {
        response = httpd_resp_send(req, http_html, HTTPD_RESP_USE_STRLEN);
    }
    return response;
}

/** favicon 请求处理(避免 404)
 * @param req http请求
 * @return ESP_OK
*/
esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t   web_ws_send(uint8_t* data, int len)
{
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(httpd_ws_frame_t));
    pkt.payload = data;
    pkt.len = len;
    pkt.type = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_data(http_ws_server,client_sockfd,&pkt);
}

/** 初始化ws
 * @param cfg ws一些配置,请看ws_cfg_t定义
 * @return  ESP_OK or ESP_FAIL
*/
esp_err_t   web_ws_start(ws_cfg_t *cfg)
{
    if(cfg == NULL)
        return ESP_FAIL;
    http_html = cfg->html_code;
    ws_receive_fn = cfg->receive_fn;

    //http和websocket初始化
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;          // 增加 URI 处理器数量
    config.max_resp_headers = 12;          // 增加响应头数量
    config.stack_size = 10240;             // 增加栈大小到 10KB
    config.lru_purge_enable = true;        // 启用 LRU 清除
    
    httpd_uri_t uri_get = 
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_req_handler,
    };
    httpd_uri_t ws = 
    {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .is_websocket = true
    };
    httpd_uri_t favicon = 
    {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };

    ESP_LOGI(TAG, "Starting HTTP server...");
    esp_err_t ret = httpd_start(&http_ws_server, &config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP server started");
        httpd_register_uri_handler(http_ws_server, &uri_get);
        httpd_register_uri_handler(http_ws_server, &ws);
        httpd_register_uri_handler(http_ws_server, &favicon);
        ESP_LOGI(TAG, "Registered URI handlers: /, /ws, /favicon.ico");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
}

esp_err_t   web_ws_stop(void)
{
    if(http_ws_server)
    {
        return httpd_stop(http_ws_server);
        http_ws_server = NULL;
    }
    return ESP_OK;
}
