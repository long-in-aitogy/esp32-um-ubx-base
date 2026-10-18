#include "functions/RTCM_Receiver.h"

#include "HardwareSerial.h"
#include "functions/Backend_Agent.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

constexpr size_t MAX_NMEA_LENGTH = 512;
constexpr size_t MAX_RTCM_FRAME_LENGTH = 1029; // 3-byte header + 1023 + CRC

String nmeaBuffer;
uint8_t rtcmFrame[MAX_RTCM_FRAME_LENGTH] = {};
size_t rtcmLength = 0;
size_t rtcmExpectedLength = 0;

GnssTelemetrySnapshot telemetry;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t inputWindowStartedMs = 0;
uint32_t inputWindowBytes = 0;
uint32_t inputBytesPerSecond = 0;

void noteInputBytes(size_t count) {
    portENTER_CRITICAL(&telemetryMux);
    telemetry.bytesRead += count;
    telemetry.hasData = true;
    telemetry.lastDataMs = millis();
    portEXIT_CRITICAL(&telemetryMux);

}

void noteRtcmBytes(size_t count) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&telemetryMux);
    if (inputWindowStartedMs == 0) inputWindowStartedMs = now;
    inputWindowBytes += static_cast<uint32_t>(count);
    if (now - inputWindowStartedMs >= 1000UL) {
        const uint32_t elapsed = now - inputWindowStartedMs;
        inputBytesPerSecond = elapsed > 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(inputWindowBytes) * 1000ULL) / elapsed)
            : inputWindowBytes;
        inputWindowStartedMs = now;
        inputWindowBytes = 0;
    }
    portEXIT_CRITICAL(&telemetryMux);
}

double nmeaCoordinate(const char *raw, const char *hemisphere) {
    if (!raw || !*raw) return 0.0;
    const double value = atof(raw);
    const double degrees = static_cast<int>(value / 100.0);
    const double minutes = value - degrees * 100.0;
    double coordinate = degrees + minutes / 60.0;
    if (hemisphere && (hemisphere[0] == 'S' || hemisphere[0] == 'W')) {
        coordinate = -coordinate;
    }
    return coordinate;
}

void setFixStatus(char *target, size_t capacity, int quality) {
    const char *status = "UNKNOWN";
    switch (quality) {
        case 0: status = "NO_FIX"; break;
        case 1: status = "GPS_FIX"; break;
        case 2: status = "DGPS"; break;
        case 4: status = "RTK_FIXED"; break;
        case 5: status = "RTK_FLOAT"; break;
        default: status = "NO_FIX"; break;
    }
    strncpy(target, status, capacity - 1);
    target[capacity - 1] = '\0';
}

