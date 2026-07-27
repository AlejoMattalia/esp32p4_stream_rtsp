/*
 * Servidor RTSP minimo para validacion con VLC.
 *
 * Decisiones de diseno (a proposito simples):
 *  - Un solo cliente a la vez (alcanza y sobra para validar el link).
 *  - Transporte "RTP/AVP/TCP;interleaved=0-1": los paquetes RTP viajan
 *    dentro del mismo socket TCP de control, con el framing estandar
 *    '$' + canal(1B) + longitud(2B). Esto evita todo el tema de NAT /
 *    puertos UDP abiertos y es exactamente lo que VLC negocia cuando
 *    se le pide explicitamente transporte TCP (o cuando UDP falla y
 *    hace fallback automatico).
 *  - No implementa autenticacion, multiples tracks de audio, ni
 *    RTCP: es un servidor de validacion, no un servidor de produccion.
 */

/**
*******************************************************************************
* @file           : template.c
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
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <unistd.h>
#include "rtsp_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <sys/socket.h>
/******************************************************************************
    Defines and constants
******************************************************************************/
#define RTP_PAYLOAD_TYPE				(96)
#define RTP_MAX_PAYLOAD    				(1200)   // deja margen para header RTP + framing TCP dentro de una trama Ethernet
#define RTSP_RECV_BUF      				(1024)
#define RTSP_SESSION_ID    				("F4A5C001")

static const char *TAG = "[RTSP_SERV]";
/******************************************************************************
    Data types
******************************************************************************/

/******************************************************************************
    Local variables
******************************************************************************/
static int  s_listen_fd = -1;
static TaskHandle_t s_server_task = NULL;
static volatile bool s_server_running = false;
// ------------------------------------------------------------------
// Empaquetado H.264 -> RTP (RFC 6184): NAL unico o fragmentacion FU-A
// ------------------------------------------------------------------
static uint16_t s_seq = 0;
static const uint32_t SSRC = 0x1A2B3C4D;
/******************************************************************************
    Local function prototypes
******************************************************************************/
// Sustituto transparente de writev() para sockets LwIP:
static inline ssize_t lwip_writev_socket(int sock, const struct iovec *iov, int iovcnt);
static int send_rtp_packet_zerocopy(int sock, const uint8_t *payload, size_t payload_len, uint32_t ts, bool marker);
static int rtp_send_nal_zerocopy(int sock, const uint8_t *nal, size_t nal_len, uint32_t ts, bool last_nal_in_frame);
// Recorre un buffer Annex-B y manda cada NAL como uno o mas paquetes RTP:
static int rtp_send_annexb_frame(int sock, const uint8_t *data, size_t len, uint32_t ts);
// Construccion del SDP para DESCRIBE:
static int build_sdp(char *out, size_t out_len, const char *server_ip);
// Manejo de una sesion de cliente (bloqueante, un cliente a la vez)
static void get_cseq(const char *req, char *out, size_t out_len);
static void handle_client(int sock);
static void server_task(void *arg);
/******************************************************************************
    Local function definitions
******************************************************************************/
static inline ssize_t lwip_writev_socket(int sock, const struct iovec *iov, int iovcnt)
{
    struct msghdr msg = 
	{
        .msg_iov = (struct iovec *)iov,
        .msg_iovlen = iovcnt,
    };
    
	return sendmsg(sock, &msg, 0);
}

