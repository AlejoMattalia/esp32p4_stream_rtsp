/**
*******************************************************************************
* @file           : wifi_connect.h
* @brief          : Description of header file
* @author         : 
* @date           : dd/mm/aaaa
*******************************************************************************
* @attention
*
* Copyright (c) <date> grivera. All rights reserved.
*
*/
#ifndef __WIFI_CONNECT_H__
#define __WIFI_CONNECT_H__
/******************************************************************************
        Includes
******************************************************************************/
#pragma once
#include "esp_err.h"
/******************************************************************************
        Constants
******************************************************************************/
#define WIFI_SSID 				"IPTEL164f2"
#define WIFI_PASSWORD 			"iptel1123"
#define WIFI_MAXIMUM_RETRY 		(5)
/******************************************************************************
        Data types
******************************************************************************/

/******************************************************************************
        Public function prototypes
******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa NVS + netif + wifi en modo STA (usando el Wi-Fi remoto
 * provisto por esp_wifi_remote/esp_hosted sobre el coprocesador C6/C2)
 * y bloquea hasta obtener IP o agotar los reintentos configurados.
 *
 * Devuelve ESP_OK si quedo conectado, ESP_FAIL en caso contrario.
 */
esp_err_t wifi_connect_start (void);

/** Devuelve la IP actual en formato "a.b.c.d", o "0.0.0.0" si no hay link. */
void wifi_connect_get_ip_str (char *out, size_t out_len);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // EOF __WIFI_CONNECT_H__
