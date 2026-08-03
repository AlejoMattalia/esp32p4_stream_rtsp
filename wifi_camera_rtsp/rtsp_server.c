/**
*******************************************************************************
* @file           : rtsp_server.c
* @brief          : Servidor RTSP (control TCP + datos RTP/UDP)
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
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
/******************************************************************************
    Defines and constants
******************************************************************************/
#define RTP_PAYLOAD_TYPE				(96)
#define RTP_MAX_PAYLOAD    				(1400)   // sin framing TCP: header IP+UDP+RTP (20+8+12=40B) deja margen de sobra en MTU 1500
#define RTSP_RECV_BUF      				(1024)
#define RTSP_SESSION_ID    				("F4A5C001")
#define SERVER_RTP_PORT					(6970)   // puerto UDP fijo del plano de datos
#define RTP_INTER_PACKET_DELAY_US		(1200)   // pacing entre paquetes de una misma rafaga (ver nota mas abajo)

static const char *TAG = "[RTSP_SERV]";
/******************************************************************************
    Data types
******************************************************************************/

/******************************************************************************
    Local variables
******************************************************************************/
static int  s_listen_fd  = -1;   // socket TCP de control (RTSP)
static int  s_rtp_udp_fd = -1;   // socket UDP de datos (RTP)
static TaskHandle_t s_server_task = NULL;
static volatile bool s_server_running = false;

// Direccion del cliente para el plano de datos, fijada en SETUP.
// sin_port == 0 significa "todavia no hay cliente configurado".
static struct sockaddr_in s_client_rtp_addr = { 0 };