static int send_rtp_packet_zerocopy(int sock, const uint8_t *payload, size_t payload_len, uint32_t ts, bool marker)
{
    // Solo construimos el encabezado (Framing Interleaved 4B + RTP Header 12B = 16B) en Stack
    uint8_t hdr[16];
    uint16_t rtp_len = 12 + payload_len;

    // Framing Interleaved ($)
    hdr[0] = '$';
    hdr[1] = 0; // Canal RTP
    hdr[2] = (rtp_len >> 8) & 0xFF;
    hdr[3] = rtp_len & 0xFF;

    // Cabecera RTP
    hdr[4] = 0x80;
    hdr[5] = (marker ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
    hdr[6] = (s_seq >> 8) & 0xFF;
    hdr[7] = s_seq & 0xFF;
    s_seq++;
    hdr[8]  = (ts >> 24) & 0xFF; hdr[9]  = (ts >> 16) & 0xFF;
    hdr[10] = (ts >> 8) & 0xFF;  hdr[11] = ts & 0xFF;
    hdr[12] = (SSRC >> 24) & 0xFF; hdr[13] = (SSRC >> 16) & 0xFF;
    hdr[14] = (SSRC >> 8) & 0xFF;  hdr[15] = SSRC & 0xFF;

    // Vector I/O: Apuntamos al header local y directamente a la PSRAM del frame codificado
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 16;
    iov[1].iov_base = (void *)payload; // Buffer en PSRAM original (Zero-Copy)
    iov[1].iov_len  = payload_len;

    // Una sola operacion de envio sin memcpy intermedio
    ssize_t total = 16 + payload_len;
    ssize_t sent = lwip_writev_socket(sock, iov, 2);

    return (sent == total) ? 0 : -1;
}

static int rtp_send_nal_zerocopy(int sock, const uint8_t *nal, size_t nal_len, uint32_t ts, bool last_nal_in_frame)
{
    if (nal_len == 0) 
	{
		return 0;
	}

    if (nal_len <= RTP_MAX_PAYLOAD) 
	{
        return send_rtp_packet_zerocopy(sock, nal, nal_len, ts, last_nal_in_frame);
    }

    // Fragmentacion FU-A sin copiar el payload
    uint8_t nal_header = nal[0];
    uint8_t nal_type   = nal_header & 0x1F;
    uint8_t nri        = nal_header & 0x60;
    const uint8_t *p   = nal + 1;
    size_t remaining   = nal_len - 1;
    bool first         = true;

    while (remaining > 0) 
	{
        size_t chunk = remaining;
        if (chunk > RTP_MAX_PAYLOAD - 2) chunk = RTP_MAX_PAYLOAD - 2;
        bool is_last_frag = (remaining - chunk) == 0;

        uint8_t hdr[18]; // 16 bytes (Interleaved+RTP) + 2 bytes (FU-A)
        uint16_t rtp_len = 12 + 2 + chunk;

        // Header Interleaved + RTP
        hdr[0] = '$'; hdr[1] = 0;
        hdr[2] = (rtp_len >> 8) & 0xFF; hdr[3] = rtp_len & 0xFF;
        hdr[4] = 0x80;
        hdr[5] = ((is_last_frag && last_nal_in_frame) ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
        hdr[6] = (s_seq >> 8) & 0xFF; hdr[7] = s_seq & 0xFF; s_seq++;
        hdr[8]  = (ts >> 24) & 0xFF; hdr[9]  = (ts >> 16) & 0xFF;
        hdr[10] = (ts >> 8) & 0xFF;  hdr[11] = ts & 0xFF;
        hdr[12] = (SSRC >> 24) & 0xFF; hdr[13] = (SSRC >> 16) & 0xFF;
        hdr[14] = (SSRC >> 8) & 0xFF;  hdr[15] = SSRC & 0xFF;

        // FU-A Indicator + Header
        hdr[16] = 0x1C | nri;
        hdr[17] = (first ? 0x80 : 0x00) | (is_last_frag ? 0x40 : 0x00) | nal_type;

        struct iovec iov[2];
        iov[0].iov_base = hdr;
        iov[0].iov_len  = 18;
        iov[1].iov_base = (void *)p; // Apunta directo a la porcion del buffer NAL
        iov[1].iov_len  = chunk;

        if (lwip_writev_socket(sock, iov, 2) <= 0) 
		{
			return -1;
		}

        p += chunk;
        remaining -= chunk;
        first = false;
    }
	
    return 0;
}

static int rtp_send_annexb_frame(int sock, const uint8_t *data, size_t len, uint32_t ts)
{
    size_t i = 0;
    // localizar todos los NALs primero para saber cual es el ultimo (?) ubicacion
    typedef struct 
	{ 
		size_t start, len; 
		
	} nal_range_t;
    
	nal_range_t nals[16];
    int n_nals = 0;

    while (i + 3 < len && n_nals < 16) 
	{
        size_t sc_len = 0;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) 
		{
			sc_len = 3;
		}
        else if (i + 4 < len && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1)
		{
			sc_len = 4;
		}
        
		if (sc_len == 0)
		{ 
			i++; continue; 
		}

        size_t start = i + sc_len;
        size_t end = start;
        while (end + 3 < len && !(data[end] == 0 && data[end+1] == 0 && (data[end+2] == 1 || (data[end+2]==0 && data[end+3]==1)))) 
		{
            end++;
        }
        
		if (end + 3 >= len) 
		{
			end = len;
		}

        nals[n_nals].start = start;
        nals[n_nals].len   = end - start;
        n_nals++;
        i = end;
    }

    for (int k = 0; k < n_nals; k++) 
	{
        bool last = (k == n_nals - 1);
		if (rtp_send_nal_zerocopy(sock, &data[nals[k].start], nals[k].len, ts, last) != 0)  
		{
            return -1;
        }
    }
	
    return 0;
}

static int build_sdp(char *out, size_t out_len, const char *server_ip)
{
    char sps_b64[128] = {0}, pps_b64[128] = {0};
    size_t sps_b64_len = 0, pps_b64_len = 0;

    xSemaphoreTake(g_sps_pps_cache.lock, portMAX_DELAY);
    mbedtls_base64_encode((unsigned char *)sps_b64, sizeof(sps_b64), &sps_b64_len, g_sps_pps_cache.sps, g_sps_pps_cache.sps_len);
    mbedtls_base64_encode((unsigned char *)pps_b64, sizeof(pps_b64), &pps_b64_len, g_sps_pps_cache.pps, g_sps_pps_cache.pps_len);
    uint8_t profile_level_id[3];
    memcpy(profile_level_id, &g_sps_pps_cache.sps[1], 3); // bytes 1..3 del SPS = profile-level-id
    xSemaphoreGive(g_sps_pps_cache.lock);

    char plid_hex[7];
    snprintf(plid_hex, sizeof(plid_hex), "%02X%02X%02X", profile_level_id[0], profile_level_id[1], profile_level_id[2]);

    return snprintf(out, out_len,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=ESP32-P4 OV5647 Live\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "a=tool:esp32p4-rtsp-validation\r\n"
        "m=video 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=fmtp:%d packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,%s\r\n"
        "a=control:track1\r\n",
        server_ip, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE,
        plid_hex, sps_b64, pps_b64);
}

static void get_cseq(const char *req, char *out, size_t out_len)
{
    const char *p = strstr(req, "CSeq:");
    out[0] = '\0';
    if (!p) return;
    p += 5;
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
}

static void handle_client(int sock)
{
    char req[RTSP_RECV_BUF];
    char resp[1600];
    char cseq[32];
    bool playing = false;
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(sock, (struct sockaddr *)&local_addr, &addr_len);
    char server_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, server_ip, sizeof(server_ip));

	// 1. Desactivar algoritmo de Nagle (latencia minima)
	int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	
	// 2. Aumentar el buffer de salida del socket (evita bloqueos en I-Frames/Keyframes)
	int sndbuf_size = 32 * 1024; // 32 KB
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));
    
	s_seq = 0;

    while (s_server_running) 
	{
        int n = recv(sock, req, sizeof(req) - 1, 0);
        if (n <= 0) break;
        req[n] = '\0';

        get_cseq(req, cseq, sizeof(cseq));

        if (!strncmp(req, "OPTIONS", 7)) 
		{
            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n", cseq);
            send(sock, resp, strlen(resp), 0);

        } else if (!strncmp(req, "DESCRIBE", 8)) 
		{
            // Esperar a tener SPS/PPS (primer keyframe) antes de describir el stream
            int wait_ms = 0;
            while (!g_sps_pps_cache.valid && wait_ms < 4000) 
			{
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_ms += 100;
            }
            
			if (!g_sps_pps_cache.valid) 
			{
                snprintf(resp, sizeof(resp), "RTSP/1.0 503 Service Unavailable\r\nCSeq: %s\r\n\r\n", cseq);
                send(sock, resp, strlen(resp), 0);
                continue;
            }
            char sdp[600];
            int sdp_len = build_sdp(sdp, sizeof(sdp), server_ip);
            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                "Content-Base: rtsp://%s/stream/\r\n"
                "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
                cseq, server_ip, sdp_len, sdp);
            send(sock, resp, strlen(resp), 0);

        } else if (!strncmp(req, "SETUP", 5)) 
		{
            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                "Session: %s\r\n\r\n", cseq, RTSP_SESSION_ID);
            send(sock, resp, strlen(resp), 0);

        } else if (!strncmp(req, "PLAY", 4)) 
		{
            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %s\r\n"
                "RTP-Info: url=rtsp://%s/stream/track1;seq=0\r\n\r\n",
                cseq, RTSP_SESSION_ID, server_ip);
            send(sock, resp, strlen(resp), 0);
            playing = true;

            // vaciar frames viejos acumulados antes de arrancar
            encoded_frame_t *stale;
            while (xQueueReceive(g_encoded_frame_queue, &stale, 0) == pdTRUE) 
			{
                encoded_frame_free(stale);
            }

            // Bucle de streaming: mientras el cliente siga en PLAY,
            // sacamos frames de la cola y los mandamos como RTP/TCP.
            while (s_server_running && playing) 
			{
                encoded_frame_t *frame = NULL;
                if (xQueueReceive(g_encoded_frame_queue, &frame, pdMS_TO_TICKS(500)) == pdTRUE) 
				{
                    if (rtp_send_annexb_frame(sock, frame->data, frame->len, frame->rtp_timestamp) != 0) 
					{
                        encoded_frame_free(frame);
                        playing = false;
                        break;
                    }
                    encoded_frame_free(frame);
                }
                // chequeo no bloqueante de TEARDOWN/GET_PARAMETER entrante
                fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
                struct timeval tv = {0, 0};
                if (select(sock + 1, &rfds, NULL, NULL, &tv) > 0) 
				{
                    int n2 = recv(sock, req, sizeof(req) - 1, MSG_DONTWAIT);
                    if (n2 <= 0) 
					{ 
						playing = false; 
						break; 
					}
                    
					req[n2] = '\0';
                    get_cseq(req, cseq, sizeof(cseq));
                    
					if (!strncmp(req, "TEARDOWN", 8)) 
					{
                        snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n", cseq);
                        send(sock, resp, strlen(resp), 0);
                        playing = false;
                    } else 
					{
                        // GET_PARAMETER u otro keep-alive: responder 200 OK
                        snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n", cseq);
                        send(sock, resp, strlen(resp), 0);
                    }
                }
            }

        } else if (!strncmp(req, "TEARDOWN", 8)) 
		{
            snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n", cseq);
            send(sock, resp, strlen(resp), 0);
            break;
        } else 
		{
            snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n", cseq);
            send(sock, resp, strlen(resp), 0);
        }
    }
}

