/*
 * Pipeline de video ESP32-P4 + OV5647 usando el framework oficial
 * esp_video (API compatible V4L2):
 *
 *   OV5647 (RAW8, MIPI-CSI) -> ISP -> /dev/video0  (YUV420, captura)
 *                                        |
 *                                        v  (memcpy a buffer OUTPUT)
 *                                  /dev/video11     (encoder H.264 HW, M2M)
 *                                        |
 *                                        v
 *                              Annex-B H.264 -> cola -> rtsp_server.c
 *
 * Simplificacion deliberada (pedido explicito de "no sobre-complejizar"):
 *  - Un solo hilo de captura+encode, sin pool generico multi-sensor.
 *  - Copia CSI->encoder por CPU en vez de DMA chaining, para mantener
 *    el codigo legible; a 640x480/720p y 20 fps el margen de CPU en el
 *    P4 sobra para esto en un firmware de validacion.
 */
 
/**
*******************************************************************************
* @file           : camera_rtsp.c
* @brief          : Description of C implementation module
* @author         : 
* @date           : dd/mm/aaaa
*******************************************************************************
* @attention
*
* Copyright (c) <date> grivera. All rights reserved.
*
*/
/******************************************************************************
    Includes
******************************************************************************/
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "camera_rtsp.h"
#include "esp_err.h"
#include "rtsp_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"

#include "esp_timer.h"

#include "esp_ldo_regulator.h"
/******************************************************************************
    Defines and constants
******************************************************************************/
#define CAP_BUF_COUNT   				(6)
#define ENC_OUT_COUNT   				(6)
#define ENC_CAP_COUNT					(6)

#define CAM_SCCB_I2C_PORT				(0)
#define CAM_SCCB_SCL_GPIO 				(8)
#define CAM_SCCB_SDA_GPIO 				(7)
#define CAM_SCCB_FREQ					(100000)

static const char *TAG = "[CAMERA_RTSP]";
/******************************************************************************
    Data types
******************************************************************************/
typedef struct 
{
    void   *start;
    size_t  length;
	
} mmap_buf_t;
/******************************************************************************
    Local variables
******************************************************************************/
static cam_rtsp_config_t s_cfg;
static TaskHandle_t s_capture_task_handle = NULL;
static volatile bool s_running = false;
static int s_video_fd = -1;   // /dev/video0  (CSI capture, salida YUV420 via ISP)
static int s_h264_fd  = -1;   // /dev/video11 (encoder H.264 M2M)
static mmap_buf_t s_cap_bufs[CAP_BUF_COUNT];
// static mmap_buf_t s_enc_out_bufs[ENC_OUT_COUNT];
static mmap_buf_t s_enc_cap_bufs[ENC_CAP_COUNT];
/******************************************************************************
    Local function prototypes
******************************************************************************/
static esp_err_t v4l2_set_fmt(int fd, uint32_t type, uint32_t w, uint32_t h, uint32_t pixfmt);
static void v4l2_try_set_fps(int fd, uint8_t fps);
static esp_err_t v4l2_reqbufs_mmap(int fd, uint32_t type, uint32_t count, mmap_buf_t *out_bufs);
static esp_err_t v4l2_streamon(int fd, uint32_t type);
static void cache_sps_pps_if_needed(const uint8_t *data, size_t len);
static bool h264_is_keyframe(const uint8_t *data, size_t len);
static void format_enumeration(void);
static void diagnostic_node_video(void);
static esp_err_t ldo_enable(void);
static void capture_task(void *arg);
/******************************************************************************
    Extern variables
******************************************************************************/
QueueHandle_t g_encoded_frame_queue = NULL;
sps_pps_cache_t g_sps_pps_cache = { .lock = NULL };
uint16_t g_cam_width = 640;
uint16_t g_cam_height = 480;
uint8_t  g_cam_fps = 20;
/******************************************************************************
    Local function definitions
******************************************************************************/
static esp_err_t v4l2_set_fmt(int fd, uint32_t type, uint32_t w, uint32_t h, uint32_t pixfmt)
{
    struct v4l2_format fmt = { 0 };
    fmt.type = type;
    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE || type == V4L2_BUF_TYPE_VIDEO_OUTPUT) 
	{
        fmt.fmt.pix.width       = w;
        fmt.fmt.pix.height      = h;
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    }
    
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) 
	{
        ESP_LOGE(TAG, "VIDIOC_S_FMT fallo en fd=%d (%s)", fd, strerror(errno));
        return ESP_FAIL;
    }
    
	return ESP_OK;
}

