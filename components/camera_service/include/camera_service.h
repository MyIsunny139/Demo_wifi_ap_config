#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include "esp_camera.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t camera_service_init(void);
esp_err_t camera_service_recover(void);
bool camera_service_is_ready(void);
camera_fb_t *camera_service_get_frame(void);
void camera_service_return_frame(camera_fb_t *frame);

#endif
