# ESP32-P4 RTSP Video Server

## Publicación en Anny/AWS

1. Crear una placa en el panel de cámaras y obtener su ID y secreto.
2. Compilar y flashear. En el primer arranque la placa crea `ANNY-CAM-xxxxxx`
   (clave `annysetup`).
3. En Anny abrir **Dispositivos > Lentes con cámara ESP32 > Conectar**, ingresar
   el código de activación y el WiFi del lugar. La app canjea el código por la
   identidad de publicación y envía todo a la placa.
4. El firmware reinicia y publica por RTSP/TCP en MediaMTX mediante
   `ANNOUNCE`, `SETUP` y `RECORD`, y se reconecta automáticamente.
5. Vincular la placa desde Anny con el código de activación.

Si la red guardada no está disponible, la placa vuelve a crear su red
`ANNY-CAM-xxxxxx` para poder configurarla en una ubicación nueva. La IP y el
puerto permanecen fijos en `device_config.h`; WiFi, ID y secreto quedan en NVS.

Por seguridad, los logs no deben imprimir `Authorization`, `Device secret` ni
la URL RTSP completa con credenciales. Basic Base64 codifica, pero no cifra.

Servidor RTSP para streaming en vivo desde un ESP32-P4 con cámara OV5647 (CSI + ISP) y encoder H.264 por hardware (V4L2 M2M). Pensado como firmware de validación/desarrollo, no para producción.

## Archivos principales

| Archivo | Rol |
|---|---|
| `camera_rtsp.c` / `.h` | Orquesta el pipeline completo: captura CSI (`/dev/video0`), encoder H.264 (`/dev/video11`), y la tarea que empaqueta cada frame codificado en `encoded_frame_t` y lo entrega a `g_encoded_frame_queue`. Expone `cam_rtsp_init/start/stop/deinit`. |
| `rtsp_server.c` / `.h` | Servidor RTSP: maneja el handshake de control (OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN) y arma/envía los paquetes RTP (fragmentación H.264 FU-A por RFC 6184) a partir de los frames que llegan por `g_encoded_frame_queue`. |
| `wifi_connect.c` / `.h` | Conexión WiFi en modo STA usando el co-procesador remoto (esp-hosted sobre ESP32-C6). Bloquea hasta obtener IP o agotar reintentos; expone `wifi_connect_get_ip_str()` para loguear la IP a usar en ffplay. |

## Dos variantes del servidor (dos branches)

Este proyecto tiene dos formas de transportar el video, en dos branches distintos, para poder comparar comportamiento en distintas condiciones de red:

- **`main`** — `rtsp_server.c` transporta el video **interleaved dentro del mismo socket TCP** de control (`RTP/AVP/TCP`). Más simple, sin problemas de NAT/firewall, pero sufre head-of-line blocking: si se pierde un segmento TCP, el stream se congela hasta la retransmisión.
- **`rtsp_stream_video_udp`** — `rtsp_server.c` transporta el video por un **socket UDP dedicado** (`RTP/AVP` puro, puerto 6970), negociado en el `SETUP` vía `client_port=`/`server_port=`. Menor latencia y sin freezes por HOL blocking, a cambio de pérdida de paquetes real (visible como corrupción de macroblocks hasta el próximo keyframe). Incluye además un ajuste de `I_PERIOD` en `camera_rtsp.c` (intervalo de keyframe) para reducir el tamaño/frecuencia de las ráfagas de paquetes.

El servidor detecta el modo a través del `Transport:` que manda el cliente en el `SETUP`; en la variante UDP, si el cliente no ofrece `client_port=` (es decir, pide TCP), el server responde `461 Unsupported Transport`.

## Probar con ffplay

Reemplazá `<IP_OBTENIDA>` por la IP que loguea el ESP32-P4 al conectarse (ver `wifi_connect_get_ip_str()`).

### Branch `main` (TCP interleaved)

```bash
ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental -rtsp_flags prefer_tcp rtsp://<IP_OBTENIDA>:554/stream
```

```bash
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay -framedrop rtsp://<IP_OBTENIDA>:554/stream
```

Cualquiera de los dos comandos sirve; el server siempre fuerza TCP interleaved en esta variante independientemente de lo que pida el cliente.

### Branch `rtsp_stream_video_udp`

```bash
ffplay -rtsp_transport udp -fflags nobuffer -flags low_delay -framedrop rtsp://<IP_OBTENIDA>:554/stream
```

`-rtsp_transport udp` es obligatorio acá: el server de este branch solo acepta `SETUP` con `client_port=` (transporte UDP).

En los tres casos, `-fflags nobuffer -flags low_delay -framedrop` minimizan la latencia de reproducción de ffplay (evitan el buffering pensado para VOD y descartan frames si el decode se atrasa, en vez de acumular delay).
