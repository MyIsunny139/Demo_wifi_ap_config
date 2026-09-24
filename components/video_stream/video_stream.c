#include "video_stream.h"

#include "camera_service.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "status_led.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VIDEO_HTTP_PORT 81
#define VIDEO_HTTP_CONTROL_PORT 32769
#define VIDEO_FRAME_PERIOD_MS 250
#define VIDEO_STATS_PERIOD_MS 60000
#define VIDEO_WARMUP_FRAMES 8
#define VIDEO_INVALID_FRAMES_BEFORE_RECOVERY 3
#define VIDEO_MAX_COMMAND_SIZE 512
#define VIDEO_TASK_STACK 4096
#define VIDEO_HTTP_TASK_STACK 16384

static const char *TAG = "video_stream";
static httpd_handle_t server;
static SemaphoreHandle_t state_mutex;
static SemaphoreHandle_t send_mutex;
static int client_fd = -1;
static int last_command_id = -1;
static int warmup_frames_remaining;
static bool initialized;

static bool find_jpeg_payload(const uint8_t *buffer, size_t length,
                              const uint8_t **payload_out, size_t *length_out)
{
    if (!buffer || length < 4) {
        return false;
    }

    size_t start = length;
    for (size_t i = 0; i + 2 < length; ++i) {
        if (buffer[i] == 0xff && buffer[i + 1] == 0xd8 && buffer[i + 2] == 0xff) {
            start = i;
            break;
        }
    }
    if (start == length) {
        return false;
    }

    for (size_t end = length - 2; end > start; --end) {
        if (buffer[end] == 0xff && buffer[end + 1] == 0xd9) {
            *payload_out = buffer + start;
            *length_out = end + 2 - start;
            return true;
        }
    }
    return false;
}

static void wait_for_next_frame_period(TickType_t started)
{
    const TickType_t elapsed = xTaskGetTickCount() - started;
    const TickType_t period = pdMS_TO_TICKS(VIDEO_FRAME_PERIOD_MS);
    if (elapsed < period) {
        vTaskDelay(period - elapsed);
    } else {
        taskYIELD();
    }
}

static void maybe_log_stream_stats(uint32_t *valid_sent, uint32_t *dropped,
                                   uint32_t *trimmed, uint32_t *warmup,
                                   uint32_t *timeouts, uint32_t *send_fail,
                                   TickType_t *last_stats_at)
{
    const TickType_t now = xTaskGetTickCount();
    if (*last_stats_at == 0) {
        *last_stats_at = now;
        return;
    }

    if (now - *last_stats_at < pdMS_TO_TICKS(VIDEO_STATS_PERIOD_MS)) {
        return;
    }

    const uint32_t total = *valid_sent + *dropped + *warmup + *timeouts + *send_fail;
    if (total == 0 && *trimmed == 0) {
        *last_stats_at = now;
        return;
    }

    ESP_LOGI(TAG,
             "Video stats: valid=%lu dropped=%lu trimmed=%lu warmup=%lu timeouts=%lu send_fail=%lu total=%lu",
             (unsigned long)*valid_sent, (unsigned long)*dropped,
             (unsigned long)*trimmed, (unsigned long)*warmup,
             (unsigned long)*timeouts, (unsigned long)*send_fail,
             (unsigned long)total);

    *valid_sent = 0;
    *dropped = 0;
    *trimmed = 0;
    *warmup = 0;
    *timeouts = 0;
    *send_fail = 0;
    *last_stats_at = now;
}

static int replace_client(int fd)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    const int previous_fd = client_fd;
    client_fd = fd;
    warmup_frames_remaining = VIDEO_WARMUP_FRAMES;
    xSemaphoreGive(state_mutex);
    return previous_fd;
}

static bool consume_warmup_frame(void)
{
    bool skip = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (warmup_frames_remaining > 0) {
        warmup_frames_remaining--;
        skip = true;
    }
    xSemaphoreGive(state_mutex);
    return skip;
}

