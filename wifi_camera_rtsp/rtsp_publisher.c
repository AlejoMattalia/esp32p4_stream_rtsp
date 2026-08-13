#include "rtsp_publisher.h"

#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "rtsp_server.h"

#define RTP_PAYLOAD_TYPE 96
#define RTP_MAX_PAYLOAD 1200
#define RESPONSE_SIZE 2048
#define SDP_SIZE 768

static const char *TAG = "[RTSP_PUB]";
static rtsp_publisher_config_t s_config;
static TaskHandle_t s_task;
static volatile bool s_running;
static uint16_t s_sequence;
static uint32_t s_ssrc = 0x415E9001;

static int send_all(int socket_fd, const void *data, size_t length)
{
    const uint8_t *cursor = data;
    while (length > 0) {
        ssize_t sent = send(socket_fd, cursor, length, 0);
        if (sent <= 0) return -1;
        cursor += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int send_vectors(int socket_fd, struct iovec *iov, int count)
{
    while (count > 0) {
        struct msghdr message = {.msg_iov = iov, .msg_iovlen = (size_t)count};
        ssize_t sent = sendmsg(socket_fd, &message, 0);
        if (sent <= 0) return -1;
        while (count > 0 && sent >= (ssize_t)iov[0].iov_len) {
            sent -= (ssize_t)iov[0].iov_len;
            iov++;
            count--;
        }
        if (count > 0 && sent > 0) {
            iov[0].iov_base = (uint8_t *)iov[0].iov_base + sent;
            iov[0].iov_len -= (size_t)sent;
        }
    }
    return 0;
}

static int connect_server(void)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", s_config.port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(s_config.host, port, &hints, &addresses) != 0) return -1;

    int socket_fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) continue;
        struct timeval timeout = {.tv_sec = 10};
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    return socket_fd;
}

static void basic_authorization(char *output, size_t output_size)
{
    char credentials[160];
    char encoded[256];
    size_t encoded_length = 0;
    snprintf(credentials, sizeof(credentials), "%s:%s", s_config.camera_id, s_config.device_secret);
    if (mbedtls_base64_encode((uint8_t *)encoded, sizeof(encoded) - 1, &encoded_length,
                              (const uint8_t *)credentials, strlen(credentials)) != 0) {
        output[0] = 0;
        return;
    }
    encoded[encoded_length] = 0;
    snprintf(output, output_size, "Authorization: Basic %s\r\n", encoded);
    memset(credentials, 0, sizeof(credentials));
}

static int receive_response(int socket_fd, char *response, size_t response_size)
{
    size_t used = 0;
    while (used + 1 < response_size) {
        ssize_t received = recv(socket_fd, response + used, response_size - used - 1, 0);
        if (received <= 0) return -1;
        used += (size_t)received;
        response[used] = 0;
        if (strstr(response, "\r\n\r\n")) break;
    }
    int status = 0;
    return sscanf(response, "RTSP/1.0 %d", &status) == 1 ? status : -1;
}

static int request(int socket_fd, int cseq, const char *method, const char *url,
                   const char *extra_headers, const char *body,
                   char *response, size_t response_size)
{
    char authorization[320];
    char header[1536];
    basic_authorization(authorization, sizeof(authorization));
    size_t body_length = body ? strlen(body) : 0;
    int header_length = body
        ? snprintf(header, sizeof(header),
            "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Anny-ESP32P4/1.0\r\n"
            "%s%sContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n",
            method, url, cseq, authorization, extra_headers ? extra_headers : "",
            (unsigned)body_length)
        : snprintf(header, sizeof(header),
            "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Anny-ESP32P4/1.0\r\n%s%s\r\n",
            method, url, cseq, authorization, extra_headers ? extra_headers : "");
    if (header_length <= 0 || (size_t)header_length >= sizeof(header)) return -1;
    if (send_all(socket_fd, header, (size_t)header_length) != 0) return -1;
    if (body && send_all(socket_fd, body, body_length) != 0) return -1;
    return receive_response(socket_fd, response, response_size);
}