void parseNmeaSentence(const String &rawSentence) {
    String sentence = rawSentence;
    sentence.trim();
    if (sentence.length() < 7 || sentence[0] != '$') return;
    if (sentence.startsWith("$PUBX")) {
        backendAgent.setDetectedChipIfUnknown("Ublox");
    }

    char line[MAX_NMEA_LENGTH] = {};
    const size_t copyLength = min(sentence.length(), sizeof(line) - 1);
    memcpy(line, sentence.c_str(), copyLength);
    line[copyLength] = '\0';

    char *fields[32] = {};
    size_t fieldCount = 0;
    char *cursor = line + 1;
    while (fieldCount < (sizeof(fields) / sizeof(fields[0]))) {
        fields[fieldCount++] = cursor;
        char *comma = strchr(cursor, ',');
        if (!comma) break;
        *comma = '\0';
        cursor = comma + 1;
    }
    if (fieldCount == 0) return;

    const size_t typeLength = strlen(fields[0]);
    if (typeLength < 3) return;
    const char *type = fields[0] + typeLength - 3;

    bool updateFix = false;
    int fixQuality = 0;
    bool updatePosition = false;
    bool updateSnr = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    float hdop = 0.0F;
    uint8_t satellites = 0;
    float snrSum = 0.0F;
    uint32_t snrCount = 0;
    bool updateGst = false;
    float gstSemiMajor = 0.0F;
    float gstSemiMinor = 0.0F;
    float gstHacc = 0.0F;

    if (strcmp(type, "GGA") == 0) {
        updateFix = fieldCount > 6;
        fixQuality = updateFix ? atoi(fields[6]) : 0;
        if (fieldCount > 9 && fields[2][0] && fields[3][0]
            && fields[4][0] && fields[5][0]) {
            // GGA fields are: talker, UTC, latitude, N/S, longitude, E/W,
            // fix quality, satellites, HDOP, altitude, ...
            latitude = nmeaCoordinate(fields[2], fields[3]);
            longitude = nmeaCoordinate(fields[4], fields[5]);
            satellites = static_cast<uint8_t>(constrain(atoi(fields[7]), 0, 255));
            hdop = static_cast<float>(atof(fields[8]));
            altitude = atof(fields[9]);
            updatePosition = true;
        }
    } else if (strcmp(type, "GSA") == 0) {
        // GSA field 2 is only the 2D/3D solution dimension, not the RTK
        // quality reported by GGA. Keep the last GGA fix state authoritative
        // so a following GSA sentence cannot turn RTK_FIXED into a generic
        // 2D/3D status.
    } else if (strcmp(type, "GSV") == 0) {
        // GSV repeats satellite groups as PRN/elevation/azimuth/SNR.
        for (size_t i = 4; i + 3 < fieldCount; i += 4) {
            if (fields[i + 3] && *fields[i + 3]) {
                const int snr = atoi(fields[i + 3]);
                if (snr >= 0 && snr <= 99) {
                    snrSum += static_cast<float>(snr);
                    ++snrCount;
                }
            }
        }
        updateSnr = snrCount > 0;
    } else if (strcmp(type, "GST") == 0) {
        if (fieldCount > 7) {
            gstSemiMajor = static_cast<float>(atof(fields[3]));
            gstSemiMinor = static_cast<float>(atof(fields[4]));
            const float latitudeSigma = static_cast<float>(atof(fields[6]));
            const float longitudeSigma = static_cast<float>(atof(fields[7]));
            gstHacc = sqrtf(latitudeSigma * latitudeSigma + longitudeSigma * longitudeSigma);
            updateGst = gstHacc > 0.0F;
        }
    }

    portENTER_CRITICAL(&telemetryMux);
    telemetry.hasData = true;
    telemetry.lastDataMs = millis();
    ++telemetry.nmeaSentences;
    if (strcmp(type, "GGA") == 0) ++telemetry.ggaCount;
    else if (strcmp(type, "GSA") == 0) ++telemetry.gsaCount;
    else if (strcmp(type, "GSV") == 0) ++telemetry.gsvCount;
    else if (strcmp(type, "GST") == 0) ++telemetry.gstCount;

    if (updateFix) setFixStatus(telemetry.fixStatus, sizeof(telemetry.fixStatus), fixQuality);
    if (strcmp(type, "GGA") == 0) {
        strncpy(telemetry.lastGga, sentence.c_str(), sizeof(telemetry.lastGga) - 1);
        telemetry.lastGga[sizeof(telemetry.lastGga) - 1] = '\0';
        telemetry.lastGgaMs = millis();
    }
    if (updatePosition) {
        telemetry.latitude = latitude;
        telemetry.longitude = longitude;
        telemetry.altitude = altitude;
        telemetry.hdop = hdop;
        telemetry.satellites = satellites;
    }
    if (updateSnr) {
        const uint32_t oldSamples = telemetry.snrSamples;
        telemetry.snrSamples += snrCount;
        telemetry.latestGsvSamples = snrCount;
        telemetry.lastGsvMs = millis();
        telemetry.latestAverageSnr = snrSum / snrCount;
        telemetry.averageSnr = oldSamples == 0
            ? snrSum / snrCount
            : ((telemetry.averageSnr * oldSamples) + snrSum) / telemetry.snrSamples;
    }
    if (updateGst) {
        telemetry.gstSemiMajor = gstSemiMajor;
        telemetry.gstSemiMinor = gstSemiMinor;
        telemetry.gstHacc = gstHacc;
        telemetry.lastGstMs = millis();
    }
    portEXIT_CRITICAL(&telemetryMux);

    // Match the Python agent's backend publisher: forward the receiver health
    // sentences that the backend understands, while keeping unrelated GNSS
    // chatter off the raw_data topic.
    const bool backendSentence = strcmp(type, "GGA") == 0
        || strcmp(type, "GSA") == 0
        || strcmp(type, "GSV") == 0
        || strcmp(type, "GST") == 0;
    if (backendSentence) {
        String outbound = rawSentence;
        outbound.trim();
        outbound += "\r\n";
        backendAgent.enqueueNmea(reinterpret_cast<const uint8_t *>(outbound.c_str()), outbound.length());
    }
}

void resetRtcmParser() {
    rtcmLength = 0;
    rtcmExpectedLength = 0;
}

