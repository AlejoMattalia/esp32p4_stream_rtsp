# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

file(GLOB srcs_files "../wifi_camera_rtsp/*.c")

idf_component_register(
    SRCS
        "main.c"
        ${srcs_files}

    INCLUDE_DIRS
        "."
        "../wifi_camera_rtsp"

    REQUIRES
        esp_video
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
        esp_timer
        mbedtls
        esp_driver_cam
        esp_driver_isp
        esp_cam_sensor
)