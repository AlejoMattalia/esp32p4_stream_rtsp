#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "wifi_connect.h"
#include "camera_rtsp.h"

#include "esp_heap_caps.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Validacion de streaming RTSP - ESP32-P4 + OV5647");
	ESP_LOGI(TAG, "PSRAM Libre: %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	ESP_LOGI(TAG, "SRAM Libre:  %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
	vTaskDelay(pdMS_TO_TICKS(1000));
	
    if (wifi_connect_start() != ESP_OK) 
	{
        ESP_LOGE(TAG, "No se pudo conectar a WiFi, abortando");
        return;
    }

    char ip[16];
    wifi_connect_get_ip_str(ip, sizeof(ip));

    // --- Aca la capa de aplicacion elige la resolucion ---
    // Cambiar a CAM_RTSP_RES_720P para 1280x720. En un firmware real
    // este valor podria venir de NVS, de un boton, de un comando por
    // consola, etc. Para esta validacion queda fijo en el codigo.
    cam_rtsp_config_t cfg;
    cam_rtsp_config_default(&cfg);
    cfg.resolution = CAM_RTSP_RES_720P;   // <-- cambiar aca si se quiere 720p

    ESP_ERROR_CHECK(cam_rtsp_init(&cfg));
    ESP_ERROR_CHECK(cam_rtsp_start());

    uint16_t w, h;
    cam_rtsp_get_dimensions(&w, &h);
	// en linux ejecutar ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental -rtsp_flags prefer_tcp rtsp://<IP>:554/stream
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Stream RTSP activo: rtsp://%s:%u/stream", ip, cfg.rtsp_port);
    ESP_LOGI(TAG, "  Resolucion: %ux%u @ %u fps, %lu kbps", w, h, cfg.fps, (unsigned long)(cfg.bitrate_bps / 1000));
    ESP_LOGI(TAG, "  Abrir esa URL con VLC (Media > Abrir ubicacion de red)");
    ESP_LOGI(TAG, "=================================================");
}
