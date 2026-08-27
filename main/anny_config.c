#include "anny_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_NAMESPACE "anny"

static void read_string(nvs_handle_t handle, const char *key, char *value, size_t capacity)
{
    size_t length = capacity;
    if (nvs_get_str(handle, key, value, &length) != ESP_OK) value[0] = '\0';
}

esp_err_t anny_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t anny_config_load(anny_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    read_string(handle, "ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    read_string(handle, "wifi_pass", config->wifi_password, sizeof(config->wifi_password));
    read_string(handle, "camera_id", config->camera_id, sizeof(config->camera_id));
    read_string(handle, "dev_secret", config->device_secret, sizeof(config->device_secret));
    nvs_close(handle);
    return anny_config_is_valid(config) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t anny_config_save(const anny_config_t *config)
{
    if (!anny_config_is_valid(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_str(handle, "ssid", config->wifi_ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, "wifi_pass", config->wifi_password)) == ESP_OK &&
        (err = nvs_set_str(handle, "camera_id", config->camera_id)) == ESP_OK &&
        (err = nvs_set_str(handle, "dev_secret", config->device_secret)) == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t anny_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool anny_config_is_valid(const anny_config_t *config)
{
    return config && config->wifi_ssid[0] && config->camera_id[0] && config->device_secret[0];
}
