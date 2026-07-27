# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/grivera/.espressif/v6.0/esp-idf/components/bootloader/subproject"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/tmp"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/src/bootloader-stamp"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/src"
  "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/grivera/develop/firmware/trcom_firmware/caecus_repos/esp32p4_stream_rtsp/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