static int build_sdp(char *output, size_t output_size)
{
    char sps_base64[192] = {0};
    char pps_base64[192] = {0};
    size_t sps_length = 0, pps_length = 0;
    uint8_t profile[3] = {0};

    xSemaphoreTake(g_sps_pps_cache.lock, portMAX_DELAY);
    if (!g_sps_pps_cache.valid || g_sps_pps_cache.sps_len < 4) {
        xSemaphoreGive(g_sps_pps_cache.lock);
        return -1;
    }
    mbedtls_base64_encode((uint8_t *)sps_base64, sizeof(sps_base64) - 1, &sps_length,
                          g_sps_pps_cache.sps, g_sps_pps_cache.sps_len);
    mbedtls_base64_encode((uint8_t *)pps_base64, sizeof(pps_base64) - 1, &pps_length,
                          g_sps_pps_cache.pps, g_sps_pps_cache.pps_len);
    memcpy(profile, g_sps_pps_cache.sps + 1, sizeof(profile));
    xSemaphoreGive(g_sps_pps_cache.lock);
    sps_base64[sps_length] = 0;
    pps_base64[pps_length] = 0;

    return snprintf(output, output_size,
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Anny ESP32-P4\r\n"
        "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;"
        "sprop-parameter-sets=%s,%s\r\na=control:trackID=0\r\n",
        profile[0], profile[1], profile[2], sps_base64, pps_base64);
}

static int send_rtp_payload(int socket_fd, const uint8_t *payload, size_t payload_length,
                            uint32_t timestamp, bool marker)
{
    uint8_t header[16];
    uint16_t rtp_length = (uint16_t)(12 + payload_length);
    header[0] = '$'; header[1] = 0;
    header[2] = (uint8_t)(rtp_length >> 8); header[3] = (uint8_t)rtp_length;
    header[4] = 0x80; header[5] = (marker ? 0x80 : 0) | RTP_PAYLOAD_TYPE;
    header[6] = (uint8_t)(s_sequence >> 8); header[7] = (uint8_t)s_sequence++;
    header[8] = (uint8_t)(timestamp >> 24); header[9] = (uint8_t)(timestamp >> 16);
    header[10] = (uint8_t)(timestamp >> 8); header[11] = (uint8_t)timestamp;
    header[12] = (uint8_t)(s_ssrc >> 24); header[13] = (uint8_t)(s_ssrc >> 16);
    header[14] = (uint8_t)(s_ssrc >> 8); header[15] = (uint8_t)s_ssrc;
    struct iovec vectors[2] = {{header, sizeof(header)}, {(void *)payload, payload_length}};
    return send_vectors(socket_fd, vectors, 2);
}

static int send_nal(int socket_fd, const uint8_t *nal, size_t nal_length,
                    uint32_t timestamp, bool last_nal)
{
    if (nal_length <= RTP_MAX_PAYLOAD) {
        return send_rtp_payload(socket_fd, nal, nal_length, timestamp, last_nal);
    }
    uint8_t nal_header = nal[0];
    const uint8_t *cursor = nal + 1;
    size_t remaining = nal_length - 1;
    bool first = true;
    while (remaining) {
        size_t chunk = remaining > RTP_MAX_PAYLOAD - 2 ? RTP_MAX_PAYLOAD - 2 : remaining;
        bool last = chunk == remaining;
        uint8_t payload_header[2] = {
            (uint8_t)(0x1C | (nal_header & 0x60)),
            (uint8_t)((first ? 0x80 : 0) | (last ? 0x40 : 0) | (nal_header & 0x1F))
        };
        uint8_t rtp_header[16];
        uint16_t rtp_length = (uint16_t)(12 + 2 + chunk);
        rtp_header[0]='$'; rtp_header[1]=0; rtp_header[2]=(uint8_t)(rtp_length>>8); rtp_header[3]=(uint8_t)rtp_length;
        rtp_header[4]=0x80; rtp_header[5]=(uint8_t)(((last && last_nal)?0x80:0)|RTP_PAYLOAD_TYPE);
        rtp_header[6]=(uint8_t)(s_sequence>>8); rtp_header[7]=(uint8_t)s_sequence++;
        rtp_header[8]=(uint8_t)(timestamp>>24); rtp_header[9]=(uint8_t)(timestamp>>16);
        rtp_header[10]=(uint8_t)(timestamp>>8); rtp_header[11]=(uint8_t)timestamp;
        rtp_header[12]=(uint8_t)(s_ssrc>>24); rtp_header[13]=(uint8_t)(s_ssrc>>16);
        rtp_header[14]=(uint8_t)(s_ssrc>>8); rtp_header[15]=(uint8_t)s_ssrc;
        struct iovec vectors[3]={{rtp_header,sizeof(rtp_header)},{payload_header,2},{(void *)cursor,chunk}};
        if (send_vectors(socket_fd, vectors, 3) != 0) return -1;
        cursor += chunk; remaining -= chunk; first = false;
    }
    return 0;
}