static void server_task(void *arg)
{
    while (s_server_running) 
	{
        struct sockaddr_in client_addr;
        socklen_t alen = sizeof(client_addr);
        int client = accept(s_listen_fd, (struct sockaddr *)&client_addr, &alen);
        
		if (client < 0) 
		{
            if (s_server_running) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
		ESP_LOGI(TAG, "Cliente RTSP conectado: %s", inet_ntoa(client_addr.sin_addr));
        handle_client(client);
        ESP_LOGI(TAG, "Cliente RTSP desconectado");
        close(client);
    }
	
    vTaskDelete(NULL);
}
/******************************************************************************
    Public function definitions
******************************************************************************/
esp_err_t rtsp_server_start(uint16_t port)
{
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) 
	{
		return ESP_FAIL;
	}

    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) 
	{
        ESP_LOGE(TAG, "bind() al puerto %u fallo (%s)", port, strerror(errno));
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }
    
	listen(s_listen_fd, 1);

    s_server_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(server_task, "rtsp_server", 16384, NULL, 8, &s_server_task, 0);
    if (ok != pdPASS) 
	{
		return ESP_ERR_NO_MEM;
	}

    ESP_LOGI(TAG, "Servidor RTSP escuchando en puerto %u", port);
    
	return ESP_OK;
}

esp_err_t rtsp_server_stop(void)
{
    s_server_running = false;
    
	if (s_listen_fd >= 0) 
	{
        shutdown(s_listen_fd, SHUT_RDWR);
        close(s_listen_fd);
        s_listen_fd = -1;
    }
	
    return ESP_OK;
}
