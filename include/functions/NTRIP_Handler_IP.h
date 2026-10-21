#ifndef NTRIP_HANDLER_IP_H
#define NTRIP_HANDLER_IP_H

// ============ GẮN CÁC THƯ VIỆN CẦN THIẾT ==============

#include <Arduino.h>
#include "Top_Lvl_Config.h"
#include "hardware/Connection_type.h"
#include "functions/ubx_cmd_builder.h"

extern SemaphoreHandle_t rtcmBufferMutex;
extern SemaphoreHandle_t tcpStreamMutex;

// =============== KHAI BÁO HÀM =================

int setupNTRIP();
int bootstrapUM980();
UbxCmdBuilder::GnssSensorType detectGnssSensorType();
int loopNTRIP(String& rtcmData);
int connectNTRIP();
// Thêm hàm lấy trạng thái NTRIP
bool isNtripConnected();
Client& activeNtripClient();

#endif
