#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Inicia el aprovisionamiento BLE. connect_hosted debe ser true cuando el
 * transporte al coprocesador todavia no fue inicializado por Wi-Fi. */
esp_err_t ble_provisioning_start(bool connect_hosted);
