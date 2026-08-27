#include <string.h>
#include "wifi_connect.h"
#include "device_config.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "[WIFI]";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static esp_netif_t *s_netif;
static bool s_initialized;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num++ < WIFI_MAXIMUM_RETRY) esp_wifi_connect();
        else xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_initialize(void)
{
    if (s_initialized) return ESP_OK;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_connect_start(const anny_config_t *config)
{
    if (!config || !config->wifi_ssid[0]) return ESP_ERR_INVALID_ARG;
    ESP_ERROR_CHECK(wifi_initialize());
    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, config->wifi_ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, config->wifi_password, sizeof(station.sta.password));
    station.sta.threshold.authmode = config->wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando a '%s'", config->wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

static bool json_string(cJSON *root, const char *key, char *out, size_t size, bool empty_ok)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || (!empty_ok && !item->valuestring[0]) ||
        strlen(item->valuestring) >= size) return false;
    strlcpy(out, item->valuestring, size);
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t config_handler(httpd_req_t *request)
{
    if (request->content_len < 2 || request->content_len > 768)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "JSON inválido");
    char body[769];
    size_t offset = 0;
    while (offset < request->content_len) {
        int read = httpd_req_recv(request, body + offset, request->content_len - offset);
        if (read <= 0) return ESP_FAIL;
        offset += read;
    }
    body[offset] = '\0';
    cJSON *root = cJSON_Parse(body);
    anny_config_t config = {0};
    bool valid = root &&
        json_string(root, "wifiSsid", config.wifi_ssid, sizeof(config.wifi_ssid), false) &&
        json_string(root, "wifiPassword", config.wifi_password, sizeof(config.wifi_password), true) &&
        json_string(root, "cameraId", config.camera_id, sizeof(config.camera_id), false) &&
        json_string(root, "deviceSecret", config.device_secret, sizeof(config.device_secret), false);
    cJSON_Delete(root);
    if (!valid || anny_config_save(&config) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Datos incompletos o inválidos");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "anny_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"device\":\"anny-camera\"}");
}

esp_err_t wifi_start_provisioning(void)
{
    ESP_ERROR_CHECK(wifi_initialize());
    wifi_config_t access_point = {0};
    strlcpy((char *)access_point.ap.ssid, ANNY_PROVISIONING_AP_SSID, sizeof(access_point.ap.ssid));
    strlcpy((char *)access_point.ap.password, ANNY_PROVISIONING_AP_PASSWORD,
            sizeof(access_point.ap.password));
    access_point.ap.ssid_len = strlen(ANNY_PROVISIONING_AP_SSID);
    access_point.ap.max_connection = 2;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = ANNY_PROVISIONING_HTTP_PORT;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &http_config));
    const httpd_uri_t health = {.uri="/api/health", .method=HTTP_GET, .handler=health_handler};
    const httpd_uri_t configure = {.uri="/api/config", .method=HTTP_POST, .handler=config_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &health));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &configure));
    ESP_LOGW(TAG, "Configuración activa en %s (clave %s)", ANNY_PROVISIONING_AP_SSID,
             ANNY_PROVISIONING_AP_PASSWORD);
    return ESP_OK;
}

void wifi_connect_get_ip_str(char *out, size_t length)
{
    esp_netif_ip_info_t info;
    if (s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK)
        snprintf(out, length, IPSTR, IP2STR(&info.ip));
    else snprintf(out, length, "0.0.0.0");
}