static void v4l2_try_set_fps(int fd, uint8_t fps)
{
    struct v4l2_streamparm parameter = {0};
    parameter.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameter.parm.capture.timeperframe.numerator = 1;
    parameter.parm.capture.timeperframe.denominator = fps;
    if (ioctl(fd, VIDIOC_S_PARM, &parameter) != 0) {
        ESP_LOGW(TAG, "El driver no permitio fijar %u FPS (%s)", fps, strerror(errno));
    } else {
        ESP_LOGI(TAG, "Captura configurada a %u FPS", fps);
    }
}

static esp_err_t v4l2_reqbufs_mmap(int fd, uint32_t type, uint32_t count, mmap_buf_t *out_bufs)
{
    struct v4l2_requestbuffers req = { 0 };
    req.count  = count;
    req.type   = type;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "VIDIOC_REQBUFS fallo (%s)", strerror(errno));

    for (uint32_t i = 0; i < count; i++) 
	{
        struct v4l2_buffer buf = { 0 };
        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "VIDIOC_QUERYBUF fallo (%s)", strerror(errno));

        out_bufs[i].length = buf.length;
        out_bufs[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(out_bufs[i].start != MAP_FAILED, ESP_FAIL, TAG, "mmap fallo");

        // Las colas de tipo OUTPUT (frames que nosotros alimentamos) no
        // se encolan de entrada; las de CAPTURE si, para que el driver
        // empiece a llenarlas.
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) 
		{
            ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "VIDIOC_QBUF inicial fallo (%s)", strerror(errno));
        }
    }
    return ESP_OK;
}

static esp_err_t v4l2_streamon(int fd, uint32_t type)
{
    ESP_RETURN_ON_FALSE(ioctl(fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "VIDIOC_STREAMON fallo tipo=%u (%s)", type, strerror(errno));
    return ESP_OK;
}

// ---------------------------------------------------------------------
// Extraccion de SPS/PPS desde un Annex-B stream (para el SDP del RTSP)
// ---------------------------------------------------------------------
static void cache_sps_pps_if_needed(const uint8_t *data, size_t len)
{
    if (g_sps_pps_cache.valid) 
	{
        return;
    }
    
	xSemaphoreTake(g_sps_pps_cache.lock, portMAX_DELAY);
    size_t i = 0;
    bool got_sps = false, got_pps = false;
    
	while (i + 4 < len) 
	{
        // Buscar start code de 3 o 4 bytes
        size_t sc_len = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) 
		{
            sc_len = 3;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) 
		{
            sc_len = 4;
        }
        
		if (sc_len == 0) 
		{ 
			i++; continue; 
		}

        size_t nal_start = i + sc_len;
        size_t nal_end = nal_start;
        while (nal_end + 3 < len && !(data[nal_end] == 0 && data[nal_end + 1] == 0 && (data[nal_end + 2] == 1 || (data[nal_end + 2] == 0 && data[nal_end + 3] == 1)))) 
		{
            nal_end++;
        }
        
		if (nal_end + 3 >= len) 
		{
            nal_end = len;
        }

        uint8_t nal_type = data[nal_start] & 0x1F;
        size_t nal_len = nal_end - nal_start;

        if (nal_type == 7 && nal_len <= SPS_PPS_MAX_LEN) 
		{   // SPS
            memcpy(g_sps_pps_cache.sps, &data[nal_start], nal_len);
            g_sps_pps_cache.sps_len = nal_len;
            got_sps = true;
        } else if (nal_type == 8 && nal_len <= SPS_PPS_MAX_LEN) 
		{   // PPS
            memcpy(g_sps_pps_cache.pps, &data[nal_start], nal_len);
            g_sps_pps_cache.pps_len = nal_len;
            got_pps = true;
        }
        i = nal_end;
    }
	
    if (got_sps && got_pps) 
	{
        g_sps_pps_cache.valid = true;
        ESP_LOGI(TAG, "SPS/PPS cacheados (sps=%d pps=%d bytes)", (int)g_sps_pps_cache.sps_len, (int)g_sps_pps_cache.pps_len);
    }
	
    xSemaphoreGive(g_sps_pps_cache.lock);
}