static int send_frame(int socket_fd, const encoded_frame_t *frame)
{
    typedef struct { size_t start, length; } nal_range_t;
    nal_range_t nals[20];
    int count = 0;
    size_t i = 0;
    while (i + 3 < frame->len && count < 20) {
        size_t start_code = 0;
        if (frame->data[i] == 0 && frame->data[i+1] == 0 && frame->data[i+2] == 1) start_code = 3;
        else if (i + 4 < frame->len && frame->data[i] == 0 && frame->data[i+1] == 0 && frame->data[i+2] == 0 && frame->data[i+3] == 1) start_code = 4;
        if (!start_code) { i++; continue; }
        size_t start = i + start_code;
        size_t end = start;
        while (end + 3 < frame->len && !(frame->data[end] == 0 && frame->data[end+1] == 0 &&
              (frame->data[end+2] == 1 || (frame->data[end+2] == 0 && frame->data[end+3] == 1)))) end++;
        if (end + 3 >= frame->len) end = frame->len;
        nals[count++] = (nal_range_t){start, end - start};
        i = end;
    }
    for (int index = 0; index < count; index++) {
        if (send_nal(socket_fd, frame->data + nals[index].start, nals[index].length,
                     frame->rtp_timestamp, index == count - 1) != 0) return -1;
    }
    return count ? 0 : -1;
}

static void drain_frames(void)
{
    encoded_frame_t *frame = NULL;
    while (xQueueReceive(g_encoded_frame_queue, &frame, 0) == pdTRUE) encoded_frame_free(frame);
}

static int publish_session(void)
{
    for (int waited = 0; !g_sps_pps_cache.valid && waited < 10000; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    char sdp[SDP_SIZE];
    if (build_sdp(sdp, sizeof(sdp)) <= 0) {
        ESP_LOGW(TAG, "No hay SPS/PPS disponibles");
        return -1;
    }
    int socket_fd = connect_server();
    if (socket_fd < 0) {
        ESP_LOGW(TAG, "No se pudo conectar a %s:%u", s_config.host, s_config.port);
        return -1;
    }
    char base_url[192], track_url[220], response[RESPONSE_SIZE], session[128] = {0};
    snprintf(base_url, sizeof(base_url), "rtsp://%s:%u/%s", s_config.host, s_config.port, s_config.camera_id);
    snprintf(track_url, sizeof(track_url), "%s/trackID=0", base_url);
    int cseq = 1;
    int status = request(socket_fd, cseq++, "ANNOUNCE", base_url, NULL, sdp, response, sizeof(response));
    if (status != 200) { ESP_LOGE(TAG, "ANNOUNCE rechazado: HTTP %d", status); close(socket_fd); return -1; }
    status = request(socket_fd, cseq++, "SETUP", track_url,
                     "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", NULL,
                     response, sizeof(response));
    if (status != 200) { ESP_LOGE(TAG, "SETUP rechazado: HTTP %d", status); close(socket_fd); return -1; }
    const char *session_header = strstr(response, "Session:");
    if (session_header) {
        session_header += 8; while (*session_header == ' ') session_header++;
        size_t length = strcspn(session_header, ";\r\n");
        if (length >= sizeof(session)) length = sizeof(session) - 1;
        memcpy(session, session_header, length); session[length] = 0;
    }
    char record_headers[200];
    snprintf(record_headers, sizeof(record_headers), "Session: %s\r\nRange: npt=0.000-\r\n", session);
    status = request(socket_fd, cseq++, "RECORD", base_url, record_headers, NULL, response, sizeof(response));
    if (status != 200) { ESP_LOGE(TAG, "RECORD rechazado: HTTP %d", status); close(socket_fd); return -1; }

    ESP_LOGI(TAG, "Transmitiendo %s hacia AWS", s_config.camera_id);
    s_sequence = 0;
    drain_frames();
    while (s_running) {
        encoded_frame_t *frame = NULL;
        if (xQueueReceive(g_encoded_frame_queue, &frame, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int result = send_frame(socket_fd, frame);
            encoded_frame_free(frame);
            if (result != 0) break;
        }
    }
    close(socket_fd);
    return s_running ? -1 : 0;
}

static void publisher_task(void *argument)
{
    unsigned backoff_seconds = 1;
    while (s_running) {
        if (publish_session() == 0) break;
        ESP_LOGW(TAG, "Reconectando en %u segundos", backoff_seconds);
        vTaskDelay(pdMS_TO_TICKS(backoff_seconds * 1000));
        if (backoff_seconds < 30) backoff_seconds = backoff_seconds < 5 ? backoff_seconds * 2 : 30;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rtsp_publisher_start(const rtsp_publisher_config_t *config)
{
    if (!config || !config->host || !config->camera_id || !config->device_secret) return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_OK;
    s_config = *config;
    s_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(publisher_task, "rtsp_publisher", 12288,
                                                 NULL, 9, &s_task, 0);
    if (created != pdPASS) { s_running = false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t rtsp_publisher_stop(void)
{
    s_running = false;
    return ESP_OK;
}
