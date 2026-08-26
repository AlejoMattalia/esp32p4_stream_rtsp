#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char camera_id[64];
    char device_secret[128];
} anny_config_t;

esp_err_t anny_config_init(void);
esp_err_t anny_config_load(anny_config_t *config);
esp_err_t anny_config_save(const anny_config_t *config);
bool anny_config_is_valid(const anny_config_t *config);
