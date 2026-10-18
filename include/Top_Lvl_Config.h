/*
    Cấu hình cho thiết bị ĐỂ SỬ DỤNG TẠI MAIN.CPP
*/
#ifndef TOP_LVL_CONFIG_H
#define TOP_LVL_CONFIG_H

// ================= CẤU HÌNH KHỞI TẠO =================
// #define PROGRAM_DEBUG 1

#ifndef USE_KCT8103L_PA
#define USE_KCT8103L_PA
#endif

#define TCP_IP 0

#ifndef RTCM_COMMUNICATION_PROTOCOL
#define RTCM_COMMUNICATION_PROTOCOL TCP_IP
#endif

#define GNSS_MODULE_TYPE_UBLOX 0
#define GNSS_MODULE_TYPE_UNICORE 1

#ifndef GNSS_MODULE_TYPE
#define GNSS_MODULE_TYPE GNSS_MODULE_TYPE_UBLOX // Legacy ATG default; CORS selects the type at runtime.
#endif

// Receiver UART wired to ESP32 Serial1 (COM1/COM2/COM3 on UM980/UM982).
#ifndef GNSS_UNICORE_PORT
#define GNSS_UNICORE_PORT 2
#endif
static_assert(GNSS_UNICORE_PORT >= 1 && GNSS_UNICORE_PORT <= 3, "Invalid Unicore UART");

// CORS agent compatibility switches.  RTCM is framed and forwarded as raw
// bytes only; this firmware deliberately does not decode RTCM message types.
#ifndef BACKEND_PUBLISH_RAW_RTCM
#define BACKEND_PUBLISH_RAW_RTCM 0
#endif

#ifndef BACKEND_WEBSOCKET_ENABLED
#define BACKEND_WEBSOCKET_ENABLED 1
#endif

#endif
