#include "camera_service.h"

#include "esp_log.h"
#include "sensor.h"

#define CAMERA_PIN_PWDN -1
#define CAMERA_PIN_RESET -1
#define CAMERA_PIN_XCLK 15
#define CAMERA_PIN_SIOD 4
#define CAMERA_PIN_SIOC 5
#define CAMERA_PIN_D0 11
#define CAMERA_PIN_D1 9
#define CAMERA_PIN_D2 8
#define CAMERA_PIN_D3 10
#define CAMERA_PIN_D4 12
#define CAMERA_PIN_D5 18
#define CAMERA_PIN_D6 17
#define CAMERA_PIN_D7 16
#define CAMERA_PIN_VSYNC 6
#define CAMERA_PIN_HREF 7
#define CAMERA_PIN_PCLK 13

static const char *TAG = "camera_service";
static bool camera_ready;

esp_err_t camera_service_init(void)
{
    if (camera_ready) {
        return ESP_OK;
    }

    const camera_config_t config = {
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = CAMERA_PIN_XCLK,
        .pin_sccb_sda = CAMERA_PIN_SIOD,
        .pin_sccb_scl = CAMERA_PIN_SIOC,
        .pin_d7 = CAMERA_PIN_D7,
        .pin_d6 = CAMERA_PIN_D6,
        .pin_d5 = CAMERA_PIN_D5,
        .pin_d4 = CAMERA_PIN_D4,
        .pin_d3 = CAMERA_PIN_D3,
        .pin_d2 = CAMERA_PIN_D2,
        .pin_d1 = CAMERA_PIN_D1,
        .pin_d0 = CAMERA_PIN_D0,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = -1,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV5640_PID) {
        const uint16_t pid = sensor ? sensor->id.PID : 0;
        ESP_LOGE(TAG, "Expected OV5640, detected PID 0x%04x", pid);
        esp_camera_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }

    camera_ready = true;
    ESP_LOGI(TAG, "OV5640 ready: VGA JPEG, quality=12, frame_buffers=2");
    return ESP_OK;
}

bool camera_service_is_ready(void)
{
    return camera_ready;
}

camera_fb_t *camera_service_get_frame(void)
{
    return camera_ready ? esp_camera_fb_get() : NULL;
}

void camera_service_return_frame(camera_fb_t *frame)
{
    if (frame) {
        esp_camera_fb_return(frame);
    }
}
