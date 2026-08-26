#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "wifi_connect.h"
#include "camera_rtsp.h"
#include "anny_config.h"
#include "device_config.h"
#include "rtsp_publisher.h"

#include "esp_heap_caps.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_ERROR_CHECK(anny_config_init());
    anny_config_t device;
    if (anny_config_load(&device) != ESP_OK) {
        ESP_LOGW(TAG, "Placa sin configurar; iniciando red de configuración");
        ESP_ERROR_CHECK(wifi_start_provisioning());
        return;
    }
    ESP_LOGI(TAG, "Validacion de streaming RTSP - ESP32-P4 + OV5647");
	ESP_LOGI(TAG, "PSRAM Libre: %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	ESP_LOGI(TAG, "SRAM Libre:  %d KB", (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
	vTaskDelay(pdMS_TO_TICKS(1000));
	
    if (wifi_connect_start(&device) != ESP_OK)
	{
        ESP_LOGW(TAG, "Falló la red guardada; iniciando red de configuración");
        ESP_ERROR_CHECK(wifi_start_provisioning());
        return;
    }

    ESP_LOGI(TAG, "Placa configurada: %s", device.camera_id);
    ESP_LOGI(TAG, "Servidor remoto: %s:%u", ANNY_SERVER_HOST, ANNY_SERVER_PORT);

    // --- Aca la capa de aplicacion elige la resolucion ---
    // Cambiar a CAM_RTSP_RES_720P para 1280x720. En un firmware real
    // este valor podria venir de NVS, de un boton, de un comando por
    // consola, etc. Para esta validacion queda fijo en el codigo.
    cam_rtsp_config_t cfg;
    cam_rtsp_config_default(&cfg);
    cfg.resolution = CAM_RTSP_RES_720P;   // <-- cambiar aca si se quiere 720p
    cfg.fps = 20;
    /* Deja margen al enlace TCP para sostener todos los frames sin cola. */
    cfg.bitrate_bps = 1000000;

    ESP_ERROR_CHECK(cam_rtsp_init(&cfg));
    ESP_ERROR_CHECK(cam_rtsp_start_capture());

    const rtsp_publisher_config_t publisher = {
        .host = ANNY_SERVER_HOST,
        .port = ANNY_SERVER_PORT,
        .camera_id = device.camera_id,
        .device_secret = device.device_secret,
    };
    ESP_ERROR_CHECK(rtsp_publisher_start(&publisher));

    uint16_t w, h;
    cam_rtsp_get_dimensions(&w, &h);
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  Publicacion remota iniciada para %s", device.camera_id);
    ESP_LOGI(TAG, "  Resolucion: %ux%u @ %u fps, %lu kbps", w, h, cfg.fps, (unsigned long)(cfg.bitrate_bps / 1000));
    ESP_LOGI(TAG, "  Abrir esa URL con VLC (Media > Abrir ubicacion de red)");
    ESP_LOGI(TAG, "=================================================");
}