static bool h264_is_keyframe(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i + 4 < len; i++) {
        size_t start_code_len = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_code_len = 3;
        } else if (data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            start_code_len = 4;
        }
        if (start_code_len && i + start_code_len < len &&
            (data[i + start_code_len] & 0x1F) == 5) {
            return true;
        }
    }
    return false;
}

static void format_enumeration(void)
{
	ESP_LOGI(TAG, "================ ENUMERANDO FORMATOS/RESOLUCIONES DE /dev/video0 ================");
	struct v4l2_fmtdesc fmtdesc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = 0 };
	while (ioctl(s_video_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) 
	{
		char fourcc[5] = 
		{
			(char)(fmtdesc.pixelformat & 0xFF),
	     	(char)((fmtdesc.pixelformat >> 8) & 0xFF),
	     	(char)((fmtdesc.pixelformat >> 16) & 0xFF),
	     	(char)((fmtdesc.pixelformat >> 24) & 0xFF), '\0'
	 	};
	 
	 ESP_LOGI(TAG, "[Format %d] FourCC: '%s' (0x%" PRIx32 ") - %s", fmtdesc.index, fourcc, fmtdesc.pixelformat, fmtdesc.description);

	 struct v4l2_frmsizeenum frmsize = 
	 { 
		.pixel_format = fmtdesc.pixelformat, 
		.index = 0 
	 };
	 
	 while (ioctl(s_video_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) 
	 {
	     if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) 
		 {
			ESP_LOGI(TAG, "   -> Res soportada: %" PRIu32 "x%" PRIu32, frmsize.discrete.width, frmsize.discrete.height);
	     }
		 
	     frmsize.index++;
	 }
	 
	 fmtdesc.index++;
	}
	
	ESP_LOGI(TAG, "==================================================================================");						 
	vTaskDelay(pdMS_TO_TICKS(1000));
}

static void diagnostic_node_video(void)
{
	// Prueba diagnóstica de nodos de video
	int test_fd = -1;
	for (int i = 0; i < 15; i++) 
	{
	    char dev_name[16];
	    snprintf(dev_name, sizeof(dev_name), "/dev/video%d", i);
	    test_fd = open(dev_name, O_RDWR);
	    if (test_fd >= 0) {
	        ESP_LOGI(TAG, "Nodo de video encontrado: %s", dev_name);
	        close(test_fd);
	    }
	}		
}

static esp_err_t ldo_enable(void)
{
	esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
	esp_ldo_channel_config_t ldo_mipi_phy_config = 
	{
	    .chan_id = 3,          // Canal LDO por defecto para MIPI en ESP32-P4
	    .voltage_mv = 2500,     // 2.5V requeridos por el PHY MIPI
	};
	
	esp_err_t err = esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
	
	if (err != ESP_OK) 
	{
	    ESP_LOGE("CAM", "Error al activar LDO MIPI: %s", esp_err_to_name(err));
	}
	
	return err;	
}

