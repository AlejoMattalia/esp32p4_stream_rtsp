#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "wifi_connect.h"
#include "camera_rtsp.h"
#include "device_config.h"
#include "rtsp_publisher.h"

#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "app_main";

void app_main(void)
{
    if (strcmp(ANNY_CAMERA_ID, "camera_XXXXXXXXXXXX") == 0 ||
        strcmp(ANNY_DEVICE_SECRET, "SECRETO_DEL_PANEL") == 0 ||
        strcmp(ANNY_WIFI_SSID, "NOMBRE_DEL_WIFI") == 0) {
        ESP_LOGE(TAG, "Completar main/device_config.h antes de flashear");
        return;
    }
    ESP_LOGI(TAG, "Validacion de streaming RTSP - ESP32-P4 + OV5647");
	ESP_LOGI(TAG, "PSRAM Libre: %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	ESP_LOGI(TAG, "SRAM Libre:  %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
	vTaskDelay(pdMS_TO_TICKS(1000));
	
    if (wifi_connect_start() != ESP_OK) 
	{
        ESP_LOGE(TAG, "No se pudo conectar a WiFi, abortando");
        return;
    }

    ESP_LOGI(TAG, "Placa configurada: %s", ANNY_CAMERA_ID);
    ESP_LOGI(TAG, "Servidor remoto: %s:%u", ANNY_SERVER_HOST, ANNY_SERVER_PORT);

    // --- Aca la capa de aplicacion elige la resolucion ---
    // Cambiar a CAM_RTSP_RES_720P para 1280x720. En un firmware real
    // este valor podria venir de NVS, de un boton, de un comando por
    // consola, etc. Para esta validacion queda fijo en el codigo.
    cam_rtsp_config_t cfg;
    cam_rtsp_config_default(&cfg);
    cfg.resolution = CAM_RTSP_RES_800X800;   // <-- cambiar aca si se quiere 720p
    cfg.fps = 20;     // Aumentado a 20 FPS para fluidez
    cfg.bitrate_bps = 2500000;  // Aumentado a 2.5 Mbps para mejor calidad

    ESP_ERROR_CHECK(cam_rtsp_init(&cfg));
    ESP_ERROR_CHECK(cam_rtsp_start_capture());

    const rtsp_publisher_config_t publisher = {
        .host = ANNY_SERVER_HOST,
        .port = ANNY_SERVER_PORT,
        .camera_id = ANNY_CAMERA_ID,
        .device_secret = ANNY_DEVICE_SECRET,
    };
    ESP_ERROR_CHECK(rtsp_publisher_start(&publisher));

    uint16_t w, h;
    cam_rtsp_get_dimensions(&w, &h);
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Publicacion remota iniciada para %s", ANNY_CAMERA_ID);
    ESP_LOGI(TAG, "  Resolucion: %ux%u @ %u fps, %lu kbps", w, h, cfg.fps, (unsigned long)(cfg.bitrate_bps / 1000));
    ESP_LOGI(TAG, "  Abrir esa URL con VLC (Media > Abrir ubicacion de red)");
    ESP_LOGI(TAG, "=================================================");
}