static bool clear_client_if(int fd)
{
    bool cleared = false;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (client_fd == fd) {
        client_fd = -1;
        cleared = true;
    }
    xSemaphoreGive(state_mutex);
    return cleared;
}

static void get_connection(httpd_handle_t *server_out, int *fd_out)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    *server_out = server;
    *fd_out = client_fd;
    xSemaphoreGive(state_mutex);
}

static esp_err_t send_ack(httpd_req_t *req, int command_id, bool applied, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddNumberToObject(root, "command_id", command_id);
    cJSON_AddBoolToObject(root, "applied", applied);
    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    esp_err_t err = httpd_ws_send_frame(req, &frame);
    cJSON_free(payload);
    return err;
}

static bool parse_risk(const char *value, status_led_risk_t *risk)
{
    if (strcmp(value, "low") == 0) {
        *risk = STATUS_LED_RISK_LOW;
    } else if (strcmp(value, "medium") == 0) {
        *risk = STATUS_LED_RISK_MEDIUM;
    } else if (strcmp(value, "high") == 0) {
        *risk = STATUS_LED_RISK_HIGH;
    } else {
        return false;
    }
    return true;
}

static bool is_nonnegative_int(const cJSON *value)
{
    return cJSON_IsNumber(value) && value->valuedouble >= 0 &&
           value->valuedouble <= INT_MAX && value->valuedouble == (double)value->valueint;
}

static bool is_probability(const cJSON *value)
{
    return cJSON_IsNumber(value) && value->valuedouble >= 0.0 &&
           value->valuedouble <= 1.0;
}

static esp_err_t handle_risk_command(httpd_req_t *req, const uint8_t *payload, size_t length)
{
    char *json = calloc(1, length + 1);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, payload, length);

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        return send_ack(req, -1, false, "invalid_json");
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *command_id = cJSON_GetObjectItemCaseSensitive(root, "command_id");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(root, "level");
    const cJSON *people = cJSON_GetObjectItemCaseSensitive(root, "people");
    const cJSON *smoking = cJSON_GetObjectItemCaseSensitive(root, "smoking");
    const cJSON *smoking_confidence =
        cJSON_GetObjectItemCaseSensitive(root, "smoking_confidence");

    int id = -1;
    esp_err_t result = ESP_OK;
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "risk") != 0 ||
        !is_nonnegative_int(command_id) || !cJSON_IsString(level) ||
        !is_nonnegative_int(people) ||
        (smoking && !is_nonnegative_int(smoking)) ||
        (smoking_confidence && !is_probability(smoking_confidence))) {
        result = send_ack(req, id, false, "invalid_command");
        cJSON_Delete(root);
        return result;
    }

    id = command_id->valueint;
    status_led_risk_t risk;
    if (!parse_risk(level->valuestring, &risk)) {
        result = send_ack(req, id, false, "invalid_level");
    } else if (id == last_command_id) {
        result = send_ack(req, id, true, NULL);
    } else {
        esp_err_t led_result = status_led_set_risk(risk);
        if (led_result == ESP_OK) {
            last_command_id = id;
            result = send_ack(req, id, true, NULL);
            ESP_LOGI(TAG,
                     "Risk command %d applied: level=%s people=%d smoking=%d confidence=%.3f",
                     id, level->valuestring, people->valueint,
                     smoking ? smoking->valueint : 0,
                     smoking_confidence ? smoking_confidence->valuedouble : 0.0);
        } else {
            result = send_ack(req, id, false, "led_unavailable");
        }
    }

    cJSON_Delete(root);
    return result;
}

static esp_err_t video_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        const int fd = httpd_req_to_sockfd(req);
        const int previous_fd = replace_client(fd);
        last_command_id = -1;
        status_led_clear();
        if (previous_fd >= 0 && previous_fd != fd) {
            httpd_sess_trigger_close(req->handle, previous_fd);
            ESP_LOGI(TAG, "Replaced video client fd=%d with fd=%d", previous_fd, fd);
        }
        ESP_LOGI(TAG, "Video client connected, fd=%d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len > VIDEO_MAX_COMMAND_SIZE) {
        return send_ack(req, -1, false, "command_too_large");
    }

    uint8_t *payload = NULL;
    if (frame.len > 0) {
        payload = malloc(frame.len);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = payload;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
    }

    if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT && payload) {
        err = handle_risk_command(req, payload, frame.len);
    } else if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_CLOSE) {
        if (clear_client_if(httpd_req_to_sockfd(req))) {
            status_led_clear();
        }
    }

    free(payload);
    return err;
}