// ------------------------------------------------------------------
// Empaquetado H.264 -> RTP (RFC 6184): NAL unico o fragmentacion FU-A
// ------------------------------------------------------------------
static uint16_t s_seq = 0;
static const uint32_t SSRC = 0x1A2B3C4D;
/******************************************************************************
    Local function prototypes
******************************************************************************/
static int send_rtp_packet_udp(const uint8_t *payload, size_t payload_len, uint32_t ts, bool marker);
static int rtp_send_nal_udp(const uint8_t *nal, size_t nal_len, uint32_t ts, bool last_nal_in_frame);
// Recorre un buffer Annex-B y manda cada NAL como uno o mas paquetes RTP:
static int rtp_send_annexb_frame(const uint8_t *data, size_t len, uint32_t ts);
// Construccion del SDP para DESCRIBE:
static int build_sdp(char *out, size_t out_len, const char *server_ip);
// Manejo de una sesion de cliente (bloqueante, un cliente a la vez)
static void get_cseq(const char *req, char *out, size_t out_len);
static bool parse_client_port(const char *req, int *cport, int *cport2);
static void handle_client(int sock);
static void server_task(void *arg);
/******************************************************************************
    Local function definitions
******************************************************************************/
static int send_rtp_packet_udp(const uint8_t *payload, size_t payload_len, uint32_t ts, bool marker)
{
    if (s_rtp_udp_fd < 0 || s_client_rtp_addr.sin_port == 0)
	{
        return -1; // todavia no hubo SETUP con client_port valido
    }

    // Solo el header RTP puro (12B), sin framing interleaved: eso era
    // exclusivo del transporte TCP.
    uint8_t hdr[12];
    hdr[0] = 0x80;
    hdr[1] = (marker ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
    hdr[2] = (s_seq >> 8) & 0xFF;
    hdr[3] = s_seq & 0xFF;
    s_seq++;
    hdr[4]  = (ts >> 24) & 0xFF; hdr[5]  = (ts >> 16) & 0xFF;
    hdr[6]  = (ts >> 8) & 0xFF;  hdr[7]  = ts & 0xFF;
    hdr[8]  = (SSRC >> 24) & 0xFF; hdr[9]  = (SSRC >> 16) & 0xFF;
    hdr[10] = (SSRC >> 8) & 0xFF;  hdr[11] = SSRC & 0xFF;

    // Igual que en la version TCP: el header va en stack, el payload
    // apunta directo al buffer original (PSRAM) sin memcpy intermedio.
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len  = 12;
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len  = payload_len;

    struct msghdr msg =
	{
        .msg_name    = &s_client_rtp_addr,
        .msg_namelen = sizeof(s_client_rtp_addr),
        .msg_iov     = iov,
        .msg_iovlen  = 2,
    };

    ssize_t total = 12 + payload_len;
    ssize_t sent = sendmsg(s_rtp_udp_fd, &msg, 0);

    return (sent == total) ? 0 : -1;
}

static int rtp_send_nal_udp(const uint8_t *nal, size_t nal_len, uint32_t ts, bool last_nal_in_frame)
{
    if (nal_len == 0)
	{
		return 0;
	}

    if (nal_len <= RTP_MAX_PAYLOAD)
	{
        return send_rtp_packet_udp(nal, nal_len, ts, last_nal_in_frame);
    }

    // Fragmentacion FU-A sin copiar el payload
    uint8_t nal_header = nal[0];
    uint8_t nal_type   = nal_header & 0x1F;
    uint8_t nri        = nal_header & 0x60;
    const uint8_t *p   = nal + 1;
    size_t remaining   = nal_len - 1;
    bool first         = true;
    bool any_fail      = false;

    while (remaining > 0)
	{
        size_t chunk = remaining;
        if (chunk > RTP_MAX_PAYLOAD - 2) chunk = RTP_MAX_PAYLOAD - 2;
        bool is_last_frag = (remaining - chunk) == 0;

        uint8_t hdr[14]; // 12 bytes RTP + 2 bytes FU-A (sin framing interleaved)
        hdr[0] = 0x80;
        hdr[1] = ((is_last_frag && last_nal_in_frame) ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
        hdr[2] = (s_seq >> 8) & 0xFF; hdr[3] = s_seq & 0xFF; s_seq++;
        hdr[4]  = (ts >> 24) & 0xFF; hdr[5]  = (ts >> 16) & 0xFF;
        hdr[6]  = (ts >> 8) & 0xFF;  hdr[7]  = ts & 0xFF;
        hdr[8]  = (SSRC >> 24) & 0xFF; hdr[9]  = (SSRC >> 16) & 0xFF;
        hdr[10] = (SSRC >> 8) & 0xFF;  hdr[11] = SSRC & 0xFF;

        // FU-A Indicator + Header
        hdr[12] = 0x1C | nri;
        hdr[13] = (first ? 0x80 : 0x00) | (is_last_frag ? 0x40 : 0x00) | nal_type;

        struct iovec iov[2];
        iov[0].iov_base = hdr;
        iov[0].iov_len  = 14;
        iov[1].iov_base = (void *)p;
        iov[1].iov_len  = chunk;

        struct msghdr msg =
		{
            .msg_name    = &s_client_rtp_addr,
            .msg_namelen = sizeof(s_client_rtp_addr),
            .msg_iov     = iov,
            .msg_iovlen  = 2,
        };

        ssize_t total = 14 + chunk;
        if (sendmsg(s_rtp_udp_fd, &msg, 0) != total)
		{
            // No cortamos el frame entero por un fragmento perdido: UDP es
            // lossy por diseno, seguimos con el resto de fragmentos/frames.
            any_fail = true;
        }

        p += chunk;
        remaining -= chunk;
        first = false;

        // Pacing: un I-frame grande genera muchos fragmentos FU-A seguidos.
        // Mandarlos todos "a la velocidad de la CPU" satura colas rio abajo
        // (SPI/SDIO hacia el co-procesador WiFi, driver WiFi, o el socket
        // UDP de recepcion del lado del cliente) y eso se traduce en
        // perdida de paquetes EN RAFAGA justo en los frames mas importantes.
        // Un delay corto entre fragmentos le da tiempo a esas colas a
        // drenar. Costo: unos pocos ms extra de latencia por I-frame grande,
        // a cambio de bastante menos corrupcion visual.
        if (remaining > 0)
		{
            esp_rom_delay_us(RTP_INTER_PACKET_DELAY_US);
        }
    }

    return any_fail ? -1 : 0;
}

static int rtp_send_annexb_frame(const uint8_t *data, size_t len, uint32_t ts)
{
    size_t i = 0;
    // localizar todos los NALs primero para saber cual es el ultimo
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

    bool any_fail = false;
    for (int k = 0; k < n_nals; k++)
	{
        bool last = (k == n_nals - 1);
		if (rtp_send_nal_udp(&data[nals[k].start], nals[k].len, ts, last) != 0)
		{
            // Igual que en rtp_send_nal_udp: un NAL/fragmento perdido no
            // aborta la sesion, solo se pierde ese pedazo de frame.
            any_fail = true;
        }
        if (!last)
		{
            esp_rom_delay_us(RTP_INTER_PACKET_DELAY_US);
        }
    }

    return any_fail ? -1 : 0;
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

// Busca "client_port=X-Y" dentro del header Transport: del SETUP.
// Devuelve true si encontro un client_port valido.
static bool parse_client_port(const char *req, int *cport, int *cport2)
{
    *cport = 0;
    *cport2 = 0;
    const char *t = strstr(req, "client_port=");
    if (t == NULL)
	{
        return false;
    }
    sscanf(t, "client_port=%d-%d", cport, cport2);
    return (*cport > 0);
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

	// Desactivar Nagle en el socket de CONTROL (latencia minima en las
	// respuestas RTSP; ya no lleva datos de video, eso ahora es UDP aparte).
	int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	s_seq = 0;
    // Limpiar cualquier destino RTP de una sesion anterior hasta que este
    // cliente haga su propio SETUP.
    memset(&s_client_rtp_addr, 0, sizeof(s_client_rtp_addr));

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
            int cport = 0, cport2 = 0;
            if (!parse_client_port(req, &cport, &cport2))
			{
                // Este build solo soporta transporte UDP; si el cliente no
                // mando client_port (p.ej. pidio TCP interleaved), rechazamos.
                snprintf(resp, sizeof(resp), "RTSP/1.0 461 Unsupported Transport\r\nCSeq: %s\r\n\r\n", cseq);
                send(sock, resp, strlen(resp), 0);
                continue;
            }

            // IP del cliente: la sacamos del socket TCP de control, que ya
            // esta conectado a el.
            struct sockaddr_in peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            getpeername(sock, (struct sockaddr *)&peer_addr, &peer_len);

            s_client_rtp_addr.sin_family = AF_INET;
            s_client_rtp_addr.sin_addr   = peer_addr.sin_addr;
            s_client_rtp_addr.sin_port   = htons((uint16_t)cport);

            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n"
                "Session: %s\r\n\r\n",
                cseq, cport, cport2, SERVER_RTP_PORT, SERVER_RTP_PORT + 1, (unsigned)SSRC, RTSP_SESSION_ID);
            send(sock, resp, strlen(resp), 0);

            ESP_LOGI(TAG, "SETUP UDP: cliente %s, client_port=%d, server_port=%d", inet_ntoa(s_client_rtp_addr.sin_addr), cport, SERVER_RTP_PORT);

        } else if (!strncmp(req, "PLAY", 4))
		{
            if (s_client_rtp_addr.sin_port == 0)
			{
                // No hubo SETUP valido antes del PLAY.
                snprintf(resp, sizeof(resp), "RTSP/1.0 455 Method Not Valid In This State\r\nCSeq: %s\r\n\r\n", cseq);
                send(sock, resp, strlen(resp), 0);
                continue;
            }

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
            // sacamos frames de la cola y los mandamos como RTP/UDP.
            while (s_server_running && playing)
			{
                encoded_frame_t *frame = NULL;
                if (xQueueReceive(g_encoded_frame_queue, &frame, pdMS_TO_TICKS(500)) == pdTRUE)
				{
                    // A diferencia de TCP: un fallo de envio UDP (paquete
                    // perdido, ENOBUFS momentaneo) NO termina la sesion,
                    // solo se pierde ese frame. La sesion solo termina por
                    // TEARDOWN o porque el socket de CONTROL se cae.
                    rtp_send_annexb_frame(frame->data, frame->len, frame->rtp_timestamp);
                    encoded_frame_free(frame);
                }
                // chequeo no bloqueante de TEARDOWN/GET_PARAMETER entrante
                // en el socket de CONTROL (TCP)
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

    // Fin de sesion: invalidar destino RTP para que no sigan saliendo
    // paquetes UDP hacia un cliente que ya se desconecto.
    memset(&s_client_rtp_addr, 0, sizeof(s_client_rtp_addr));
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
    // --- Socket TCP de control (RTSP: OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN) ---
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

    // --- Socket UDP de datos (plano RTP) ---
    s_rtp_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rtp_udp_fd < 0)
	{
        ESP_LOGE(TAG, "no se pudo crear socket UDP RTP (%s)", strerror(errno));
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    // Buffer de envio generoso: un I-frame grande puede generar varios
    // paquetes seguidos en rafaga.
    int sndbuf = 64 * 1024;
    setsockopt(s_rtp_udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in rtp_addr = { 0 };
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    rtp_addr.sin_port = htons(SERVER_RTP_PORT);
    if (bind(s_rtp_udp_fd, (struct sockaddr *)&rtp_addr, sizeof(rtp_addr)) != 0)
	{
        ESP_LOGE(TAG, "bind UDP :%u fallo (%s)", SERVER_RTP_PORT, strerror(errno));
        close(s_rtp_udp_fd);
        s_rtp_udp_fd = -1;
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    s_server_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(server_task, "rtsp_server", 16384, NULL, 8, &s_server_task, 0);
    if (ok != pdPASS)
	{
		return ESP_ERR_NO_MEM;
	}

    ESP_LOGI(TAG, "Servidor RTSP escuchando en :%u (control TCP) / :%u (datos RTP/UDP)", port, SERVER_RTP_PORT);

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

    if (s_rtp_udp_fd >= 0)
	{
        close(s_rtp_udp_fd);
        s_rtp_udp_fd = -1;
    }

    return ESP_OK;
}