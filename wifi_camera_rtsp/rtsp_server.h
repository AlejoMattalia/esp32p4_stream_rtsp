/**
*******************************************************************************
* @file           : rtsp_server.h
* @brief          : Description of header file
* @author         : 
* @date           : dd/mm/aaaa
*******************************************************************************
* @attention
*
* Copyright (c) <date> grivera. All rights reserved.
*
*/
#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__
/******************************************************************************
        Includes
******************************************************************************/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include <stdbool.h>
/******************************************************************************
        Constants
******************************************************************************/
#define SPS_PPS_MAX_LEN				(128)
/******************************************************************************
        Data types
******************************************************************************/
// Un frame H.264 codificado (Annex-B: NALs separados por start codes),
// copiado a heap/PSRAM para poder desacoplar al encoder de la red.
typedef struct 
{
    uint8_t *data;
    size_t len;
    bool is_keyframe;
    uint32_t rtp_timestamp;   // en unidades de reloj RTP (90 kHz)
	
} encoded_frame_t;

// SPS/PPS cacheados del primer keyframe visto, necesarios para armar
// el SDP (a=fmtp: sprop-parameter-sets=...) en el DESCRIBE.
typedef struct 
{
    uint8_t sps[SPS_PPS_MAX_LEN];
    size_t sps_len;
    uint8_t pps[SPS_PPS_MAX_LEN];
    size_t pps_len;
    bool valid;
    SemaphoreHandle_t lock;
	
} sps_pps_cache_t;
/******************************************************************************
        Extern variables
******************************************************************************/
// Cola de frames codificados: la llena la tarea de captura/encoding,
// la consume el servidor RTSP mientras haya un cliente en PLAY.
// Si nadie la consume (sin cliente conectado) los frames viejos se
// descartan para no acumular memoria.
extern QueueHandle_t g_encoded_frame_queue;
extern sps_pps_cache_t g_sps_pps_cache;
// Ancho/alto/fps activos, usados por rtsp_server.c para el SDP y el
// paso del reloj RTP.
extern uint16_t g_cam_width;
extern uint16_t g_cam_height;
extern uint8_t  g_cam_fps;
/******************************************************************************
        Public function prototypes
******************************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rtsp_server_start(uint16_t port);
esp_err_t rtsp_server_stop(void);
void encoded_frame_free(encoded_frame_t *f);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // EOF __RTSP_SERVER_H__

