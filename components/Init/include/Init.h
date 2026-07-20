#ifndef APP_INIT_H
#define APP_INIT_H

#include "esp_err.h"

/** Validate the board profile, then initialize persistent storage and Wi-Fi. */
esp_err_t app_system_init(void);

#endif