static void capture_task(void *arg)
{

	uint32_t rtp_ts = 0;
    int64_t t_stream_start_us = esp_timer_get_time();
	int64_t next_encode_us = t_stream_start_us;
	
	// Contadores para diagnosticar el pipeline
	uint32_t cam_dqbuf_ok = 0, cam_dqbuf_fail = 0;
	uint32_t enc_qbuf_ok = 0, enc_qbuf_fail = 0;
	uint32_t enc_dqbuf_ok = 0, enc_dqbuf_fail = 0;
	uint32_t queue_ok = 0, queue_fail = 0;
	int64_t last_diag_time = esp_timer_get_time();
	
	// static uint32_t s_enc_out_idx = 0;
    ESP_LOGI(TAG, "Tarea de captura/encoding iniciada (%ux%u @ %u fps, %lu bps)", g_cam_width, g_cam_height, g_cam_fps, (unsigned long)s_cfg.bitrate_bps);

    while (s_running) 
	{
        // 1) Tomar un frame YUV420 de la camara (CSI+ISP)
        struct v4l2_buffer cap_buf = { 0 };
        cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cap_buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &cap_buf) != 0) 
		{
            cam_dqbuf_fail++;
			// vTaskDelay(pdMS_TO_TICKS(10));
			continue;
        }
		cam_dqbuf_ok++;

        /*
         * Algunos drivers ignoran VIDIOC_S_PARM y entregan unos 34 FPS aunque
         * se hayan solicitado 20. Limitamos antes del encoder para no producir
         * mas frames de los que la publicacion necesita.
         */
        int64_t capture_now_us = esp_timer_get_time();
        int64_t frame_period_us = 1000000LL / (g_cam_fps ? g_cam_fps : 20);
        if (capture_now_us < next_encode_us) {
            ioctl(s_video_fd, VIDIOC_QBUF, &cap_buf);
            continue;
        }
        next_encode_us = capture_now_us + frame_period_us;

		// 2) Pasar el puntero de la camara DIRECTAMENTE al encoder (Zero-Copy V4L2)
        struct v4l2_buffer enc_out = { 0 };
        enc_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        enc_out.memory = V4L2_MEMORY_USERPTR;
        enc_out.index = cap_buf.index;
        enc_out.m.userptr = (unsigned long)s_cap_bufs[cap_buf.index].start; // Dirección en PSRAM
        enc_out.length = s_cap_bufs[cap_buf.index].length;
        enc_out.bytesused = cap_buf.bytesused;

        if (ioctl(s_h264_fd, VIDIOC_QBUF, &enc_out) != 0) 
        {
            enc_qbuf_fail++;
        } else {
			enc_qbuf_ok++;
		}

        // 3) Sacar el frame ya codificado en H.264
        struct v4l2_buffer enc_cap = { 0 };
        enc_cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        enc_cap.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(s_h264_fd, VIDIOC_DQBUF, &enc_cap) == 0) 
        {
			enc_dqbuf_ok++;
            uint8_t *src = (uint8_t *)s_enc_cap_bufs[enc_cap.index].start;
            size_t len = enc_cap.bytesused;
            cache_sps_pps_if_needed(src, len);
            bool is_keyframe = h264_is_keyframe(src, len);

            encoded_frame_t *frame = heap_caps_malloc(sizeof(encoded_frame_t), MALLOC_CAP_DEFAULT);
            if (frame) 
            {
                frame->data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!frame->data) 
                {
                    frame->data = heap_caps_malloc(len, MALLOC_CAP_DEFAULT);
                }
                
                if (frame->data) 
                {
                    memcpy(frame->data, src, len);
                    frame->len = len;
                    frame->is_keyframe = is_keyframe;
                    frame->rtp_timestamp = rtp_ts;

                    if (xQueueSend(g_encoded_frame_queue, &frame, 0) != pdTRUE) 
                    {
                        /*
                         * Mantener video reciente: reemplazar solamente el
                         * frame mas antiguo. Vaciar toda la cola y esperar al
                         * siguiente IDR convertia cualquier pico en ~1 FPS.
                         */
                        encoded_frame_t *old = NULL;
                        if (xQueueReceive(g_encoded_frame_queue, &old, 0) == pdTRUE) {
                            encoded_frame_free(old);
                        }
                        if (xQueueSend(g_encoded_frame_queue, &frame, 0) != pdTRUE) {
                            encoded_frame_free(frame);
                        }
                    } else {
						queue_ok++;
					}
                } else 
                {
                    heap_caps_free(frame);
                }
            }
            
            ioctl(s_h264_fd, VIDIOC_QBUF, &enc_cap);
			
        } else 
        {
            enc_dqbuf_fail++;
        }

        // 4) Reclamar el buffer USERPTR consumido por el encoder
        struct v4l2_buffer enc_out_done = { 0 };
        enc_out_done.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        enc_out_done.memory = V4L2_MEMORY_USERPTR;
        if (ioctl(s_h264_fd, VIDIOC_DQBUF, &enc_out_done) != 0) 
        {
            // Silenciamos estos warnings pues son muy frecuentes
        }

        // 5) Devolver la memoria original a la camara para volver a capturar
        ioctl(s_video_fd, VIDIOC_QBUF, &cap_buf);
		
		// Timestamp RTP basado en reloj interno
        int64_t elapsed_us = esp_timer_get_time() - t_stream_start_us;
        rtp_ts = (uint32_t)((elapsed_us * 90000) / 1000000);
		
		// Log de diagnóstico cada 5 segundos
		int64_t now = esp_timer_get_time();
		if (now - last_diag_time >= 5000000) {
			ESP_LOGI(TAG, "DIAGNOSTICO PIPELINE: cam_ok=%u cam_fail=%u | enc_qbuf_ok=%u enc_qbuf_fail=%u | enc_dqbuf_ok=%u enc_dqbuf_fail=%u | queue_ok=%u queue_fail=%u",
					cam_dqbuf_ok, cam_dqbuf_fail,
					enc_qbuf_ok, enc_qbuf_fail,
					enc_dqbuf_ok, enc_dqbuf_fail,
					queue_ok, queue_fail);
			cam_dqbuf_ok = cam_dqbuf_fail = 0;
			enc_qbuf_ok = enc_qbuf_fail = 0;
			enc_dqbuf_ok = enc_dqbuf_fail = 0;
			queue_ok = queue_fail = 0;
			last_diag_time = now;
		}
    }

    vTaskDelete(NULL);
}

