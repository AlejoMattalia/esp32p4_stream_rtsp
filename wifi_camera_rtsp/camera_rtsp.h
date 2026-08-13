/**
*******************************************************************************
* @file           : camera_rtsp.h
* @brief          : Description of header file
* @author         : 
* @date           : dd/mm/aaaa
*******************************************************************************
* @attention
*
* Copyright (c) <date> grivera. All rights reserved.
*
*/
#ifndef __CAMERA_RTSP_H__
#define __CAMERA_RTSP_H__
/******************************************************************************
        Includes
******************************************************************************/
#pragma once
#include <stdint.h>
#include "esp_err.h"
/******************************************************************************
        Constants
******************************************************************************/

/******************************************************************************
        Data types
******************************************************************************/
typedef enum 
{
    CAM_RTSP_RES_VGA = 0,   // 640x480
	CAM_RTSP_RES_800X640,
	CAM_RTSP_RES_800X800,
	CAM_RTSP_RES_720P,      // 1280x720
	
} cam_rtsp_resolution_t;

typedef struct 
{
    cam_rtsp_resolution_t resolution;
    uint8_t  fps;             // frames por segundo objetivo
    uint32_t bitrate_bps;     // bitrate objetivo del encoder H.264
    uint16_t rtsp_port;       // puerto TCP del servidor RTSP (554 tipico)
	
} cam_rtsp_config_t;
/******************************************************************************
        Public function prototypes
******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

/** Llena cfg con los valores por defecto tomados de Kconfig. */
void cam_rtsp_config_default(cam_rtsp_config_t *cfg);

/**
 * Inicializa sensor OV5647 (CSI), ISP y encoder H.264 por hardware
 * segun la resolucion pedida en cfg. Debe llamarse una sola vez,
 * antes de cam_rtsp_start(). Para cambiar de resolucion hay que
 * cam_rtsp_stop() -> cam_rtsp_deinit() -> cam_rtsp_init() de nuevo.
 */
esp_err_t cam_rtsp_init(const cam_rtsp_config_t *cfg);

/** Libera el pipeline de camara/encoder inicializado por cam_rtsp_init(). */
esp_err_t cam_rtsp_deinit(void);

/**
 * Arranca la tarea de captura+encoding y el servidor RTSP en el
 * puerto configurado. No bloquea: ambas tareas corren en background.
 */
esp_err_t cam_rtsp_start(void);

/** Arranca solamente captura + encoder, sin servidor RTSP local. */
esp_err_t cam_rtsp_start_capture(void);

/** Detiene captura, encoding y el servidor RTSP. */
esp_err_t cam_rtsp_stop(void);

/** Ancho/alto activos segun la resolucion configurada. */
void cam_rtsp_get_dimensions(uint16_t *width, uint16_t *height);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // EOF __CAMERA_RTSP_H__
