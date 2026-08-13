#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;
    uint16_t port;
    const char *camera_id;
    const char *device_secret;
} rtsp_publisher_config_t;

esp_err_t rtsp_publisher_start(const rtsp_publisher_config_t *config);
esp_err_t rtsp_publisher_stop(void);

#ifdef __cplusplus
}
#endif
