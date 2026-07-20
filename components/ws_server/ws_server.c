#include "ws_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WebSocket Server";
static const char *http_html = NULL;
static ws_receive_cb ws_receive_fn = NULL;
static httpd_handle_t http_ws_server = NULL;
static int client_sockfd = -1;

static esp_err_t handle_ws_req(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        client_sockfd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client connected, fd: %d", client_sockfd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *buf = NULL;
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate websocket buffer");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_receive_fn && buf) {
        ws_receive_fn(buf, ws_pkt.len);
    }

    free(buf);
    return ESP_OK;
}

static esp_err_t get_req_handler(httpd_req_t *req)
{
    if (!http_html) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    return httpd_resp_send(req, http_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t web_ws_send(uint8_t *data, int len)
{
    if (!http_ws_server || client_sockfd < 0 || !data || len <= 0) {
        return ESP_FAIL;
    }

    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(httpd_ws_frame_t));
    pkt.payload = data;
    pkt.len = len;
    pkt.type = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_data(http_ws_server, client_sockfd, &pkt);
}

esp_err_t web_ws_start(ws_cfg_t *cfg)
{
    if (!cfg || !cfg->html_code) {
        return ESP_FAIL;
    }

    if (http_ws_server) {
        return ESP_OK;
    }

    http_html = cfg->html_code;
    ws_receive_fn = cfg->receive_fn;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.max_resp_headers = 12;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_req_handler,
    };
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .is_websocket = true,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };

    esp_err_t ret = httpd_start(&http_ws_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        http_ws_server = NULL;
        return ret;
    }

    ESP_ERROR_CHECK(httpd_register_uri_handler(http_ws_server, &uri_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_ws_server, &ws));
    ESP_ERROR_CHECK(httpd_register_uri_handler(http_ws_server, &favicon));
    return ESP_OK;
}

esp_err_t web_ws_stop(void)
{
    if (!http_ws_server) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(http_ws_server);
    http_ws_server = NULL;
    client_sockfd = -1;
    return ret;
}
