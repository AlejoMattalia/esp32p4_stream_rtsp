#include "ble_provisioning.h"

#include <assert.h>
#include <string.h>
#include "anny_config.h"
#include "cJSON.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_DEVICE_NAME "ANNY-CAM"

/* 9f4e0001-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x4e, 0x9f);
/* 9f4e0002-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t s_config_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x4e, 0x9f);

static const char *TAG = "[BLE_SETUP]";
static uint8_t s_own_addr_type;

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

static int config_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length < 2 || length > 768) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char body[769];
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, body, sizeof(body) - 1, &copied) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    body[copied] = '\0';

    cJSON *root = cJSON_Parse(body);
    anny_config_t config = {0};
    const bool valid = root &&
        json_string(root, "wifiSsid", config.wifi_ssid, sizeof(config.wifi_ssid), false) &&
        json_string(root, "wifiPassword", config.wifi_password, sizeof(config.wifi_password), true) &&
        json_string(root, "cameraId", config.camera_id, sizeof(config.camera_id), false) &&
        json_string(root, "deviceSecret", config.device_secret, sizeof(config.device_secret), false);
    cJSON_Delete(root);

    if (!valid || anny_config_save(&config) != ESP_OK) {
        ESP_LOGE(TAG, "Configuracion BLE invalida");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    ESP_LOGI(TAG, "Configuracion recibida para %s; reiniciando", config.camera_id);
    xTaskCreate(restart_task, "anny_restart", 2048, NULL, 5, NULL);
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {{
        .uuid = &s_config_uuid.u,
        .access_cb = config_access,
        .flags = BLE_GATT_CHR_F_WRITE,
    }, {0}},
}, {0}};

static void advertise(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISCONNECT || event->type == BLE_GAP_EVENT_ADV_COMPLETE)
        advertise();
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(TAG, "No se pudo preparar el anuncio BLE");
        return;
    }
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                                     &params, gap_event, NULL);
    if (rc == 0) ESP_LOGW(TAG, "Configuracion BLE activa como %s", BLE_DEVICE_NAME);
    else ESP_LOGE(TAG, "No se pudo anunciar BLE: %d", rc);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "No se pudo obtener la direccion BLE");
        return;
    }
    advertise();
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_provisioning_start(bool connect_hosted)
{
    if (connect_hosted) ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_init());
    ESP_ERROR_CHECK(esp_hosted_bt_controller_enable());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(BLE_DEVICE_NAME));
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