/******************************************************************************
    Public function definitions
******************************************************************************/
void encoded_frame_free(encoded_frame_t *f)
{
    if (!f) return;
    if (f->data) heap_caps_free(f->data);
    heap_caps_free(f);
}

void cam_rtsp_config_default(cam_rtsp_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
	
#if CONFIG_EXAMPLE_CAM_RES_720P
    cfg->resolution = CAM_RTSP_RES_720P;
#else
    cfg->resolution = CAM_RTSP_RES_VGA;
#endif
    cfg->fps         = CONFIG_EXAMPLE_CAM_FPS;
    cfg->bitrate_bps = CONFIG_EXAMPLE_CAM_BITRATE_KBPS * 1000;
    cfg->rtsp_port   = CONFIG_EXAMPLE_RTSP_PORT;
}

esp_err_t cam_rtsp_init(const cam_rtsp_config_t *cfg)
{
    s_cfg = *cfg;

//    if (cfg->resolution == CAM_RTSP_RES_720P) {
//        g_cam_width  = 1280;
//        g_cam_height = 720;
//    } else 
//	{
//        g_cam_width  = 640;
//        g_cam_height = 480;
//    }
	
	g_cam_width  = 800;
	g_cam_height = 800;	
	g_cam_fps = cfg->fps ? cfg->fps : 20;
    g_sps_pps_cache.lock = xSemaphoreCreateMutex();
    g_sps_pps_cache.valid = false;
	/* Absorbe rafagas breves sin acumular latencia visible. A 20 FPS
	 * representa como maximo unos 150 ms de cola. */
	g_encoded_frame_queue = xQueueCreate(3, sizeof(encoded_frame_t *));
	ESP_RETURN_ON_FALSE(g_encoded_frame_queue && g_sps_pps_cache.lock, ESP_ERR_NO_MEM, TAG, "no se pudieron crear cola/mutex");

    esp_video_init_csi_config_t csi_cfg = 
	{
        .sccb_config = 
		{
            .init_sccb   = true,
            .i2c_config = {
                .port      = CAM_SCCB_I2C_PORT,
                .scl_pin   = CAM_SCCB_SCL_GPIO,
                .sda_pin   = CAM_SCCB_SDA_GPIO,
            },
            .freq        = CAM_SCCB_FREQ,
        },
        .reset_pin = -1,
        .pwdn_pin  = -1,
    };
	
    esp_video_init_config_t video_cfg = 
	{
        .csi      = &csi_cfg,
        .dvp      = NULL,
    };
	
	if (ldo_enable() != ESP_OK)
	{
		return ESP_FAIL;
	}
	
    ESP_RETURN_ON_ERROR(esp_video_init(&video_cfg), TAG, "esp_video_init fallo");

    // --- /dev/video0: salida ISP en YUV420 al tamano elegido ---
    s_video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    ESP_RETURN_ON_FALSE(s_video_fd >= 0, ESP_FAIL, TAG, "no se pudo abrir %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

	format_enumeration();
							 
	ESP_RETURN_ON_ERROR(v4l2_set_fmt(s_video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, g_cam_width, g_cam_height, V4L2_PIX_FMT_YUV420), TAG, "set fmt camara fallo");
    v4l2_try_set_fps(s_video_fd, g_cam_fps);
    ESP_RETURN_ON_ERROR(v4l2_reqbufs_mmap(s_video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, CAP_BUF_COUNT, s_cap_bufs), TAG, "reqbufs camara fallo");

	diagnostic_node_video();
		
    // --- /dev/video11: encoder H.264 por hardware (M2M) ---
    s_h264_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDWR);
    ESP_RETURN_ON_FALSE(s_h264_fd >= 0, ESP_FAIL, TAG, "no se pudo abrir %s", ESP_VIDEO_H264_DEVICE_NAME);
    ESP_RETURN_ON_ERROR(v4l2_set_fmt(s_h264_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, g_cam_width, g_cam_height, V4L2_PIX_FMT_YUV420), TAG, "set fmt encoder OUTPUT fallo");
    ESP_RETURN_ON_ERROR(v4l2_set_fmt(s_h264_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, g_cam_width, g_cam_height, V4L2_PIX_FMT_H264), TAG, "set fmt encoder CAPTURE fallo");

    // Bitrate / GOP a traves de controles extendidos V4L2 estandar
    {
        struct v4l2_ext_control ctrl[2] = { 0 };
        struct v4l2_ext_controls ctrls = { 0 };
        ctrl[0].id    = V4L2_CID_MPEG_VIDEO_BITRATE;
        ctrl[0].value = s_cfg.bitrate_bps;
        ctrl[1].id    = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;	// V4L2_CID_MPEG_VIDEO_GOP_SIZE;
        ctrl[1].value = 1; // CRUCIAL: cada frame es keyframe para asegurar output continuo @ 20 FPS
        ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
        ctrls.count      = 2;
        ctrls.controls   = ctrl;
        if (ioctl(s_h264_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) 
		{
            ESP_LOGW(TAG, "No se pudieron fijar bitrate/GOP (control no soportado en esta version del componente), se usan valores por defecto del encoder");
        }
    }

	// 1) Solicitar buffers USERPTR para la entrada del encoder (Zero-Copy desde la cámara)
    struct v4l2_requestbuffers req_enc_out = 
	{
        .count  = CAP_BUF_COUNT, // Debe coincidir con la cantidad de buffers de la cámara
        .type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    
	ESP_RETURN_ON_FALSE(ioctl(s_h264_fd, VIDIOC_REQBUFS, &req_enc_out) == 0, ESP_FAIL, TAG, "reqbufs encoder OUTPUT USERPTR fallo (%s)", strerror(errno));

    // 2) Mapear buffers MMAP para la salida codificada H.264
    ESP_RETURN_ON_ERROR(v4l2_reqbufs_mmap(s_h264_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, ENC_CAP_COUNT, s_enc_cap_bufs), TAG, "reqbufs encoder CAPTURE fallo");
    ESP_RETURN_ON_ERROR(v4l2_streamon(s_video_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE), TAG, "streamon camara fallo");
    ESP_RETURN_ON_ERROR(v4l2_streamon(s_h264_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT), TAG, "streamon encoder OUTPUT fallo");
    ESP_RETURN_ON_ERROR(v4l2_streamon(s_h264_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE), TAG, "streamon encoder CAPTURE fallo");
    ESP_LOGI(TAG, "Pipeline de camara inicializado: %ux%u @ %u fps", g_cam_width, g_cam_height, g_cam_fps);
	
    return ESP_OK;
}

esp_err_t cam_rtsp_deinit(void)
{
    if (s_h264_fd >= 0)  close(s_h264_fd);
    if (s_video_fd >= 0) close(s_video_fd);
    s_h264_fd = s_video_fd = -1;
    return ESP_OK;
}

esp_err_t cam_rtsp_start(void)
{
    ESP_RETURN_ON_ERROR(cam_rtsp_start_capture(), TAG, "no se pudo iniciar captura");
    return rtsp_server_start(s_cfg.rtsp_port);
}

esp_err_t cam_rtsp_start_capture(void)
{
    if (s_running) return ESP_OK;
    s_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "cam_capture", 6144, NULL, 10, &s_capture_task_handle, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "no se pudo crear la tarea de captura");

    return ESP_OK;
}

esp_err_t cam_rtsp_stop(void)
{
    s_running = false;
    rtsp_server_stop();
    return ESP_OK;
}

void cam_rtsp_get_dimensions(uint16_t *width, uint16_t *height)
{
    *width  = g_cam_width;
    *height = g_cam_height;
}