static void capture_task(void *context)
{
    uint32_t valid_sent = 0;
    uint32_t dropped = 0;
    uint32_t trimmed = 0;
    uint32_t warmup = 0;
    uint32_t timeouts = 0;
    uint32_t send_fail = 0;
    uint32_t consecutive_invalid = 0;
    TickType_t last_stats_at = 0;

    while (true) {
        const TickType_t started = xTaskGetTickCount();
        xSemaphoreTake(send_mutex, portMAX_DELAY);

        httpd_handle_t active_server;
        int fd;
        get_connection(&active_server, &fd);
        if (active_server && fd >= 0 &&
            httpd_ws_get_fd_info(active_server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            camera_fb_t *frame_buffer = camera_service_get_frame();
            if (!frame_buffer) {
                timeouts++;
                ESP_LOGW(TAG, "Camera frame unavailable");
                consecutive_invalid = 0;
                if (camera_service_recover() != ESP_OK) {
                    ESP_LOGE(TAG, "Camera recovery failed");
                }
            } else {
                const uint8_t *jpeg_payload = NULL;
                size_t jpeg_length = 0;
                if (!find_jpeg_payload(frame_buffer->buf, frame_buffer->len,
                                       &jpeg_payload, &jpeg_length)) {
                    static uint32_t dropped_frames = 0;
                    dropped_frames++;
                    dropped++;
                    consecutive_invalid++;
                    if (dropped_frames == 1 || dropped_frames % 100 == 0) {
                        ESP_LOGW(TAG, "Dropped invalid JPEG frame, len=%u, head=%02x %02x %02x %02x %02x %02x %02x %02x, dropped=%lu",
                                 (unsigned)frame_buffer->len,
                                 (unsigned)(frame_buffer->len > 0 ? frame_buffer->buf[0] : 0),
                                 (unsigned)(frame_buffer->len > 1 ? frame_buffer->buf[1] : 0),
                                 (unsigned)(frame_buffer->len > 2 ? frame_buffer->buf[2] : 0),
                                 (unsigned)(frame_buffer->len > 3 ? frame_buffer->buf[3] : 0),
                                 (unsigned)(frame_buffer->len > 4 ? frame_buffer->buf[4] : 0),
                                 (unsigned)(frame_buffer->len > 5 ? frame_buffer->buf[5] : 0),
                                 (unsigned)(frame_buffer->len > 6 ? frame_buffer->buf[6] : 0),
                                 (unsigned)(frame_buffer->len > 7 ? frame_buffer->buf[7] : 0),
                                 (unsigned long)dropped_frames);
                    }
                    camera_service_return_frame(frame_buffer);
                    if (consecutive_invalid >= VIDEO_INVALID_FRAMES_BEFORE_RECOVERY) {
                        consecutive_invalid = 0;
                        if (camera_service_recover() != ESP_OK) {
                            ESP_LOGE(TAG, "Camera recovery failed");
                        }
                    }
                    xSemaphoreGive(send_mutex);
                    maybe_log_stream_stats(&valid_sent, &dropped, &trimmed, &warmup,
                                           &timeouts, &send_fail, &last_stats_at);
                    wait_for_next_frame_period(started);
                    continue;
                }
                consecutive_invalid = 0;
                if (jpeg_payload != frame_buffer->buf || jpeg_length != frame_buffer->len) {
                    static uint32_t trimmed_frames = 0;
                    trimmed_frames++;
                    trimmed++;
                    if (trimmed_frames == 1 || trimmed_frames % 100 == 0) {
                        ESP_LOGW(TAG, "Trimmed JPEG frame %u -> %u bytes, trimmed=%lu",
                                 (unsigned)frame_buffer->len, (unsigned)jpeg_length,
                                 (unsigned long)trimmed_frames);
                    }
                }
                if (consume_warmup_frame()) {
                    warmup++;
                    camera_service_return_frame(frame_buffer);
                    xSemaphoreGive(send_mutex);
                    maybe_log_stream_stats(&valid_sent, &dropped, &trimmed, &warmup,
                                           &timeouts, &send_fail, &last_stats_at);
                    wait_for_next_frame_period(started);
                    continue;
                }
                httpd_ws_frame_t frame = {
                    .type = HTTPD_WS_TYPE_BINARY,
                    .payload = (uint8_t *)jpeg_payload,
                    .len = jpeg_length,
                };
                esp_err_t err = httpd_ws_send_data(active_server, fd, &frame);
                camera_service_return_frame(frame_buffer);
                if (err != ESP_OK) {
                    send_fail++;
                    ESP_LOGW(TAG, "Video send failed: %s", esp_err_to_name(err));
                    if (clear_client_if(fd)) {
                        status_led_clear();
                    }
                } else {
                    valid_sent++;
                }
            }
        } else if (fd >= 0) {
            if (clear_client_if(fd)) {
                status_led_clear();
            }
        }

        xSemaphoreGive(send_mutex);
        maybe_log_stream_stats(&valid_sent, &dropped, &trimmed, &warmup,
                               &timeouts, &send_fail, &last_stats_at);
        wait_for_next_frame_period(started);
    }
}

esp_err_t video_stream_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    if (!camera_service_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    state_mutex = xSemaphoreCreateMutex();
    if (!state_mutex) {
        return ESP_ERR_NO_MEM;
    }
    send_mutex = xSemaphoreCreateMutex();
    if (!send_mutex) {
        vSemaphoreDelete(state_mutex);
        state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(capture_task, "video_capture", VIDEO_TASK_STACK, NULL, 4,
                                NULL, 1) != pdPASS) {
        vSemaphoreDelete(send_mutex);
        vSemaphoreDelete(state_mutex);
        send_mutex = NULL;
        state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    return ESP_OK;
}

esp_err_t video_stream_start(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(send_mutex, portMAX_DELAY);
    httpd_handle_t active_server;
    int fd;
    get_connection(&active_server, &fd);
    if (active_server) {
        xSemaphoreGive(send_mutex);
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = VIDEO_HTTP_PORT;
    config.ctrl_port = VIDEO_HTTP_CONTROL_PORT;
    config.core_id = 1;
    config.stack_size = VIDEO_HTTP_TASK_STACK;
    config.max_open_sockets = 4;
    config.send_wait_timeout = 2;
    config.recv_wait_timeout = 2;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&active_server, &config);
    if (err == ESP_OK) {
        const httpd_uri_t video_uri = {
            .uri = "/video",
            .method = HTTP_GET,
            .handler = video_ws_handler,
            .is_websocket = true,
        };
        err = httpd_register_uri_handler(active_server, &video_uri);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        server = active_server;
        client_fd = -1;
        xSemaphoreGive(state_mutex);
        ESP_LOGI(TAG, "Video WebSocket ready at ws://<device-ip>:%d/video", VIDEO_HTTP_PORT);
    } else if (active_server) {
        httpd_stop(active_server);
    }
    xSemaphoreGive(send_mutex);
    return err;
}

esp_err_t video_stream_stop(void)
{
    if (!initialized) {
        return ESP_OK;
    }

    xSemaphoreTake(send_mutex, portMAX_DELAY);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    httpd_handle_t active_server = server;
    server = NULL;
    client_fd = -1;
    xSemaphoreGive(state_mutex);

    status_led_clear();
    esp_err_t err = active_server ? httpd_stop(active_server) : ESP_OK;
    xSemaphoreGive(send_mutex);
    return err;
}

bool video_stream_is_running(void)
{
    if (!initialized) {
        return false;
    }
    httpd_handle_t active_server;
    int fd;
    get_connection(&active_server, &fd);
    return active_server != NULL;
}
