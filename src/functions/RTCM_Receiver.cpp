#include "functions/RTCM_Receiver.h"
#include "HardwareSerial.h"

namespace {
    constexpr uint32_t CRC24Q_POLYNOMIAL = 0x1864CFB;
    uint32_t rtcmMessageTypeCounts[RTCM_SUPPORTED_MESSAGE_TYPE_COUNT] = {};
    String rtcmParserBuffer;

    uint32_t crc24q(const uint8_t *data, size_t length) {
        uint32_t crc = 0;
        for (size_t i = 0; i < length; ++i) {
            crc ^= static_cast<uint32_t>(data[i]) << 16;
            for (uint8_t bit = 0; bit < 8; ++bit) {
                crc <<= 1;
                if (crc & 0x1000000) {
                    crc ^= CRC24Q_POLYNOMIAL;
                }
            }
        }
        return crc & 0xFFFFFF;
    }

    int messageTypeBit(const uint16_t messageType) {
        constexpr uint16_t supportedTypes[] = {
            1005, 1074, 1077, 1084, 1087,
            1094, 1097, 1124, 1127, 1230,
        };
        for (size_t i = 0; i < sizeof(supportedTypes) / sizeof(supportedTypes[0]); ++i) {
            if (supportedTypes[i] == messageType) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void analyzeRtcmFrames(const String &data) {
        rtcmParserBuffer.concat(data);

        while (rtcmParserBuffer.length() >= 3) {
            int preamble = rtcmParserBuffer.indexOf(static_cast<char>(0xD3));
            if (preamble < 0) {
                rtcmParserBuffer = "";
                return;
            }
            if (preamble > 0) {
                rtcmParserBuffer.remove(0, preamble);
            }

            const uint16_t payloadLength =
                (static_cast<uint16_t>(static_cast<uint8_t>(rtcmParserBuffer[1])) & 0x03) << 8 |
                static_cast<uint8_t>(rtcmParserBuffer[2]);
            const size_t frameLength = 3 + payloadLength + 3;
            if (rtcmParserBuffer.length() < frameLength) {
                return;
            }

            const uint8_t *frame = reinterpret_cast<const uint8_t *>(rtcmParserBuffer.c_str());
            const uint32_t expectedCrc = (static_cast<uint32_t>(frame[frameLength - 3]) << 16) |
                                         (static_cast<uint32_t>(frame[frameLength - 2]) << 8) |
                                         frame[frameLength - 1];
            if (crc24q(frame, frameLength - 3) == expectedCrc && payloadLength >= 2) {
                const uint16_t messageType =
                    (static_cast<uint16_t>(frame[3]) << 4) | (frame[4] >> 4);
                const int bit = messageTypeBit(messageType);
                if (bit >= 0) {
                    ++rtcmMessageTypeCounts[bit];
                }
            }
            rtcmParserBuffer.remove(0, frameLength);
        }
    }
}

String receiveRtcmFromGnss() {
    #ifdef PROGRAM_TEST
    String rtcmData = "THIS IS NOT A REAL RTCM DATA. THIS IS A TEST STRING FOR UNIT TESTING PURPOSES.";
    #else
    String rtcmData;
    uint8_t buf[128];

    while (Serial1.available() > 0) {
        const size_t bytesAvailable = static_cast<size_t>(Serial1.available());
        const size_t bytesToRead = min(bytesAvailable, sizeof(buf));

        if (bytesToRead == 0) {
            break;
        }

        const size_t bytesRead = Serial1.readBytes(reinterpret_cast<char*>(buf), bytesToRead);
        if (bytesRead == 0) {
            break;
        }

        rtcmData.concat(reinterpret_cast<char*>(buf), bytesRead);
    }
    #endif

    if (!rtcmData.isEmpty()) {
        analyzeRtcmFrames(rtcmData);
    }

    if (!rtcmData.isEmpty()) {
        constexpr uint16_t supportedRtcmTypes[] = {
            1005, 1074, 1077, 1084, 1087,
            1094, 1097, 1124, 1127, 1230,
        };
        Serial.println("[UM980] Da nhan du lieu RTCM tu mach RTK. So byte: " + String(rtcmData.length()));
        Serial.println("[UM980] So luong cac loai tin nhan RTCM da nhan: ");
        RtcmMessageCounts messageCounts = consumeRtcmMessageCounts();
        for (size_t i = 0; i < RTCM_SUPPORTED_MESSAGE_TYPE_COUNT; ++i) {
            Serial.println("  - Tin nhan loai " + String(supportedRtcmTypes[i]) + ": " + String(messageCounts.values[i]));
        }
        #if PROGRAM_DEBUG
        Serial.println("[UM980] Du lieu nhan duoc: ");
        Serial.println(rtcmData);
        #endif
    } else {
        Serial.println("[UM980] Khong co du lieu RTCM hop le.");
    }
    return rtcmData;
}

RtcmMessageCounts consumeRtcmMessageCounts() {
    RtcmMessageCounts messageCounts;
    for (size_t i = 0; i < RTCM_SUPPORTED_MESSAGE_TYPE_COUNT; ++i) {
        messageCounts.values[i] = rtcmMessageTypeCounts[i];
        rtcmMessageTypeCounts[i] = 0;
    }
    return messageCounts;
}