void consumeByte(uint8_t value, String &rtcmOutput) {
    if (rtcmLength > 0) {
        if (rtcmLength >= sizeof(rtcmFrame)) {
            resetRtcmParser();
        } else {
            rtcmFrame[rtcmLength++] = value;
            if (rtcmLength == 3) {
                const uint16_t payloadLength =
                    (static_cast<uint16_t>(rtcmFrame[1]) & 0x03U) * 256U + rtcmFrame[2];
                rtcmExpectedLength = static_cast<size_t>(payloadLength) + 6U;
                if (rtcmExpectedLength > sizeof(rtcmFrame)) resetRtcmParser();
            }
            if (rtcmExpectedLength > 0 && rtcmLength == rtcmExpectedLength) {
                // Forward the complete frame exactly as received. CRC and
                // message type decoding are deliberately backend concerns.
                rtcmOutput.concat(reinterpret_cast<const char *>(rtcmFrame), rtcmLength);
                backendAgent.enqueueRtcm(rtcmFrame, rtcmLength);
                noteRtcmBytes(rtcmLength);
                portENTER_CRITICAL(&telemetryMux);
                ++telemetry.rtcmFrames;
                telemetry.hasData = true;
                telemetry.lastDataMs = millis();
                portEXIT_CRITICAL(&telemetryMux);
                resetRtcmParser();
            }
        }
        return;
    }

    if (nmeaBuffer.length() > 0) {
        if (value == '\n') {
            nmeaBuffer += static_cast<char>(value);
            parseNmeaSentence(nmeaBuffer);
            nmeaBuffer = "";
        } else if (value == '$') {
            // A malformed/incomplete line should not swallow the next valid
            // sentence when the GNSS stream recovers.
            nmeaBuffer = "$";
        } else if (nmeaBuffer.length() < MAX_NMEA_LENGTH - 1) {
            nmeaBuffer += static_cast<char>(value);
        } else {
            nmeaBuffer = "";
        }
        return;
    }

    if (value == 0xD3) {
        rtcmFrame[0] = value;
        rtcmLength = 1;
        rtcmExpectedLength = 0;
    } else if (value == '$') {
        nmeaBuffer = "$";
    }
}

} // namespace

String receiveRtcmFromGnss() {
    String rtcmData;
    rtcmData.reserve(2048);
    uint8_t buffer[128] = {};

    while (Serial1.available() > 0) {
        const size_t bytesAvailable = static_cast<size_t>(Serial1.available());
        const size_t bytesToRead = min(bytesAvailable, sizeof(buffer));
        if (bytesToRead == 0) break;

        const size_t bytesRead = Serial1.readBytes(reinterpret_cast<char *>(buffer), bytesToRead);
        if (bytesRead == 0) break;
        noteInputBytes(bytesRead);
        for (size_t i = 0; i < bytesRead; ++i) consumeByte(buffer[i], rtcmData);
    }

    if (!rtcmData.isEmpty()) {
        Serial.println("[UM980] Da nhan khung RTCM opaque. So byte: " + String(rtcmData.length()));
    }
    return rtcmData;
}

RtcmMessageCounts consumeRtcmMessageCounts() {
    // Kept as a source-compatible no-op for older callers. The firmware does
    // not decode RTCM message types anymore.
    return RtcmMessageCounts{};
}

GnssTelemetrySnapshot getGnssTelemetrySnapshot() {
    GnssTelemetrySnapshot copy;
    portENTER_CRITICAL(&telemetryMux);
    copy = telemetry;
    portEXIT_CRITICAL(&telemetryMux);
    if (copy.lastGgaMs == 0 || millis() - copy.lastGgaMs > 15000UL) {
        strncpy(copy.fixStatus, "NO_FIX", sizeof(copy.fixStatus) - 1);
        copy.fixStatus[sizeof(copy.fixStatus) - 1] = '\0';
    }
    return copy;
}

uint32_t getRtcmInputBps() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&telemetryMux);
    if (inputWindowStartedMs > 0 && now - inputWindowStartedMs >= 1000UL) {
        const uint32_t elapsed = now - inputWindowStartedMs;
        inputBytesPerSecond = elapsed > 0
            ? static_cast<uint32_t>((static_cast<uint64_t>(inputWindowBytes) * 1000ULL) / elapsed)
            : inputWindowBytes;
        inputWindowStartedMs = now;
        inputWindowBytes = 0;
    }
    const uint32_t result = inputBytesPerSecond;
    portEXIT_CRITICAL(&telemetryMux);
    return result;
}
