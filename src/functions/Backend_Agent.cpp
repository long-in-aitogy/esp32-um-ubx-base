#include "functions/Backend_Agent.h"

#include "Prog_Config.h"
#include "functions/MQTT_Manager.h"
#include "functions/NTRIP_Handler_IP.h"
#include "functions/RTCM_Receiver.h"
#include "functions/Unicore_Output.h"
#include "functions/Base_Sample.h"
#include "functions/Device_Token.h"
#include "functions/Ubx_Reply.h"
#include "hardware/Connection_type.h"
#include "hardware/Sim_handler.h"
#include "hardware/Wifi_handler.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>
#include <esp_err.h>
#include <time.h>
#include <vector>
#include <ctype.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#if !defined(NATIVE_BUILD)
#include <esp_random.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#endif

extern Preferences prefs;
extern PubSubClient mqtt;
extern SemaphoreHandle_t tcpStreamMutex;
extern SemaphoreHandle_t rtcmBufferMutex;

namespace {

constexpr char BACKEND_PREFS_NAMESPACE[] = "backend";
constexpr char BACKEND_PREFS_PARTITION[] = "agent_state";
Preferences backendPrefs;
bool useDedicatedBackendPrefs = false;

constexpr size_t MAX_NMEA_PACKET = 512;
constexpr size_t MAX_RTCM_PACKET = 1030;
constexpr size_t NMEA_QUEUE_LENGTH = 16;
constexpr size_t RTCM_QUEUE_LENGTH = 4;
constexpr size_t STATUS_DOCUMENT_SIZE = 8192;
constexpr size_t CONFIG_DOCUMENT_SIZE = 4096;
constexpr size_t COMMAND_DOCUMENT_SIZE = 24576;

struct DataPacket {
    uint16_t length = 0;
    uint8_t data[MAX_RTCM_PACKET] = {};
};

struct NmeaPacket {
    uint16_t length = 0;
    uint8_t data[MAX_NMEA_PACKET] = {};
};

Preferences &backendPreferences() {
    return useDedicatedBackendPrefs ? backendPrefs : prefs;
}

bool beginBackendPreferences(bool readOnly) {
    return useDedicatedBackendPrefs
        ? backendPrefs.begin(BACKEND_PREFS_NAMESPACE, readOnly, BACKEND_PREFS_PARTITION)
        : prefs.begin("myPrefs", readOnly);
}

bool migrateBackendPreferences() {
    if (!backendPrefs.begin(BACKEND_PREFS_NAMESPACE, false, BACKEND_PREFS_PARTITION)) {
        return false;
    }
    const bool alreadyMigrated = backendPrefs.isKey("MIGRATED");
    backendPrefs.end();
    if (alreadyMigrated) return true;

    if (!prefs.begin("myPrefs", true)) return false;
    if (!backendPrefs.begin(BACKEND_PREFS_NAMESPACE, false, BACKEND_PREFS_PARTITION)) {
        prefs.end();
        return false;
    }

    auto copyString = [](const char *key) {
        if (!prefs.isKey(key)) return true;
        const String value = prefs.getString(key);
        return backendPrefs.putString(key, value) == value.length();
    };
    auto copyBool = [](const char *key) {
        if (!prefs.isKey(key)) return true;
        return backendPrefs.putBool(key, prefs.getBool(key)) == 1;
    };

    bool migrated = true;
    migrated &= copyString("DEVICE_SERIAL");
    migrated &= copyString("DEVICE_NAME");
    migrated &= copyString("SENSOR_TYPE");
    migrated &= copyBool("PROVISIONED");
    migrated &= copyBool("REMOTE_LOCK");
    if (prefs.isKey("LOCK_SERVERS")) {
        migrated &= backendPrefs.putUChar("LOCK_SERVERS", prefs.getUChar("LOCK_SERVERS")) == 1;
    }
    migrated &= copyString("BASE_JSON");
    migrated &= copyString("SERVICE_JSON");
    migrated &= copyString("LICENSE_KEY");
    migrated &= copyString("CMD_RESULT");
    migrated &= copyString("PROGRESS_JSON");
    if (migrated) migrated = backendPrefs.putBool("MIGRATED", true) == 1;
    prefs.end();
    backendPrefs.end();
    Serial.printf("[BACKEND][NVS] Migrasi state sang agent_state: %s.\n",
                  migrated ? "THANH CONG" : "THAT BAI");
    return migrated;
}

String readPreference(const char *key, const String &fallback = String()) {
    prefs.begin("myPrefs", true);
    const String value = prefs.isKey(key) ? prefs.getString(key) : fallback;
    prefs.end();
    return value;
}

bool readPreferenceBool(const char *key, bool fallback = false) {
    prefs.begin("myPrefs", true);
    const bool value = prefs.getBool(key, fallback);
    prefs.end();
    return value;
}

void writePreferenceBool(const char *key, bool value) {
    prefs.begin("myPrefs", false);
    prefs.putBool(key, value);
    prefs.end();
}

String epochString() {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
        return String(static_cast<unsigned long>(now));
    }
    // The backend uses this value for diagnostics until SNTP has completed.
    // It is replaced by a real Unix timestamp as soon as the network clock is
    // available.
    return String(static_cast<unsigned long>(millis() / 1000UL));
}

bool jsonBool(JsonObjectConst object, const String &key, bool fallback) {
    const JsonVariantConst value = object[key.c_str()];
    if (value.isNull()) {
        return fallback;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    if (value.is<const char *>()) {
        const String text = value.as<const char *>();
        if (text.equalsIgnoreCase("true") || text == "1" || text.equalsIgnoreCase("on")) {
            return true;
        }
        if (text.equalsIgnoreCase("false") || text == "0" || text.equalsIgnoreCase("off")) {
            return false;
        }
    }
    return value.as<int>() != 0;
}

String jsonString(JsonObjectConst object, const String &key, const String &fallback = String()) {
    const JsonVariantConst value = object[key.c_str()];
    if (value.isNull()) {
        return fallback;
    }
    return value.as<String>();
}

void copyJsonObject(JsonDocument &target, const char *key, const String &serialized) {
    DynamicJsonDocument source(CONFIG_DOCUMENT_SIZE);
    if (deserializeJson(source, serialized) != DeserializationError::Ok || !source.is<JsonObject>()) {
        target.createNestedObject(key);
        return;
    }
    JsonObject destination = target.createNestedObject(key);
    destination.set(source.as<JsonObjectConst>());
}

String urlEncode(const String &value) {
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    String encoded;
    encoded.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hexDigits[(c >> 4) & 0x0F];
            encoded += hexDigits[c & 0x0F];
        }
    }
    return encoded;
}

String websocketNonce() {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t nonce[16] = {};
    for (size_t i = 0; i < sizeof(nonce); i += sizeof(uint32_t)) {
#if !defined(NATIVE_BUILD)
        const uint32_t randomValue = esp_random();
#else
        const uint32_t randomValue = static_cast<uint32_t>(random(0x7FFFFFFF)) ^ micros();
#endif
        memcpy(nonce + i, &randomValue, sizeof(randomValue));
    }
    String encoded;
    encoded.reserve(24);
    for (size_t i = 0; i < sizeof(nonce); i += 3) {
        const uint32_t a = nonce[i];
        const uint32_t b = i + 1 < sizeof(nonce) ? nonce[i + 1] : 0;
        const uint32_t c = i + 2 < sizeof(nonce) ? nonce[i + 2] : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        encoded += alphabet[(value >> 18) & 0x3F];
        encoded += alphabet[(value >> 12) & 0x3F];
        encoded += i + 1 < sizeof(nonce) ? alphabet[(value >> 6) & 0x3F] : '=';
        encoded += i + 2 < sizeof(nonce) ? alphabet[value & 0x3F] : '=';
    }
    return encoded;
}

String tokenFingerprint(const String &token) {
    if (token.isEmpty()) {
        return String();
    }

#if !defined(NATIVE_BUILD)
    uint8_t digest[32] = {};
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts_ret(&context, 0) == 0
        && mbedtls_sha256_update_ret(&context,
                                     reinterpret_cast<const unsigned char *>(token.c_str()),
                                     token.length()) == 0
        && mbedtls_sha256_finish_ret(&context, digest) == 0) {
        mbedtls_sha256_free(&context);
        char result[17] = {};
        for (size_t i = 0; i < 8; ++i) {
            snprintf(result + (i * 2), 3, "%02x", digest[i]);
        }
        return String(result);
    }
    mbedtls_sha256_free(&context);
#endif

    // Native/unit-test fallback.  Hardware builds use the backend-compatible
    // SHA-256 fingerprint above.
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < token.length(); ++i) {
        hash ^= static_cast<uint8_t>(token[i]);
        hash *= 16777619UL;
    }
    char result[17] = {};
    snprintf(result, sizeof(result), "%08lx%08lx",
             static_cast<unsigned long>(hash),
             static_cast<unsigned long>(hash ^ 0xA5A5A5A5UL));
    return String(result);
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t decodeBase64(const String &encoded, uint8_t *output, size_t outputCapacity) {
    int value = 0;
    int bits = -8;
    size_t outputLength = 0;
    for (size_t i = 0; i < encoded.length(); ++i) {
        const char c = encoded[i];
        if (c == '=') {
            break;
        }
        const int decoded = base64Value(c);
        if (decoded < 0) {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
                continue;
            }
            return 0;
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            if (outputLength >= outputCapacity) {
                return 0;
            }
            output[outputLength++] = static_cast<uint8_t>((value >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return outputLength;
}

bool delayMarker(const uint8_t *data, size_t length, uint32_t &delayMs) {
    if (length < 9 || memcmp(data, "$DELAY_", 7) != 0 || data[length - 1] != '$') {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 7; i + 1 < length; ++i) {
        if (data[i] < '0' || data[i] > '9') {
            return false;
        }
        value = value * 10UL + static_cast<uint32_t>(data[i] - '0');
        if (value > 10000UL) {
            value = 10000UL;
        }
    }
    delayMs = value;
    return true;
}

String licenseCodeFromBase(const String &base) {
    const int length = static_cast<int>(base.length());
    if (length < 2 || length > 31) return String();

    std::vector<int> parts(static_cast<size_t>(length), 0);
    std::vector<double> smoothed(static_cast<size_t>(length) + 1U, 0.0);
    double number = atof(base.c_str());
    if (!std::isfinite(number)) return String();

    number *= length;
    int remainingDigits = length;
    parts[0] = static_cast<int>(number * pow(10.0, -static_cast<double>(remainingDigits)));
    for (int index = 1; index < length; ++index) {
        number -= static_cast<double>(parts[index - 1])
            / pow(10.0, -static_cast<double>(remainingDigits));
        --remainingDigits;
        parts[index] = static_cast<int>(number * pow(10.0, -static_cast<double>(remainingDigits)));
    }
    // Python keeps this final element as a float because `/ 2` is true
    // division. Truncating it here changes every generated license code.
    const double finalPart = (parts[length - 2] + parts[length - 1]) / 2.0 + 1.0;
    smoothed[0] = static_cast<double>(parts[0]);

    String output;
    output.reserve(static_cast<size_t>(length) * 2U);
    const double logarithm = log10(finalPart);
    for (int index = 0; index < length; ++index) {
        const double nextPart = index + 1 == length
            ? finalPart : static_cast<double>(parts[index + 1]);
        smoothed[index + 1] = (smoothed[index] + nextPart) / 2.0;
        const double first = smoothed[index + 1] * exp(-0.2);
        const double second = logarithm * pow(nextPart, 0.2);
        // nearbyint follows Python's round-to-nearest, ties-to-even under the
        // default floating-point rounding mode (unlike lround's ties-away).
        output += String(static_cast<long>(std::nearbyint(first + second)));
    }
    return output;
}

String licenseBaseForSerial(const String &serial) {
    const int start = serial.length() > 10
        ? static_cast<int>(serial.length() - 10U) : 0;
    String base;
    for (int index = start; index < static_cast<int>(serial.length()); ++index) {
        char character = serial[static_cast<unsigned int>(index)];
        if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        base += String(static_cast<int>(static_cast<unsigned char>(character)));
    }
    if (base.length() > 12) base = base.substring(0, 12);
    while (base.length() < 12) base += '0';
    return base;
}

bool licenseMatchesSerial(const String &serial, const String &token) {
    if (serial.isEmpty() || token.isEmpty()) return false;
    return isDeviceBearerToken(token.c_str(), token.length())
        || token == licenseCodeFromBase(licenseBaseForSerial(serial));
}

// The Python agent applies the VN2000 transformation after it has obtained a
// position from the rover's NMEA fix. These routines intentionally operate on
// coordinates only; no RTCM message or CRC is inspected here.
constexpr double WGS84_A = 6378137.0;
constexpr double WGS84_B = 6356752.31424518;
constexpr double WGS84_E2 = 1.0 - (WGS84_B * WGS84_B) / (WGS84_A * WGS84_A);
constexpr double ARCSEC_TO_RAD = 3.14159265358979323846 / (180.0 * 3600.0);

struct ProvinceProjection {
    const char *code;
    const char *name;
    double centralMeridianDeg;
    double scale;
};

constexpr ProvinceProjection VN2000_PROVINCES[] = {
    {"AG_KG", "An Giang + Kien Giang", 104.75, 0.9999},
    {"BN_BG", "Bac Ninh + Bac Giang", 107.0, 0.9999},
    {"CM_BL", "Ca Mau + Bac Lieu", 104.5, 0.9999},
    {"CAO_BANG", "Cao Bang", 105.75, 0.9999},
    {"DAKLAK_PY", "Dak Lak + Phu Yen", 108.5, 0.9999},
    {"DIEN_BIEN", "Dien Bien", 103.0, 0.9999},
    {"DNAI_BPHUOC", "Dong Nai + Binh Phuoc", 107.75, 0.9999},
    {"DTHAP_TGIANG", "Dong Thap + Tien Giang", 105.0, 0.9999},
    {"GLAI_BDINH", "Gia Lai + Binh Dinh", 108.25, 0.9999},
    {"HA_TINH", "Ha Tinh", 105.5, 0.9999},
    {"HYEN_TBINH", "Hung Yen + Thai Binh", 105.5, 0.9999},
    {"KH_HOA_NTHUAN", "Khanh Hoa + Ninh Thuan", 108.25, 0.9999},
    {"LAI_CHAU", "Lai Chau", 104.75, 0.9999},
    {"LANG_SON", "Lang Son", 107.25, 0.9999},
    {"LCAI_YBAI", "Lao Cai + Yen Bai", 104.75, 0.9999},
    {"LDONG_DNONG_BTHUAN", "Lam Dong + Dak Nong + Binh Thuan", 107.75, 0.9999},
    {"NGHE_AN", "Nghe An", 104.75, 0.9999},
    {"NBINH_HNAM_NDINH", "Ninh Binh + Ha Nam + Nam Dinh", 105.0, 0.9999},
    {"PHU_THO_VPHUC_HBINH", "Phu Tho + Vinh Phuc + Hoa Binh", 104.75, 0.9999},
    {"QNGAI_KTUM", "Quang Ngai + Kon Tum", 108.0, 0.9999},
    {"QNINH", "Quang Ninh", 107.75, 0.9999},
    {"QTRI_QBINH", "Quang Tri + Quang Binh", 106.0, 0.9999},
    {"SON_LA", "Son La", 104.0, 0.9999},
    {"TNINH_LONGAN", "Tay Ninh + Long An", 105.75, 0.9999},
    {"TNGUYEN_BKAN", "Thai Nguyen + Bac Kan", 106.5, 0.9999},
    {"THANH_HOA", "Thanh Hoa", 105.0, 0.9999},
    {"CTHO_STRANG_HGIANG", "Can Tho + Soc Trang + Hau Giang", 105.0, 0.9999},
    {"DANANG_QNAM", "Da Nang + Quang Nam", 107.75, 0.9999},
    {"HA_NOI", "Ha Noi", 105.0, 0.9999},
    {"HPHONG_HDUONG", "Hai Phong + Hai Duong", 105.75, 0.9999},
    {"HCM_BRVT_BDUONG", "Ho Chi Minh + Ba Ria Vung Tau + Binh Duong", 105.75, 0.9999},
    {"HUE", "Hue", 107.0, 0.9999},
    {"TQUANG_HGIANG", "Tuyen Quang + Ha Giang", 106.0, 0.9999},
    {"VLONG_BTRE_TVINH", "Vinh Long + Ben Tre + Tra Vinh", 105.5, 0.9999},
};

struct EcefCoordinate {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

EcefCoordinate llhToEcef(double latitudeDeg, double longitudeDeg, double height) {
    const double latitude = latitudeDeg * 3.14159265358979323846 / 180.0;
    const double longitude = longitudeDeg * 3.14159265358979323846 / 180.0;
    const double sinLatitude = sin(latitude);
    const double cosLatitude = cos(latitude);
    const double radius = WGS84_A / sqrt(1.0 - WGS84_E2 * sinLatitude * sinLatitude);
    return {
        (radius + height) * cosLatitude * cos(longitude),
        (radius + height) * cosLatitude * sin(longitude),
        (radius * (1.0 - WGS84_E2) + height) * sinLatitude,
    };
}

void ecefToLlh(const EcefCoordinate &coordinate, double &latitudeRad,
               double &longitudeRad, double &height) {
    const double ep2 = (WGS84_A * WGS84_A - WGS84_B * WGS84_B)
        / (WGS84_B * WGS84_B);
    const double p = sqrt(coordinate.x * coordinate.x + coordinate.y * coordinate.y);
    if (p < 1e-12) {
        latitudeRad = copysign(3.14159265358979323846 / 2.0, coordinate.z);
        longitudeRad = 0.0;
        height = fabs(coordinate.z) - WGS84_B;
        return;
    }
    const double theta = atan2(coordinate.z * WGS84_A, p * WGS84_B);
    const double sinTheta = sin(theta);
    const double cosTheta = cos(theta);
    latitudeRad = atan2(
        coordinate.z + ep2 * WGS84_B * sinTheta * sinTheta * sinTheta,
        p - WGS84_E2 * WGS84_A * cosTheta * cosTheta * cosTheta);
    longitudeRad = atan2(coordinate.y, coordinate.x);
    const double sinLatitude = sin(latitudeRad);
    const double radius = WGS84_A / sqrt(1.0 - WGS84_E2 * sinLatitude * sinLatitude);
    height = p / cos(latitudeRad) - radius;
}

void blhToUtm(double centralMeridianRad, double scale, double latitudeRad,
              double longitudeRad, double height, double &northing,
              double &easting) {
    const double ep2 = WGS84_E2 / (1.0 - WGS84_E2);
    const double sinLatitude = sin(latitudeRad);
    const double cosLatitude = cos(latitudeRad);
    const double tangent = tan(latitudeRad);
    const double radius = WGS84_A / sqrt(1.0 - WGS84_E2 * sinLatitude * sinLatitude);
    const double t = tangent * tangent;
    const double c = ep2 * cosLatitude * cosLatitude;
    const double a = (longitudeRad - centralMeridianRad) * cosLatitude;
    const double e4 = WGS84_E2 * WGS84_E2;
    const double e6 = e4 * WGS84_E2;
    const double meridian = WGS84_A * (
        (1.0 - WGS84_E2 / 4.0 - 3.0 * e4 / 64.0 - 5.0 * e6 / 256.0) * latitudeRad
        - (3.0 * WGS84_E2 / 8.0 + 3.0 * e4 / 32.0 + 45.0 * e6 / 1024.0) * sin(2.0 * latitudeRad)
        + (15.0 * e4 / 256.0 + 45.0 * e6 / 1024.0) * sin(4.0 * latitudeRad)
        - (35.0 * e6 / 3072.0) * sin(6.0 * latitudeRad));
    northing = scale * (meridian + radius * tangent * (
        a * a / 2.0
        + (5.0 - t + 9.0 * c + 4.0 * c * c) * pow(a, 4) / 24.0
        + (61.0 - 58.0 * t + t * t + 600.0 * c - 330.0 * ep2) * pow(a, 6) / 720.0));
    easting = 500000.0 + scale * radius * (
        a + (1.0 - t + c) * pow(a, 3) / 6.0
        + (5.0 - 18.0 * t + t * t + 72.0 * c - 58.0 * ep2) * pow(a, 5) / 120.0);
    (void)height;
}

void utmToBlh(double centralMeridianRad, double scale, double northing,
              double easting, double height, double &latitudeRad,
              double &longitudeRad) {
    const double ep2 = WGS84_E2 / (1.0 - WGS84_E2);
    const double x = northing / scale;
    const double y = (easting - 500000.0) / scale;
    const double e4 = WGS84_E2 * WGS84_E2;
    const double e6 = e4 * WGS84_E2;
    const double mu = x / (WGS84_A * (1.0 - WGS84_E2 / 4.0
        - 3.0 * e4 / 64.0 - 5.0 * e6 / 256.0));
    const double e1 = (1.0 - sqrt(1.0 - WGS84_E2))
        / (1.0 + sqrt(1.0 - WGS84_E2));
    const double j1 = 3.0 * e1 / 2.0 - 27.0 * pow(e1, 3) / 32.0;
    const double j2 = 21.0 * pow(e1, 2) / 16.0 - 55.0 * pow(e1, 4) / 32.0;
    const double j3 = 151.0 * pow(e1, 3) / 96.0;
    const double j4 = 1097.0 * pow(e1, 4) / 512.0;
    const double footpoint = mu + j1 * sin(2.0 * mu) + j2 * sin(4.0 * mu)
        + j3 * sin(6.0 * mu) + j4 * sin(8.0 * mu);
    const double sinFootpoint = sin(footpoint);
    const double cosFootpoint = cos(footpoint);
    const double tangent = tan(footpoint);
    const double c1 = ep2 * cosFootpoint * cosFootpoint;
    const double t1 = tangent * tangent;
    const double radius = WGS84_A / sqrt(1.0 - WGS84_E2 * sinFootpoint * sinFootpoint);
    const double meridianRadius = radius * (1.0 - WGS84_E2)
        / (1.0 - WGS84_E2 * sinFootpoint * sinFootpoint);
    const double d = y / radius;
    latitudeRad = footpoint - (radius * tangent / meridianRadius) * (
        d * d / 2.0
        - (5.0 + 3.0 * t1 + 10.0 * c1 - 4.0 * c1 * c1 - 9.0 * ep2) * pow(d, 4) / 24.0
        + (61.0 + 90.0 * t1 + 298.0 * c1 + 45.0 * t1 * t1
           - 252.0 * ep2 - 3.0 * c1 * c1) * pow(d, 6) / 720.0);
    longitudeRad = centralMeridianRad + (
        d - (1.0 + 2.0 * t1 + c1) * pow(d, 3) / 6.0
        + (5.0 - 2.0 * c1 + 28.0 * t1 - 3.0 * c1 * c1
           + 8.0 * ep2 + 24.0 * t1 * t1) * pow(d, 5) / 120.0) / cosFootpoint;
    (void)height;
}

bool transformItrfToVn2000(double latitudeDeg, double longitudeDeg, double height,
                           double centralMeridianDeg, double scale,
                           double &localLatitude, double &localLongitude,
                           double &localAltitude, double &northing,
                           double &easting) {
    const EcefCoordinate source = llhToEcef(latitudeDeg, longitudeDeg, height);
    const double omega = 0.00928836 * ARCSEC_TO_RAD;
    const double phi = -0.01975479 * ARCSEC_TO_RAD;
    const double epsilon = 0.00427372 * ARCSEC_TO_RAD;
    const double k = 0.999999747093722;
    const EcefCoordinate transformed = {
        191.90441429 + k * (source.x + epsilon * source.y - phi * source.z),
        39.30318279 + k * (-epsilon * source.x + source.y + omega * source.z),
        111.45032835 + k * (phi * source.x - omega * source.y + source.z),
    };

    double latitudeRad = 0.0;
    double longitudeRad = 0.0;
    double transformedHeight = 0.0;
    ecefToLlh(transformed, latitudeRad, longitudeRad, transformedHeight);
    const double centralMeridianRad = centralMeridianDeg * 3.14159265358979323846 / 180.0;
    blhToUtm(centralMeridianRad, scale, latitudeRad, longitudeRad,
             transformedHeight, northing, easting);
    utmToBlh(centralMeridianRad, scale, northing, easting, transformedHeight,
             latitudeRad, longitudeRad);

    const EcefCoordinate vn2000 = llhToEcef(
        latitudeRad * 180.0 / 3.14159265358979323846,
        longitudeRad * 180.0 / 3.14159265358979323846,
        transformedHeight);
    const double reverseOmega = -0.00928836 * ARCSEC_TO_RAD;
    const double reversePhi = 0.01975479 * ARCSEC_TO_RAD;
    const double reverseEpsilon = -0.00427372 * ARCSEC_TO_RAD;
    const double reverseK = 1.000000252906278;
    const EcefCoordinate local = {
        -191.90441429 + reverseK * (vn2000.x + reverseEpsilon * vn2000.y - reversePhi * vn2000.z),
        -39.30318279 + reverseK * (-reverseEpsilon * vn2000.x + vn2000.y + reverseOmega * vn2000.z),
        -111.45032835 + reverseK * (reversePhi * vn2000.x - reverseOmega * vn2000.y + vn2000.z),
    };
    ecefToLlh(local, latitudeRad, longitudeRad, localAltitude);
    localLatitude = latitudeRad * 180.0 / 3.14159265358979323846;
    localLongitude = longitudeRad * 180.0 / 3.14159265358979323846;
    return std::isfinite(localLatitude) && std::isfinite(localLongitude) && std::isfinite(localAltitude)
        && -90.0 <= localLatitude && localLatitude <= 90.0
        && -180.0 <= localLongitude && localLongitude <= 180.0;
}

} // namespace

// ---------------------------------------------------------------------------
// Minimal RFC6455 client.  The CORS endpoint is plain ws://, and using the
// Arduino Client abstraction lets the same code work over Wi-Fi and TinyGSM.
// ---------------------------------------------------------------------------
class BackendAgentWebsocket {
public:
    explicit BackendAgentWebsocket(BackendAgent *owner) : owner_(owner) {}

    void begin(const String &host, uint16_t port, const String &path, Client &client) {
        host_ = host;
        port_ = port;
        path_ = path;
        client_ = &client;
        nextAttemptMs_ = 0;
    }

    void loop() {
        if (!client_) {
            return;
        }
        if (!connected_) {
            if (static_cast<int32_t>(millis() - nextAttemptMs_) < 0) {
                return;
            }
            connectNow();
            return;
        }

        if (!client_->connected()) {
            disconnect(false);
            return;
        }

        if (millis() - lastPingMs_ >= 30000UL) {
            static constexpr uint8_t ping[] = {'p', 'i'};
            sendControl(0x9, ping, sizeof(ping));
            lastPingMs_ = millis();
        }

        uint8_t chunk[256];
        while (client_->available() > 0) {
            const int count = client_->read(chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            rx_.insert(rx_.end(), chunk, chunk + count);
            if (rx_.size() > 32768) {
                rx_.clear();
                disconnect(false);
                return;
            }
        }
        parseFrames();
    }

    bool connected() const { return connected_; }

    void disconnect(bool permanent) {
        if (client_) {
            client_->stop();
        }
        rx_.clear();
        connected_ = false;
        nextAttemptMs_ = millis() + (permanent ? 60000UL : 10000UL);
        if (owner_) {
            owner_->websocketConnected_ = false;
        }
    }

    bool sendText(const String &text) {
        if (!connected_ || !client_ || !client_->connected()) {
            return false;
        }

        const size_t length = text.length();
        uint8_t header[10] = {};
        size_t headerLength = 2;
        header[0] = 0x81; // FIN + text frame
        if (length < 126) {
            header[1] = static_cast<uint8_t>(0x80 | length);
        } else if (length <= 0xFFFF) {
            header[1] = 0x80 | 126;
            header[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(length & 0xFF);
            headerLength = 4;
        } else {
            header[1] = 0x80 | 127;
            for (uint8_t i = 0; i < 8; ++i) {
                header[2 + i] = static_cast<uint8_t>((length >> (56 - (i * 8))) & 0xFF);
            }
            headerLength = 10;
        }

#if !defined(NATIVE_BUILD)
        const uint32_t mask = esp_random();
#else
        const uint32_t mask = static_cast<uint32_t>(random(0x7FFFFFFF)) ^ micros();
#endif
        const uint8_t maskBytes[4] = {
            static_cast<uint8_t>((mask >> 24) & 0xFF),
            static_cast<uint8_t>((mask >> 16) & 0xFF),
            static_cast<uint8_t>((mask >> 8) & 0xFF),
            static_cast<uint8_t>(mask & 0xFF),
        };

        if (client_->write(header, headerLength) != headerLength
            || client_->write(maskBytes, sizeof(maskBytes)) != sizeof(maskBytes)) {
            disconnect(false);
            return false;
        }

        uint8_t maskedChunk[256] = {};
        for (size_t offset = 0; offset < length; offset += sizeof(maskedChunk)) {
            const size_t chunkLength = min(sizeof(maskedChunk), length - offset);
            for (size_t i = 0; i < chunkLength; ++i) {
                maskedChunk[i] = static_cast<uint8_t>(text[offset + i])
                    ^ maskBytes[(offset + i) & 3];
            }
            if (client_->write(maskedChunk, chunkLength) != chunkLength) {
                disconnect(false);
                return false;
            }
        }
        return true;
    }

private:
    BackendAgent *owner_ = nullptr;
    Client *client_ = nullptr;
    String host_;
    String path_;
    uint16_t port_ = 0;
    uint32_t nextAttemptMs_ = 0;
    uint32_t lastPingMs_ = 0;
    bool connected_ = false;
    std::vector<uint8_t> rx_;

    void connectNow() {
        nextAttemptMs_ = millis() + 10000UL;
        client_->stop();
        client_->setTimeout(3000);
        if (!client_->connect(host_.c_str(), port_)) {
            return;
        }

        // RFC6455 requires this header to decode to exactly 16 random bytes.
        const String key = websocketNonce();
        client_->print(String("GET ") + path_ + " HTTP/1.1\r\n"
                       + "Host: " + host_ + ":" + String(port_) + "\r\n"
                       + "Upgrade: websocket\r\n"
                       + "Connection: Upgrade\r\n"
                       + "Sec-WebSocket-Key: " + key + "\r\n"
                       + "Sec-WebSocket-Version: 13\r\n\r\n");

        String response;
        const uint32_t deadline = millis() + 5000UL;
        while (millis() < deadline && response.length() < 4096) {
            while (client_->available() > 0) {
                const int byte = client_->read();
                if (byte < 0) break;
                response += static_cast<char>(byte);
                if (response.endsWith("\r\n\r\n")) {
                    break;
                }
            }
            if (response.endsWith("\r\n\r\n")) {
                break;
            }
            delay(1);
        }

        if (response.indexOf(" 101 ") < 0 && response.indexOf(" 101\r") < 0) {
            client_->stop();
            return;
        }
        connected_ = true;
        nextAttemptMs_ = millis() + 10000UL;
        lastPingMs_ = millis();
        if (owner_) {
            owner_->websocketConnected_ = true;
            Serial.println("[BACKEND][WS] WebSocket da ket noi.");
            owner_->publishStatus(true);
        }
    }

    void parseFrames() {
        while (rx_.size() >= 2 && connected_) {
            const uint8_t first = rx_[0];
            const uint8_t second = rx_[1];
            const bool masked = (second & 0x80) != 0;
            uint64_t payloadLength = second & 0x7F;
            size_t headerLength = 2;

            if (payloadLength == 126) {
                if (rx_.size() < 4) return;
                payloadLength = (static_cast<uint64_t>(rx_[2]) << 8) | rx_[3];
                headerLength = 4;
            } else if (payloadLength == 127) {
                if (rx_.size() < 10) return;
                payloadLength = 0;
                for (uint8_t i = 0; i < 8; ++i) {
                    payloadLength = (payloadLength << 8) | rx_[2 + i];
                }
                headerLength = 10;
            }

            if (payloadLength > 32768) {
                disconnect(false);
                return;
            }
            if (masked) headerLength += 4;
            if (rx_.size() < headerLength + payloadLength) return;

            uint8_t mask[4] = {};
            if (masked) {
                const size_t maskOffset = headerLength - 4;
                memcpy(mask, rx_.data() + maskOffset, sizeof(mask));
            }
            const uint8_t opcode = first & 0x0F;
            String payload;
            payload.reserve(static_cast<size_t>(payloadLength));
            const size_t payloadOffset = headerLength;
            for (size_t i = 0; i < payloadLength; ++i) {
                uint8_t byte = rx_[payloadOffset + i];
                if (masked) byte ^= mask[i & 3];
                payload += static_cast<char>(byte);
            }
            rx_.erase(rx_.begin(), rx_.begin() + payloadOffset + payloadLength);

            if (opcode == 0x8) {
                sendControl(0x8, nullptr, 0);
                disconnect(false);
                return;
            }
            if (opcode == 0x9) {
                sendControl(0xA, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
            } else if (opcode == 0x1 && owner_) {
                owner_->handleWebsocketText(payload);
            }
        }
    }

    void sendControl(uint8_t opcode, const uint8_t *payload, size_t length) {
        if (!connected_ || !client_ || length > 125) return;
#if !defined(NATIVE_BUILD)
        const uint32_t mask = esp_random();
#else
        const uint32_t mask = static_cast<uint32_t>(random(0x7FFFFFFF)) ^ micros();
#endif
        const uint8_t maskBytes[4] = {
            static_cast<uint8_t>((mask >> 24) & 0xFF),
            static_cast<uint8_t>((mask >> 16) & 0xFF),
            static_cast<uint8_t>((mask >> 8) & 0xFF),
            static_cast<uint8_t>(mask & 0xFF),
        };
        const uint8_t header[2] = {
            static_cast<uint8_t>(0x80 | opcode),
            static_cast<uint8_t>(0x80 | length),
        };
        client_->write(header, sizeof(header));
        client_->write(maskBytes, sizeof(maskBytes));
        if (payload && length > 0) {
            for (size_t i = 0; i < length; ++i) {
                const uint8_t byte = payload[i] ^ maskBytes[i & 3];
                client_->write(&byte, 1);
            }
        }
    }
};

namespace {
    WiFiClient backendWebsocketWifiClient;
    TinyGsmClient backendWebsocketGsmClient(modem, 3);
}

BackendAgent backendAgent;

void BackendAgent::begin() {
    // Image verification uses shared bootloader mappings; do it once before tasks.
    firmwareSize_ = ESP.getSketchSize();
    loadPreferences();
    ensureDefaultServiceConfig();

    if (!nmeaQueue_) {
        nmeaQueue_ = xQueueCreate(NMEA_QUEUE_LENGTH, sizeof(NmeaPacket));
    }
    if (!rtcmQueue_) {
        rtcmQueue_ = xQueueCreate(RTCM_QUEUE_LENGTH, sizeof(DataPacket));
    }
    if (!websocket_) {
        websocket_ = new BackendAgentWebsocket(this);
    }
    // Configure the secondary backend channel immediately after loading the
    // persisted serial/token. The object is allocated above, so waiting for
    // `websocket_ == nullptr` in loop() would otherwise leave it unconfigured
    // forever.
    setupWebsocket();
    lastStatusMs_ = 0;
    lastControlCheckMs_ = millis();
    statusDirty_ = true;
    workflowCancelRequested_ = false;
    workflowTask_ = nullptr;
}

void BackendAgent::loadPreferences() {
    useDedicatedBackendPrefs = migrateBackendPreferences();
    if (!beginBackendPreferences(true)) {
        useDedicatedBackendPrefs = false;
        prefs.begin("myPrefs", true);
        Serial.println("[BACKEND][NVS] Khong mo duoc agent_state; dung NVS cu.");
    }
    Preferences &statePrefs = backendPreferences();

    serialNumber_ = statePrefs.getString("DEVICE_SERIAL", String());
    bool serialNeedsSave = false;
    if (serialNumber_.isEmpty()) {
        uint64_t mac = 0;
#if !defined(NATIVE_BUILD)
        mac = ESP.getEfuseMac();
#endif
        char generated[24] = {};
        snprintf(generated, sizeof(generated), "esp_%012llX",
                 static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
        serialNumber_ = String(generated);
        serialNeedsSave = true;
    } else if (serialNumber_.startsWith("LP_")) {
        // LP_ is reserved for the Windows agent; migrate existing ESP32 IDs.
        serialNumber_ = String("esp_") + serialNumber_.substring(3);
        serialNeedsSave = true;
    }

    deviceName_ = statePrefs.getString("DEVICE_NAME", "ESP32-GNSS");
    sensorType_ = statePrefs.getString("SENSOR_TYPE", "UNKNOWN");
    provisioned_ = statePrefs.getBool("PROVISIONED", false);
    stationLock_.restore(statePrefs.getBool("REMOTE_LOCK", false),
                         statePrefs.getUChar("LOCK_SERVERS", 0));
    baseConfig_ = statePrefs.getString("BASE_JSON", "{}");
    serviceConfig_ = statePrefs.getString("SERVICE_JSON", "{}");
    licenseToken_ = statePrefs.getString("LICENSE_KEY", "");
    lastCommandResult_ = statePrefs.getString("CMD_RESULT", "{}");
    autoBaseProgress_ = statePrefs.getString("PROGRESS_JSON", "{}");
    statePrefs.end();
    prefs.begin("myPrefs", true);
    const uint32_t bootCounter = static_cast<uint32_t>(prefs.getInt("RSTRT_COUNT", 0));
    prefs.end();
    if (serialNeedsSave) {
        const bool opened = beginBackendPreferences(false);
        const bool saved = opened
            && backendPreferences().putString("DEVICE_SERIAL", serialNumber_) == serialNumber_.length();
        if (opened) backendPreferences().end();
        Serial.printf("[BACKEND] Serial %s: %s\n", serialNumber_.c_str(), saved ? "da luu NVS" : "LOI luu NVS");
    }
    licenseValid_ = licenseMatchesSerial(serialNumber_, licenseToken_);
    Serial.printf("[BACKEND] Device Serial: %s\n", serialNumber_.c_str());
    Serial.printf("[BACKEND][LICENSE] NVS: %s; kiem tra: %s\n",
                  licenseToken_.isEmpty() ? "chua co key" : "da co key",
                  licenseValid_ ? "HOP LE" : "KHONG HOP LE");

    char bootId[40] = {};
    snprintf(bootId, sizeof(bootId), "%s-%08lx", serialNumber_.c_str(),
             static_cast<unsigned long>(bootCounter));
    statusBootId_ = String(bootId);
    agentState_ = normalAgentState();
}

void BackendAgent::ensureDefaultServiceConfig() {
    DynamicJsonDocument parsed(CONFIG_DOCUMENT_SIZE);
    if (deserializeJson(parsed, serviceConfig_) == DeserializationError::Ok
        && parsed.is<JsonObject>()
        && parsed.as<JsonObjectConst>().size() > 0) {
        return;
    }

    prefs.begin("myPrefs", true);
    const String host = prefs.getString("NTRIP_SERVER", String(NTRIP_CASTER_ADDRESS));
    const uint16_t port = prefs.getUShort("NTRIP_PORT", NTRIP_CASTER_PORT);
    String mountpoint = prefs.getString("NTRIP_MPT", String(NTRIP_MOUNTPOINT));
    const String password = prefs.getString("NT_AUTH_BS", String(NTRIP_AUTH_BASE_STATION));
    prefs.end();
    while (mountpoint.startsWith("/")) mountpoint.remove(0, 1);

    DynamicJsonDocument defaults(CONFIG_DOCUMENT_SIZE);
    JsonObject service = defaults.to<JsonObject>();
    service["stream_on_demand"] = false;
    service["stream_active"] = true;
    service["server1_enabled"] = !host.isEmpty();
    service["server1_stream_on_demand"] = false;
    service["server1_stream_active"] = true;
    service["ntrip_version1"] = 1;
    service["serverhost1"] = host;
    service["port1"] = port;
    service["username1"] = "source";
    service["password1"] = password;
    service["mountpoint1"] = mountpoint;
    service["server2_enabled"] = false;
    service["server2_stream_on_demand"] = false;
    service["server2_stream_active"] = false;
    serviceConfig_.clear();
    serializeJson(defaults, serviceConfig_);
    Serial.printf("[BACKEND][NVS] Default SERVICE_JSON (%u bytes): %s.\n",
                  static_cast<unsigned>(serviceConfig_.length()),
                  saveStatePreferences() ? "da luu" : "LOI luu");
}

bool BackendAgent::saveStatePreferences() {
    if (serialNumber_.isEmpty()) {
        Serial.println("[BACKEND][NVS] Refusing to save state before agent identity is loaded.");
        return false;
    }
    // Preferences' error-name table in Arduino 2.x does not cover newer NVS
    // errors. Use the native API and commit this state snapshot once.
    nvs_handle_t handle;
    const esp_err_t opened = useDedicatedBackendPrefs
        ? nvs_open_from_partition(BACKEND_PREFS_PARTITION, BACKEND_PREFS_NAMESPACE, NVS_READWRITE, &handle)
        : nvs_open("myPrefs", NVS_READWRITE, &handle);
    if (opened != ESP_OK) {
        Serial.printf("[BACKEND][NVS] Open failed: %s (0x%x)\n", esp_err_to_name(opened), opened);
        return false;
    }
    bool saved = true;
    const auto check = [&](const char *key, esp_err_t error) {
        if (error != ESP_OK) {
            Serial.printf("[BACKEND][NVS] Write %s failed: %s (0x%x)\n", key, esp_err_to_name(error), error);
            saved = false;
        }
    };
    check("DEVICE_SERIAL", nvs_set_str(handle, "DEVICE_SERIAL", serialNumber_.c_str()));
    check("DEVICE_NAME", nvs_set_str(handle, "DEVICE_NAME", deviceName_.c_str()));
    check("SENSOR_TYPE", nvs_set_str(handle, "SENSOR_TYPE", sensorType_.c_str()));
    check("PROVISIONED", nvs_set_u8(handle, "PROVISIONED", provisioned_));
    check("REMOTE_LOCK", nvs_set_u8(handle, "REMOTE_LOCK", stationLock_.all));
    check("LOCK_SERVERS", nvs_set_u8(handle, "LOCK_SERVERS", stationLock_.servers));
    check("BASE_JSON", nvs_set_str(handle, "BASE_JSON", baseConfig_.c_str()));
    check("SERVICE_JSON", nvs_set_str(handle, "SERVICE_JSON", serviceConfig_.c_str()));
    check("LICENSE_KEY", nvs_set_str(handle, "LICENSE_KEY", licenseToken_.c_str()));
    check("CMD_RESULT", nvs_set_str(handle, "CMD_RESULT", lastCommandResult_.c_str()));
    check("PROGRESS_JSON", nvs_set_str(handle, "PROGRESS_JSON", autoBaseProgress_.c_str()));
    check("commit", nvs_commit(handle));
    nvs_close(handle);
    return saved;
}

void BackendAgent::setDetectedChipIfUnknown(const String &chipType) {
    if ((sensorType_ == "UNKNOWN" || sensorType_.isEmpty()) && (chipType == "Ublox" || chipType == "Unicorecomm")) {
        sensorType_ = chipType;
        saveStatePreferences();
        Serial.printf("[GNSS] Nhan dien chip thu dong: %s (da luu NVS)\n", sensorType_.c_str());
        markStatusDirty();
    }
}

bool BackendAgent::detectGnssChip(Stream &stream, bool force) {
    if (!force && sensorType_ != "UNKNOWN" && !sensorType_.isEmpty()) {
        return true;
    }

    while (stream.available()) stream.read();

    // 1. Active probe: Unicorecomm (UM980, UM982...)
    stream.print("unlog\r\n");
    delay(30);
    while (stream.available()) stream.read();

    stream.print("version\r\n");
    String response = "";
    uint32_t startMs = millis();
    while (millis() - startMs < 500) {
        while (stream.available()) {
            char c = static_cast<char>(stream.read());
            response += c;
            if (response.length() >= 1024) break;
        }
        if (response.indexOf("UM980") >= 0 || response.indexOf("UM982") >= 0 ||
            response.indexOf("UM9") >= 0 || response.indexOf("Unicore") >= 0 ||
            response.indexOf("UB4") >= 0 || response.indexOf("#VERSION") >= 0) {
            break;
        }
        // If no data arrives within 150ms, no Unicore chip is replying
        if (millis() - startMs > 150 && response.isEmpty()) break;
        delay(10);
    }

    if (response.indexOf("UM980") >= 0 || response.indexOf("UM982") >= 0 ||
        response.indexOf("UM9") >= 0 || response.indexOf("Unicore") >= 0 ||
        response.indexOf("UB4") >= 0 || response.indexOf("#VERSION") >= 0) {
        sensorType_ = "Unicorecomm";
        saveStatePreferences();
        Serial.printf("[GNSS] => Phat hien chip: Unicorecomm (phan hoi: %s)\n",
                      response.substring(0, min((size_t)80, (size_t)response.length())).c_str());
        markStatusDirty();
        return true;
    }

    // 2. Active probe: U-blox (ZED-F9P, NEO-M8P...) via UBX-MON-VER
    while (stream.available()) stream.read();
    const uint8_t ubxMonVer[] = {0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34};
    stream.write(ubxMonVer, sizeof(ubxMonVer));

    startMs = millis();
    uint8_t ubxMatch = 0;
    bool foundUblox = false;
    while (millis() - startMs < 400) {
        while (stream.available()) {
            uint8_t b = static_cast<uint8_t>(stream.read());
            if (ubxMatch == 0 && b == 0xB5) ubxMatch = 1;
            else if (ubxMatch == 1 && b == 0x62) ubxMatch = 2;
            else if (ubxMatch == 2 && b == 0x0A) ubxMatch = 3;
            else if (ubxMatch == 3 && b == 0x04) {
                foundUblox = true;
                break;
            } else {
                ubxMatch = (b == 0xB5) ? 1 : 0;
            }
        }
        if (foundUblox) break;
        // If no bytes arrive within 150ms, no Ublox chip is replying
        if (millis() - startMs > 150 && ubxMatch == 0 && !stream.available()) break;
        delay(10);
    }

    if (foundUblox) {
        sensorType_ = "Ublox";
        saveStatePreferences();
        Serial.println("[GNSS] => Phat hien chip: Ublox (ZED-F9P qua UBX-MON-VER)");
        markStatusDirty();
        return true;
    }

    // 3. Passive scan: NMEA
    startMs = millis();
    String passiveStr = "";
    while (millis() - startMs < 300) {
        while (stream.available()) {
            uint8_t b = static_cast<uint8_t>(stream.read());
            if (b >= 32 && b <= 126) passiveStr += static_cast<char>(b);
            if (passiveStr.length() >= 512) break;
        }
        if (passiveStr.indexOf("$PUBX") >= 0) {
            sensorType_ = "Ublox";
            saveStatePreferences();
            Serial.println("[GNSS] => Phat hien chip: Ublox (qua $PUBX NMEA)");
            markStatusDirty();
            return true;
        }
        if (millis() - startMs > 150 && passiveStr.isEmpty() && !stream.available()) break;
        delay(10);
    }

    return false;
}

void BackendAgent::setAgentState(const String &state) {
    agentState_ = state;
    agentState_.toLowerCase();
    markStatusDirty();
}

String BackendAgent::normalAgentState() const {
    if (stationLock_.all) return "locked";
    if (!licenseValid_) return "awaiting_license";
    return provisioned_ ? "online" : "unprovisioned";
}

String BackendAgent::serviceConfigJson() const { return serviceConfig_; }
String BackendAgent::baseConfigJson() const { return baseConfig_; }
String BackendAgent::licenseToken() const { return licenseToken_; }

bool BackendAgent::getProvinceProjection(const String &code, double &centralMeridianDeg,
                                         double &scale, String &name) const {
    for (const ProvinceProjection &province : VN2000_PROVINCES) {
        if (code.equalsIgnoreCase(province.code)) {
            centralMeridianDeg = province.centralMeridianDeg;
            scale = province.scale;
            name = province.name;
            return true;
        }
    }
    return false;
}

bool BackendAgent::transformToVn2000(const double latitude, const double longitude,
                                     const double altitude, const double centralMeridianDeg,
                                     const double scale, double &localLatitude,
                                     double &localLongitude, double &localAltitude,
                                     double &northing, double &easting) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(altitude)
        || latitude < -90.0 || latitude > 90.0
        || longitude < -180.0 || longitude > 180.0
        || centralMeridianDeg < 100.0 || centralMeridianDeg > 112.0
        || scale < 0.9990 || scale > 1.0010) {
        return false;
    }
    return transformItrfToVn2000(latitude, longitude, altitude, centralMeridianDeg,
                                 scale, localLatitude, localLongitude, localAltitude,
                                 northing, easting);
}

void BackendAgent::setWorkflowProgress(const String &phase, const uint8_t step,
                                       const uint8_t total, const String &details) {
    DynamicJsonDocument progress(CONFIG_DOCUMENT_SIZE);
    progress["phase"] = phase;
    progress["step"] = step;
    progress["total_steps"] = total;
    progress["updated_at"] = epochString().toInt();

    if (!details.isEmpty()) {
        DynamicJsonDocument extra(4096);
        if (deserializeJson(extra, details) == DeserializationError::Ok
            && extra.is<JsonObject>()) {
            for (JsonPairConst pair : extra.as<JsonObjectConst>()) {
                progress[pair.key().c_str()] = pair.value();
            }
        }
    }
    autoBaseProgress_.clear();
    serializeJson(progress, autoBaseProgress_);
    markStatusDirty();
}

bool BackendAgent::startWorkflow(const String &kind, const String &payload) {
    if (workflowTask_ != nullptr) return false;
    workflowKind_ = kind;
    workflowPayload_ = payload;
    workflowCancelRequested_ = false;
    const BaseType_t created = xTaskCreatePinnedToCore(
        &BackendAgent::workflowTaskThunk,
        "Backend Workflow",
        12288,
        this,
        1,
        &workflowTask_,
        1);
    if (created != pdPASS) {
        workflowKind_ = String();
        workflowPayload_ = String();
        workflowTask_ = nullptr;
        return false;
    }
    return true;
}

void BackendAgent::requestWorkflowStop() {
    if (workflowTask_ != nullptr) workflowCancelRequested_ = true;
}

void BackendAgent::workflowTaskThunk(void *parameter) {
    BackendAgent *agent = static_cast<BackendAgent *>(parameter);
    if (agent) {
        agent->runWorkflow();
        agent->workflowPayload_ = String();
        agent->workflowKind_ = String();
        agent->workflowCancelRequested_ = false;
        agent->workflowTask_ = nullptr;
    }
    vTaskDelete(nullptr);
}

bool BackendAgent::restartConfiguredNtrip() {
    if (!tcpStreamMutex
        || xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS * 4)) != pdTRUE) {
        return false;
    }
    stopNtripRover();
    setupNTRIP();
    connectNTRIP();
    xSemaphoreGive(tcpStreamMutex);
    return true;
}

int BackendAgent::loopNtripRoverLocked() {
    if (!tcpStreamMutex
        || xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        // Contention is not a caster disconnect; let the workflow retry on
        // its next pass without touching a TinyGSM socket concurrently.
        return 200;
    }
    const int result = loopNtripRover();
    xSemaphoreGive(tcpStreamMutex);
    return result;
}

bool BackendAgent::stopNtripRoverLocked() {
    if (!tcpStreamMutex
        || xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS * 4)) != pdTRUE) {
        return false;
    }
    stopNtripRover();
    xSemaphoreGive(tcpStreamMutex);
    return true;
}

bool BackendAgent::writeGnssCommands(const UbxCmdBuilder::CommandList &commands) {
    if (commands.empty()) return false;
    for (const auto &command : commands) {
        uint32_t delayMs = 0;
        if (delayMarker(command.data(), command.size(), delayMs)) {
            delay(delayMs);
            continue;
        }
        if (!rtcmBufferMutex
            || xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
        const size_t written = Serial1.write(command.data(), command.size());
        Serial1.flush();
        xSemaphoreGive(rtcmBufferMutex);
        if (written != command.size()) return false;
        delay(100);
    }
    return true;
}

bool BackendAgent::applyRoverMode() {
    if (sensorType_ != "Ublox" && sensorType_ != "Unicorecomm") return false;
    Serial.printf("[AUTO BASE] Gui lenh Rover + GGA UART cho chip %s\n", sensorType_.c_str());
    if (sensorType_ == "Unicorecomm") {
        return writeGnssCommands(UbxCmdBuilder::buildRoverCommand(UbxCmdBuilder::GnssSensorType::Unicore));
    }
    if (!rtcmBufferMutex || xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
    if (!gnssSerialMutex || xSemaphoreTake(gnssSerialMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        xSemaphoreGive(rtcmBufferMutex);
        return false;
    }
    // Own both directions until ACK/readback completes; the stream task must
    // not consume the replies before the workflow can inspect them.
    auto exchange = [](const UbxCmdBuilder::Command &command, bool poll) {
        while (Serial1.available()) Serial1.read();
        if (Serial1.write(command.data(), command.size()) != command.size()) return false;
        Serial1.flush();
        UbxReply reply;
        const uint32_t started = millis();
        while (millis() - started < 1500UL) {
            while (Serial1.available()) {
                if (!reply.feed(static_cast<uint8_t>(Serial1.read()))) continue;
                if (poll && reply.frame[2] == 0x06 && reply.frame[3] == 0x71 && reply.length == 48) {
                    const uint8_t mode = reply.frame[8];
                    Serial.printf("[AUTO BASE] UBX-CFG-TMODE3 readback mode=%u (0=Rover)\n", mode);
                    return mode == 0;
                }
                if (!poll && reply.length == 10 && reply.frame[2] == 0x05
                    && (reply.frame[3] == 0x00 || reply.frame[3] == 0x01)
                    && reply.frame[6] == command[2] && reply.frame[7] == command[3]) {
                    Serial.printf("[AUTO BASE] UBX %02X/%02X: %s\n", command[2], command[3],
                                  reply.frame[3] == 1 ? "ACK" : "NAK (chip tu choi lenh)");
                    return reply.frame[3] == 1;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        Serial.printf("[AUTO BASE] UBX %02X/%02X: khong co %s tu chip\n", command[2], command[3], poll ? "readback" : "ACK");
        return false;
    };
    for (const auto &command : UbxCmdBuilder::buildRoverCommand(UbxCmdBuilder::GnssSensorType::Ublox)) {
        // Unsupported optional port keys must not block legacy CFG commands.
        exchange(command, false);
    }
    const bool roverConfirmed = exchange({0xB5, 0x62, 0x06, 0x71, 0, 0, 0x77, 0x6B}, true);
    xSemaphoreGive(gnssSerialMutex);
    xSemaphoreGive(rtcmBufferMutex);
    if (!roverConfirmed) {
        Serial.println("[AUTO BASE][ERROR] Chua xac nhan chip o Rover; dung truoc khi ket noi caster");
        return false;
    }
    const uint32_t waiting = millis();
    while (millis() - waiting < 10000UL && !workflowCancelRequested_) {
        const auto snapshot = getGnssTelemetrySnapshot();
        if (snapshot.lastGgaMs != 0 && static_cast<int32_t>(snapshot.lastGgaMs - waiting) > 0) {
            Serial.printf("[AUTO BASE] Rover da xac nhan, GGA UART: %s\n", snapshot.lastGga);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Serial.println("[AUTO BASE][ERROR] Chip o Rover nhung khong co GGA moi trong 10s");
    return false;
}

bool BackendAgent::applyBaseCoordinates(const double latitude, const double longitude,
                                        const double altitude) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(altitude)
        || latitude < -90.0 || latitude > 90.0
        || longitude < -180.0 || longitude > 180.0) {
        return false;
    }
    if (sensorType_ != "Ublox" && sensorType_ != "Unicorecomm") return false;
    UbxCmdBuilder::GnssOptions options;
    options.ports = {"UART1"};
    return writeGnssCommands(UbxCmdBuilder::buildBaseFixedLlaCommand(
        latitude, longitude, altitude, 0.01f,
        sensorType_ == "Ublox" ? UbxCmdBuilder::GnssSensorType::Ublox
                                : UbxCmdBuilder::GnssSensorType::Unicore,
        options));
}

void BackendAgent::runWorkflow() {
    DynamicJsonDocument document(COMMAND_DOCUMENT_SIZE);
    if (deserializeJson(document, workflowPayload_) != DeserializationError::Ok
        || !document.is<JsonObject>()) {
        setAgentState(normalAgentState());
        setWorkflowProgress("failed", 0, 4, "{\"reason\":\"invalid workflow payload\"}");
        saveStatePreferences();
        return;
    }

    const JsonObject payload = document.as<JsonObject>();
    const String kind = workflowKind_;
    if (kind == "auto_base") {
        runAutoBaseWorkflow(payload);
    } else if (kind == "reference_check") {
        runReferenceCheckWorkflow(payload);
    } else {
        setAgentState(normalAgentState());
        setWorkflowProgress("failed", 0, 4, "{\"reason\":\"unknown workflow\"}");
        saveStatePreferences();
    }
}

bool BackendAgent::runAutoBaseWorkflow(JsonObject payload) {
    const String host = payload["ip"] | String();
    const uint16_t port = static_cast<uint16_t>(payload["port"] | 0);
    const String username = payload["user"] | String();
    const String password = payload["password"] | String();
    const String mountpoint = payload["mountpoint"] | String();
    const uint8_t ntripVersion = static_cast<uint8_t>(payload["ntrip_version"] | 1);
    const uint32_t timeoutSeconds = constrain(
        static_cast<uint32_t>(payload["timeout"] | 3600UL), 30UL, 86400UL);
    const uint8_t fixedStreakRequired = static_cast<uint8_t>(constrain(
        static_cast<int>(payload["fixed_streak_seconds"] | 5), 1, 60));

    auto finish = [&](const bool success, const bool cancelled, const String &reason) {
        // restartConfiguredNtrip() stops the temporary rover session while
        // holding tcpStreamMutex, then restores the configured source streams.
        const bool transportRestored = restartConfiguredNtrip();
        setAgentState(normalAgentState());
        DynamicJsonDocument details(1024);
        if (!reason.isEmpty()) details["reason"] = reason;
        if (!transportRestored) details["cleanup_error"] = "could not restore configured NTRIP transport";
        String serialized;
        serializeJson(details, serialized);
        setWorkflowProgress(
            success ? "completed" : (cancelled ? "stopped" : "failed"),
            success ? 4 : 2, 4, serialized);
        Serial.printf("[AUTO BASE] KET QUA: %s; %s\n",
                      success ? "THANH CONG" : (cancelled ? "DA DUNG" : "THAT BAI"), reason.c_str());
        saveStatePreferences();
    };

    if (host.isEmpty() || username.isEmpty() || password.isEmpty() || mountpoint.isEmpty()
        || port == 0 || port > 65535) {
        finish(false, false, "missing or invalid CORS connection fields");
        return false;
    }

    setAgentState("auto_setup_rover");
    setWorkflowProgress("set_rover", 1, 4);
    if (!applyRoverMode()) {
        finish(false, false, "cannot configure GNSS rover mode");
        return false;
    }

    bool roverStarted = false;
    if (tcpStreamMutex
        && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        stopNtrip();
        roverStarted = startNtripRover(host, port, username, password, mountpoint, ntripVersion);
        xSemaphoreGive(tcpStreamMutex);
    }
    if (!roverStarted) {
        finish(false, false, "cannot connect to CORS rover mountpoint");
        return false;
    }

    setAgentState("auto_setup_wait_fix");
    setWorkflowProgress("wait_fix", 2, 4);
    const uint32_t waitStartedMs = millis();
    uint32_t nextSecondMs = waitStartedMs;
    uint8_t fixedStreak = 0;
    bool fixed = false;
    while (millis() - waitStartedMs < timeoutSeconds * 1000UL) {
        if (workflowCancelRequested_) {
            finish(false, true, "cancelled by user");
            return false;
        }
        if (loopNtripRoverLocked() == 504) {
            finish(false, false, "CORS rover connection lost");
            return false;
        }

        const uint32_t now = millis();
        if (static_cast<int32_t>(now - nextSecondMs) >= 0) {
            nextSecondMs = now + 1000UL;
            const GnssTelemetrySnapshot snapshot = getGnssTelemetrySnapshot();
            const bool usable = freshRtkSample(now, snapshot.lastGgaMs, snapshot.fixStatus);
            fixedStreak = usable ? static_cast<uint8_t>(fixedStreak + 1) : 0;
            fixed = fixedStreak >= fixedStreakRequired;
            DynamicJsonDocument details(1024);
            details["timeout"] = timeoutSeconds;
            details["elapsed"] = (now - waitStartedMs) / 1000UL;
            details["fix_status"] = snapshot.fixStatus;
            details["fixed_streak"] = fixedStreak;
            details["fixed_streak_required"] = fixedStreakRequired;
            String serialized;
            serializeJson(details, serialized);
            setWorkflowProgress("wait_fix", 2, 4, serialized);
            if (fixed) break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!fixed) {
        finish(false, false, "RTK fix timeout");
        return false;
    }

    setAgentState("auto_setup_averaging");
    const int configuredSamples = payload["samples"] | 60;
    constexpr uint16_t samplesNeeded = 60;
    constexpr uint32_t averagingSeconds = 60;
    setWorkflowProgress("averaging", 3, 4);
    std::vector<double> latitudes;
    std::vector<double> longitudes;
    std::vector<double> altitudes;
    latitudes.reserve(samplesNeeded);
    longitudes.reserve(samplesNeeded);
    altitudes.reserve(samplesNeeded);

    const uint32_t averageStartedMs = millis();
    uint32_t lastSampleGgaMs = 0;
    nextSecondMs = averageStartedMs;
    while (millis() - averageStartedMs < averagingSeconds * 1000UL
           && latitudes.size() < samplesNeeded) {
        if (workflowCancelRequested_) {
            finish(false, true, "cancelled by user");
            return false;
        }
        if (loopNtripRoverLocked() == 504) {
            finish(false, false, "CORS rover connection lost while averaging");
            return false;
        }

        const uint32_t now = millis();
        if (static_cast<int32_t>(now - nextSecondMs) >= 0) {
            nextSecondMs = now + 1000UL;
            const GnssTelemetrySnapshot snapshot = getGnssTelemetrySnapshot();
            const bool usable = freshRtkSample(now, snapshot.lastGgaMs, snapshot.fixStatus);
            if (usable && snapshot.lastGgaMs != lastSampleGgaMs
                && std::isfinite(snapshot.latitude) && std::isfinite(snapshot.longitude)
                && std::isfinite(snapshot.altitude)
                && snapshot.latitude >= -90.0 && snapshot.latitude <= 90.0
                && snapshot.longitude >= -180.0 && snapshot.longitude <= 180.0) {
                lastSampleGgaMs = snapshot.lastGgaMs;
                latitudes.push_back(snapshot.latitude);
                longitudes.push_back(snapshot.longitude);
                altitudes.push_back(snapshot.altitude);
            }
            DynamicJsonDocument details(1200);
            details["samples_collected"] = latitudes.size();
            details["samples_target"] = samplesNeeded;
            details["averaging_seconds"] = averagingSeconds;
            details["elapsed"] = (now - averageStartedMs) / 1000UL;
            details["fix_status"] = snapshot.fixStatus;
            if (configuredSamples != samplesNeeded) {
                details["requested_samples_overridden"] = configuredSamples;
            }
            if (usable) {
                JsonObject latest = details.createNestedObject("latest_coord");
                latest["lat"] = snapshot.latitude;
                latest["lon"] = snapshot.longitude;
                latest["alt"] = snapshot.altitude;
            }
            String serialized;
            serializeJson(details, serialized);
            setWorkflowProgress("averaging", 3, 4, serialized);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (latitudes.size() < 15) {
        finish(false, false, "insufficient valid RTK samples");
        return false;
    }

    double rawLatitude = 0.0;
    double rawLongitude = 0.0;
    double rawAltitude = 0.0;
    for (size_t index = 0; index < latitudes.size(); ++index) {
        rawLatitude += latitudes[index];
        rawLongitude += longitudes[index];
        rawAltitude += altitudes[index];
    }
    rawLatitude /= latitudes.size();
    rawLongitude /= longitudes.size();
    rawAltitude /= altitudes.size();

    double centralMeridianDeg = 105.0;
    double scale = 0.9999;
    String provinceName = "Khong xac dinh (fallback theo kinh do)";
    String provinceCode = "UNKNOWN";
    const String requestedProvince = payload["province_code"] | String();
    if (!requestedProvince.isEmpty()) {
        if (!getProvinceProjection(requestedProvince, centralMeridianDeg, scale, provinceName)) {
            finish(false, false, "invalid province_code");
            return false;
        }
        provinceCode = requestedProvince;
    } else if (payload["l0_deg"].isNull()) {
        static constexpr double candidates[] = {
            103.0, 104.0, 104.5, 104.75, 105.0, 105.5, 105.75,
            106.0, 106.5, 107.0, 107.25, 107.75, 108.0, 108.25, 108.5,
        };
        double bestDistance = 1e9;
        for (const double candidate : candidates) {
            const double distance = fabs(candidate - rawLongitude);
            if (distance < bestDistance) {
                bestDistance = distance;
                centralMeridianDeg = candidate;
            }
        }
    }
    if (!payload["l0_deg"].isNull()) centralMeridianDeg = payload["l0_deg"].as<double>();
    if (!payload["k0"].isNull()) scale = payload["k0"].as<double>();

    bool transformed = false;
    double finalLatitude = rawLatitude;
    double finalLongitude = rawLongitude;
    double finalAltitude = rawAltitude;
    double northing = 0.0;
    double easting = 0.0;
    const bool enableTransform = payload["enable_itrf_vn2000_transform"] | true;
    if (enableTransform) {
        transformed = transformToVn2000(rawLatitude, rawLongitude, rawAltitude,
                                         centralMeridianDeg, scale, finalLatitude,
                                         finalLongitude, finalAltitude, northing, easting);
    }

    setAgentState("auto_setup_base");
    DynamicJsonDocument baseProgress(2048);
    baseProgress["averaged_coord"]["lat"] = finalLatitude;
    baseProgress["averaged_coord"]["lon"] = finalLongitude;
    baseProgress["averaged_coord"]["alt"] = finalAltitude;
    baseProgress["sample_count"] = latitudes.size();
    baseProgress["transform_applied"] = transformed;
    baseProgress["projection_source"] = requestedProvince.isEmpty() ? "lon_fallback" : "manual_province";
    baseProgress["province_code"] = provinceCode;
    baseProgress["province_name"] = provinceName;
    baseProgress["central_meridian_deg"] = centralMeridianDeg;
    baseProgress["k0"] = scale;
    String baseProgressJson;
    serializeJson(baseProgress, baseProgressJson);
    setWorkflowProgress("set_base", 4, 4, baseProgressJson);

    if (!stopNtripRoverLocked()) {
        finish(false, false, "cannot acquire transport lock to stop CORS rover session");
        return false;
    }
    if (!applyBaseCoordinates(finalLatitude, finalLongitude, finalAltitude)) {
        finish(false, false, "cannot configure GNSS base mode");
        return false;
    }

    DynamicJsonDocument baseConfig(CONFIG_DOCUMENT_SIZE);
    JsonObject coordinates = baseConfig.createNestedObject("coords");
    coordinates["lat"] = finalLatitude;
    coordinates["lon"] = finalLongitude;
    coordinates["alt"] = finalAltitude;
    baseConfig["altitude_reference"] = "ELLIPSOID";
    baseConfig["base_setup_method"] = "AUTO_CORS";
    baseConfig["auto_configured"] = true;
    baseConfig["fixed_samples_only"] = true;
    baseConfig["itrf_vn2000_transform_applied"] = transformed;
    baseConfig["central_meridian_deg"] = centralMeridianDeg;
    baseConfig["k0"] = scale;
    baseConfig["projection_source"] = requestedProvince.isEmpty() ? "lon_fallback" : "manual_province";
    baseConfig["province_code"] = provinceCode;
    baseConfig["province_name"] = provinceName;
    baseConfig["vn2000_northing"] = northing;
    baseConfig["vn2000_easting"] = easting;
    JsonObject raw = baseConfig.createNestedObject("auto_base_raw_itrf_llh");
    raw["lat"] = rawLatitude;
    raw["lon"] = rawLongitude;
    raw["alt"] = rawAltitude;
    baseConfig_.clear();
    serializeJson(baseConfig, baseConfig_);
    saveStatePreferences();
    const bool transportRestored = restartConfiguredNtrip();
    setAgentState(normalAgentState());

    DynamicJsonDocument completed(2048);
    completed["averaged_coord"]["lat"] = finalLatitude;
    completed["averaged_coord"]["lon"] = finalLongitude;
    completed["averaged_coord"]["alt"] = finalAltitude;
    completed["raw_itrf_coord"]["lat"] = rawLatitude;
    completed["raw_itrf_coord"]["lon"] = rawLongitude;
    completed["raw_itrf_coord"]["alt"] = rawAltitude;
    completed["sample_count"] = latitudes.size();
    completed["fixed_samples_only"] = true;
    completed["transform_applied"] = transformed;
    completed["configured_ntrip_restarted"] = transportRestored;
    completed["projection_source"] = requestedProvince.isEmpty() ? "lon_fallback" : "manual_province";
    completed["province_code"] = provinceCode;
    completed["province_name"] = provinceName;
    completed["central_meridian_deg"] = centralMeridianDeg;
    completed["k0"] = scale;
    String completedJson;
    serializeJson(completed, completedJson);
    setWorkflowProgress("completed", 4, 4, completedJson);
    Serial.printf("[AUTO BASE] KET QUA: THANH CONG; samples=%u; lat=%.10f; lon=%.10f; alt=%.4f\n",
                  static_cast<unsigned>(latitudes.size()), finalLatitude, finalLongitude, finalAltitude);
    saveStatePreferences();
    return true;
}

bool BackendAgent::runReferenceCheckWorkflow(JsonObject payload) {
    JsonObject reference = payload["reference_coords"].as<JsonObject>();
    DynamicJsonDocument storedBase(CONFIG_DOCUMENT_SIZE);
    const bool baseParsed = deserializeJson(storedBase, baseConfig_) == DeserializationError::Ok
        && storedBase.is<JsonObject>();
    JsonObject storedBaseObject = storedBase.as<JsonObject>();
    JsonObject storedCoordinates = storedBaseObject["coords"].as<JsonObject>();
    if (storedCoordinates.isNull()) storedCoordinates = storedBaseObject;

    const double referenceLatitude = reference["lat"] | NAN;
    const double referenceLongitude = reference["lon"] | NAN;
    const double referenceAltitude = reference["alt"] | NAN;
    const double restoreLatitude = storedCoordinates["lat"] | NAN;
    const double restoreLongitude = storedCoordinates["lon"] | NAN;
    const double restoreAltitude = storedCoordinates["alt"] | NAN;
    const String host = payload["ip"] | String();
    const uint16_t port = static_cast<uint16_t>(payload["port"] | 0);
    const String username = payload["user"] | String();
    const String password = payload["password"] | String();
    const String mountpoint = payload["mountpoint"] | String();
    const uint32_t timeoutSeconds = constrain(
        static_cast<uint32_t>(payload["timeout"] | 300UL), 30UL, 86400UL);
    const uint16_t requestedSamples = static_cast<uint16_t>(constrain(
        static_cast<int>(payload["samples"] | 60), 5, 120));

    auto cleanup = [&]() {
        const bool roverStopped = stopNtripRoverLocked();
        if (!roverStopped) return false;
        bool baseRestored = true;
        if (std::isfinite(restoreLatitude) && std::isfinite(restoreLongitude) && std::isfinite(restoreAltitude)) {
            baseRestored = applyBaseCoordinates(restoreLatitude, restoreLongitude, restoreAltitude);
        }
        const bool transportRestored = restartConfiguredNtrip();
        return baseRestored && transportRestored;
    };
    auto finish = [&](const bool success, const bool cancelled, const String &reason) {
        const bool cleanupSucceeded = cleanup();
        const bool completed = success && cleanupSucceeded;
        const bool stopped = cancelled && cleanupSucceeded;
        setAgentState(normalAgentState());
        DynamicJsonDocument details(1024);
        if (!reason.isEmpty()) details["reason"] = reason;
        if (!cleanupSucceeded) details["cleanup_error"] = "could not safely restore base/NTRIP state";
        String serialized;
        serializeJson(details, serialized);
        setWorkflowProgress(
            completed ? "reference_check_completed" : (stopped ? "stopped" : "reference_check_failed"),
            4, 4, serialized);
        saveStatePreferences();
    };

    if (!baseParsed || reference.isNull()
        || !std::isfinite(referenceLatitude) || !std::isfinite(referenceLongitude)
        || !std::isfinite(referenceAltitude) || referenceLatitude < -90.0
        || referenceLatitude > 90.0 || referenceLongitude < -180.0
        || referenceLongitude > 180.0 || !std::isfinite(restoreLatitude)
        || !std::isfinite(restoreLongitude) || !std::isfinite(restoreAltitude)
        || host.isEmpty() || username.isEmpty() || password.isEmpty()
        || mountpoint.isEmpty() || port == 0 || port > 65535) {
        finish(false, false, "missing or invalid reference/CORS configuration");
        return false;
    }

    setAgentState("reference_check_rover");
    setWorkflowProgress("reference_check_set_rover", 1, 4);
    if (!applyRoverMode()) {
        finish(false, false, "cannot configure GNSS rover mode");
        return false;
    }

    bool roverStarted = false;
    if (tcpStreamMutex
        && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        stopNtrip();
        roverStarted = startNtripRover(host, port, username, password, mountpoint, 1);
        xSemaphoreGive(tcpStreamMutex);
    }
    if (!roverStarted) {
        finish(false, false, "cannot connect to CORS reference mountpoint");
        return false;
    }

    setAgentState("reference_check_wait_fix");
    setWorkflowProgress("reference_check_wait_fix", 2, 4);
    const uint32_t waitStartedMs = millis();
    uint32_t nextSecondMs = waitStartedMs;
    bool fixed = false;
    while (millis() - waitStartedMs < timeoutSeconds * 1000UL) {
        if (workflowCancelRequested_) {
            finish(false, true, "cancelled by user");
            return false;
        }
        if (loopNtripRoverLocked() == 504) {
            finish(false, false, "CORS reference connection lost");
            return false;
        }
        const uint32_t now = millis();
        if (static_cast<int32_t>(now - nextSecondMs) >= 0) {
            nextSecondMs = now + 1000UL;
            const GnssTelemetrySnapshot snapshot = getGnssTelemetrySnapshot();
            fixed = freshRtkSample(now, snapshot.lastGgaMs, snapshot.fixStatus);
            DynamicJsonDocument details(512);
            details["fix_status"] = snapshot.fixStatus;
            details["elapsed"] = (now - waitStartedMs) / 1000UL;
            String serialized;
            serializeJson(details, serialized);
            setWorkflowProgress("reference_check_wait_fix", 2, 4, serialized);
            if (fixed) break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!fixed) {
        finish(false, false, "RTK fix timeout");
        return false;
    }

    setAgentState("reference_check_averaging");
    setWorkflowProgress("reference_check_averaging", 3, 4);
    std::vector<double> latitudes;
    std::vector<double> longitudes;
    std::vector<double> altitudes;
    latitudes.reserve(requestedSamples);
    longitudes.reserve(requestedSamples);
    altitudes.reserve(requestedSamples);
    const uint32_t averageStartedMs = millis();
    nextSecondMs = averageStartedMs;
    const uint32_t referenceAveragingSeconds = requestedSamples > 60U ? requestedSamples : 60U;
    while (millis() - averageStartedMs < referenceAveragingSeconds * 1000UL
           && latitudes.size() < requestedSamples) {
        if (workflowCancelRequested_) {
            finish(false, true, "cancelled by user");
            return false;
        }
        if (loopNtripRoverLocked() == 504) {
            finish(false, false, "CORS reference connection lost while averaging");
            return false;
        }
        const uint32_t now = millis();
        if (static_cast<int32_t>(now - nextSecondMs) >= 0) {
            nextSecondMs = now + 1000UL;
            const GnssTelemetrySnapshot snapshot = getGnssTelemetrySnapshot();
            const bool usable = freshRtkSample(now, snapshot.lastGgaMs, snapshot.fixStatus);
            if (usable && std::isfinite(snapshot.latitude) && std::isfinite(snapshot.longitude)
                && std::isfinite(snapshot.altitude) && snapshot.latitude >= -90.0
                && snapshot.latitude <= 90.0 && snapshot.longitude >= -180.0
                && snapshot.longitude <= 180.0) {
                latitudes.push_back(snapshot.latitude);
                longitudes.push_back(snapshot.longitude);
                altitudes.push_back(snapshot.altitude);
            }
            DynamicJsonDocument details(768);
            details["samples_collected"] = latitudes.size();
            details["samples_target"] = requestedSamples;
            details["fix_status"] = snapshot.fixStatus;
            String serialized;
            serializeJson(details, serialized);
            setWorkflowProgress("reference_check_averaging", 3, 4, serialized);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (latitudes.size() < 5) {
        finish(false, false, "insufficient valid reference samples");
        return false;
    }

    double measuredLatitude = 0.0;
    double measuredLongitude = 0.0;
    double measuredAltitude = 0.0;
    for (size_t index = 0; index < latitudes.size(); ++index) {
        measuredLatitude += latitudes[index];
        measuredLongitude += longitudes[index];
        measuredAltitude += altitudes[index];
    }
    measuredLatitude /= latitudes.size();
    measuredLongitude /= longitudes.size();
    measuredAltitude /= altitudes.size();
    if (!stopNtripRoverLocked()) {
        finish(false, false, "cannot acquire transport lock to stop CORS reference session");
        return false;
    }

    const double meanLatitudeRad = (measuredLatitude + referenceLatitude)
        * 3.14159265358979323846 / 360.0;
    DynamicJsonDocument result(2048);
    result["reference_coord"]["lat"] = referenceLatitude;
    result["reference_coord"]["lon"] = referenceLongitude;
    result["reference_coord"]["alt"] = referenceAltitude;
    result["measured_coord"]["lat"] = measuredLatitude;
    result["measured_coord"]["lon"] = measuredLongitude;
    result["measured_coord"]["alt"] = measuredAltitude;
    result["delta_n_m"] = (measuredLatitude - referenceLatitude)
        * 3.14159265358979323846 / 180.0 * 6378137.0;
    result["delta_e_m"] = (measuredLongitude - referenceLongitude)
        * 3.14159265358979323846 / 180.0 * 6378137.0 * cos(meanLatitudeRad);
    result["delta_u_m"] = measuredAltitude - referenceAltitude;
    result["sample_count"] = latitudes.size();
    String resultJson;
    serializeJson(result, resultJson);

    if (!cleanup()) {
        setAgentState(normalAgentState());
        setWorkflowProgress("reference_check_failed", 4, 4,
                            "{\"reason\":\"could not safely restore base/NTRIP state\"}");
        saveStatePreferences();
        return false;
    }
    setAgentState(normalAgentState());
    setWorkflowProgress("reference_check_completed", 4, 4, resultJson);
    saveStatePreferences();
    return true;
}

String BackendAgent::statusTopic() const {
    return String("pi/devices/") + serialNumber_ + "/status";
}

String BackendAgent::rawDataTopic() const {
    return String("pi/devices/") + serialNumber_ + "/raw_data";
}

String BackendAgent::commandTopic() const {
    return String("pi/devices/") + serialNumber_ + "/command";
}

String BackendAgent::controlAckTopic() const {
    return String("pi/devices/") + serialNumber_ + "/control_ack";
}

bool BackendAgent::effectiveStreamActive(const JsonObjectConst &service,
                                         uint8_t serverId,
                                         bool failOpen) {
    if (serverId < 1 || serverId > 2) return false;
    const String prefix = String("server") + String(serverId);
    const bool enabled = jsonBool(service, prefix + "_enabled", false);
    if (!enabled) return false;
    const bool globalOnDemand = jsonBool(service, "stream_on_demand", false);
    const bool onDemand = jsonBool(service, prefix + "_stream_on_demand", globalOnDemand);
    const bool active = jsonBool(service, prefix + "_stream_active",
                                 jsonBool(service, "stream_active", !onDemand));
    return failOpen || !onDemand || active;
}

bool BackendAgent::mqttConnected() const {
    return mqttConnected_ && mqtt.connected();
}

bool BackendAgent::websocketConnected() const {
    return websocketConnected_ && websocket_ && websocket_->connected();
}

String BackendAgent::buildLwtPayload() const {
    DynamicJsonDocument doc(1024);
    doc["serial"] = serialNumber_;
    doc["status"] = "offline";
    doc["timestamp"] = epochString().toInt();
    doc["agent_timestamp"] = epochString().toInt();
    doc["status_boot_id"] = statusBootId_;
    doc["status_sequence"] = statusSequence_;
    doc["status_event"] = "lwt";
    String output;
    serializeJson(doc, output);
    return output;
}

String BackendAgent::buildStatusPayload() {
    DynamicJsonDocument doc(STATUS_DOCUMENT_SIZE);
    DynamicJsonDocument serviceDoc(CONFIG_DOCUMENT_SIZE);
    deserializeJson(serviceDoc, serviceConfig_);
    const JsonObjectConst service = serviceDoc.as<JsonObjectConst>();
    const bool server1Active = licenseValid_ && provisioned_ && !isServerLocked(1)
        && effectiveStreamActive(service, 1, controlPlaneFailOpen_);
    const bool server2Active = licenseValid_ && provisioned_ && !isServerLocked(2)
        && effectiveStreamActive(service, 2, controlPlaneFailOpen_);
    const bool anyStream = server1Active || server2Active;
    const bool onDemand = jsonBool(service, "stream_on_demand", false);

    String status = agentState_;
    if (stationLock_.all) {
        status = "locked";
    } else if (status != "configuring" && status != "rebooting"
        && status != "rebooting_for_reset" && !status.startsWith("auto_setup")
        && !status.startsWith("reference_check")) {
        if (!licenseValid_) status = "awaiting_license";
        else if (!provisioned_) status = "unprovisioned";
        else if (onDemand && !anyStream) status = "sleep";
        else status = "online";
    }

    const GnssTelemetrySnapshot gnss = getGnssTelemetrySnapshot();
    const unsigned long nowMs = millis();
    const bool gnssOk = gnss.hasData && nowMs - gnss.lastDataMs <= 15000UL;
    const int32_t signal = isWifiConnection() ? WiFi.RSSI() : modem.getSignalQuality();
    const String timestamp = epochString();

    doc["serial"] = serialNumber_;
    doc["name"] = deviceName_;
    doc["status"] = status;
    doc["version"] = AGENT_VERSION;
    doc["agent_token_fingerprint"] = tokenFingerprint(licenseToken_);
    doc["timestamp"] = timestamp.toInt();
    doc["agent_timestamp"] = timestamp.toInt();
    doc["status_boot_id"] = statusBootId_;
    doc["status_sequence"] = statusSequence_;
    doc["rtk_fix_status"] = gnss.fixStatus;
    doc["last_raw_gga"] = gnss.lastGga;
    doc["last_gga_fix_status"] = gnss.fixStatus;
    if (gnss.lastGgaMs != 0) doc["last_raw_gga_age"] = (nowMs - gnss.lastGgaMs) / 1000.0;
    else doc["last_raw_gga_age"] = nullptr;
    doc["base_mode_active"] = baseConfig_ != "{}" && baseConfig_.length() > 2;
    doc["detected_chip_type"] = sensorType_;
    doc["detected_chip_port"] = "Serial1";
    doc["detected_chip_baud"] = Serial1.baudRate();
    doc["is_provisioned"] = provisioned_;
    doc["is_locked"] = stationLock_.all;
    doc["is_synced"] = true;
    doc["is_lte"] = isGsmConnection();
    JsonObject rtcmStats = doc.createNestedObject("rtcm_stats");
    rtcmStats["frames"] = gnss.rtcmFrames;
    rtcmStats["bytes_read"] = gnss.bytesRead;
    rtcmStats["decoder"] = false;
    copyJsonObject(doc, "base_config", baseConfig_);
    copyJsonObject(doc, "service_config", serviceConfig_);
    copyJsonObject(doc, "auto_base_progress", autoBaseProgress_);
    JsonObject progress = doc["auto_base_progress"].as<JsonObject>();
    if (!progress.isNull() && progress.size() > 0) {
        if (progress["phase"] == "completed" && !(progress["fixed_samples_only"] | false)) {
            progress["phase"] = "unverified";
            progress["reason"] = "Ket qua cu chua duoc xac nhan RTK Fixed; can chay lai Auto Base";
        }
        progress["last_raw_gga"] = gnss.lastGga;
        progress["gga_fix_status"] = gnss.fixStatus;
        progress["gga_age_seconds"] = doc["last_raw_gga_age"];
        progress["rtcm_bytes"] = ntripRoverBytesReceived();
        progress["rtcm_pkts"] = ntripRoverPacketsReceived();
        JsonObject current = progress.createNestedObject("current_coord");
        current["lat"] = gnss.latitude;
        current["lon"] = gnss.longitude;
        current["alt"] = gnss.altitude;
    }

    JsonObject system = doc.createNestedObject("system_info");
    JsonObject cpu = system.createNestedObject("cpu");
    cpu["usage_percent"] = 0.0;
    cpu["frequency_mhz"] = ESP.getCpuFreqMHz();
    cpu["count"] = 2;
    JsonObject memory = system.createNestedObject("memory");
    const uint32_t heapTotal = ESP.getHeapSize();
    const uint32_t heapFree = ESP.getFreeHeap();
    const uint32_t heapUsed = (heapTotal > heapFree) ? (heapTotal - heapFree) : 0;
    memory["free_heap_bytes"] = heapFree;
    memory["total_heap_bytes"] = heapTotal;
    memory["used_bytes"] = heapUsed;
    memory["used_kb"] = heapUsed / 1024;
    memory["total_kb"] = heapTotal / 1024;
    memory["used_mb"] = static_cast<double>(heapUsed) / (1024.0 * 1024.0);
    memory["total_mb"] = static_cast<double>(heapTotal) / (1024.0 * 1024.0);
    memory["percent"] = heapTotal > 0
        ? round((100.0 - (static_cast<double>(heapFree) * 100.0 / heapTotal)) * 10.0) / 10.0
        : 0.0;

    JsonObject disk = system.createNestedObject("disk");
    const uint32_t flashTotal = ESP.getFlashChipSize();
    const uint32_t flashUsed = firmwareSize_;
    const uint32_t flashFree = (flashTotal > flashUsed) ? (flashTotal - flashUsed) : 0;
    disk["type"] = "Flash";
    disk["total_bytes"] = flashTotal;
    disk["free_bytes"] = flashFree;
    disk["used_bytes"] = flashUsed;
    disk["total_mb"] = round((static_cast<double>(flashTotal) / (1024.0 * 1024.0)) * 10.0) / 10.0;
    disk["used_mb"] = round((static_cast<double>(flashUsed) / (1024.0 * 1024.0)) * 10.0) / 10.0;
    disk["total_gb"] = static_cast<double>(flashTotal) / (1024.0 * 1024.0 * 1024.0);
    disk["used_gb"] = static_cast<double>(flashUsed) / (1024.0 * 1024.0 * 1024.0);
    disk["percent"] = flashTotal > 0 ? (round((static_cast<double>(flashUsed) * 1000.0 / flashTotal)) / 10.0) : 0.0;
    system["uptime_seconds"] = millis() / 1000UL;
    system["timestamp"] = timestamp.toInt();
    system["auth_license_bypass"] = false;
    JsonObject deviceLock = system.createNestedObject("device_lock");
    deviceLock["scoped"] = true;
    deviceLock["all"] = stationLock_.all;
    JsonArray lockedServers = deviceLock.createNestedArray("locked_servers");
    for (uint8_t id = 1; id <= 2; ++id) {
        if (isServerLocked(id)) lockedServers.add(id);
    }

    JsonObject nmeaHealth = doc.createNestedObject("nmea_health");
    JsonObject typeCounts = nmeaHealth.createNestedObject("type_counts");
    typeCounts["GGA"] = gnss.ggaCount;
    typeCounts["GSA"] = gnss.gsaCount;
    typeCounts["GSV"] = gnss.gsvCount;
    typeCounts["GST"] = gnss.gstCount;
    nmeaHealth["gsv_snr_samples"] = gnss.snrSamples;
    if (gnss.lastGsvMs > 0 && nowMs - gnss.lastGsvMs <= 15000UL) {
        nmeaHealth["gsv_snr_samples"] = gnss.latestGsvSamples;
        nmeaHealth["avg_snr"] = gnss.latestAverageSnr;
        nmeaHealth["seconds_since_gsv"] = (nowMs - gnss.lastGsvMs) / 1000.0;
    } else if (gnss.snrSamples > 0) {
        nmeaHealth["avg_snr"] = gnss.averageSnr;
    }
    if (gnss.lastGstMs > 0 && nowMs - gnss.lastGstMs <= 15000UL) {
        nmeaHealth["gst_semi_major_sigma"] = gnss.gstSemiMajor;
        nmeaHealth["gst_semi_minor_sigma"] = gnss.gstSemiMinor;
        nmeaHealth["hacc"] = gnss.gstHacc;
        nmeaHealth["hacc_source"] = "gst_lat_lon_sigma";
        nmeaHealth["seconds_since_gst"] = (nowMs - gnss.lastGstMs) / 1000.0;
    }

    JsonObject position = doc.createNestedObject("position");
    position["latitude"] = gnss.latitude;
    position["longitude"] = gnss.longitude;
    position["altitude"] = gnss.altitude;

    doc["rtk_fix_status"] = gnss.fixStatus;
    if (gnss.satellites > 0) doc["satellite_count"] = gnss.satellites;
    if (gnss.gstHacc > 0 && gnss.lastGstMs > 0
        && nowMs - gnss.lastGstMs <= 15000UL) {
        doc["hacc"] = gnss.gstHacc;
    } else if (gnss.hdop > 0) {
        doc["hacc"] = gnss.hdop;
    }

    JsonObject gnssStats = doc.createNestedObject("gnss_stats");
    gnssStats["rtcm_packets"] = gnss.rtcmFrames;
    gnssStats["nmea_packets"] = gnss.nmeaSentences;
    gnssStats["bytes_read"] = gnss.bytesRead;
    gnssStats["seconds_since_data"] = gnss.hasData
        ? (nowMs - gnss.lastDataMs) / 1000UL : 0;
    JsonObject parserDebug = gnssStats.createNestedObject("parser_debug");
    parserDebug["rtcm_valid_packets"] = gnss.rtcmFrames;
    parserDebug["rtcm_decoder"] = false;
    parserDebug["nmea_valid_packets"] = gnss.nmeaSentences;

    JsonObject ntripStats = doc.createNestedObject("ntrip_stats");
    ntripStats["rtcm_input_bps"] = getRtcmInputBps();
    ntripStats["server1_bps"] = ntripServerBps(1);
    ntripStats["server2_bps"] = ntripServerBps(2);
    ntripStats["server1_connected"] = ntripServerConnected(1);
    ntripStats["server2_connected"] = ntripServerConnected(2);
    doc["bps"] = ntripServerBps(1) + ntripServerBps(2);

    JsonObject ntripStatus = doc.createNestedObject("ntrip_status");
    JsonObject server1 = ntripStatus.createNestedObject("server1");
    server1["connected"] = ntripServerConnected(1);
    server1["bps"] = ntripServerBps(1);
    server1["active"] = server1Active;
    JsonObject server2 = ntripStatus.createNestedObject("server2");
    server2["connected"] = ntripServerConnected(2);
    server2["bps"] = ntripServerBps(2);
    server2["active"] = server2Active;
    ntripStatus["rtcm_input_bps"] = getRtcmInputBps();
    doc["ntrip_connected"] = ntripServerConnected(1) || ntripServerConnected(2);

    JsonObject streamState = doc.createNestedObject("ntrip_stream_state");
    streamState["selected_mountpoint"] = jsonString(service, "active_mountpoint");
    JsonObject stream1 = streamState.createNestedObject("server1");
    const bool server1OnDemand = jsonBool(service, "server1_stream_on_demand",
                                          jsonBool(service, "stream_on_demand", false));
    const bool server1Enabled = jsonBool(service, "server1_enabled", false);
    const bool server1Configured = licenseValid_ && provisioned_
        && BackendAgent::effectiveStreamActive(service, 1, false);
    stream1["enabled"] = server1Enabled;
    stream1["on_demand"] = server1OnDemand;
    stream1["active"] = server1Active;
    stream1["can_push"] = server1Active;
    stream1["configured_can_push"] = server1Configured;
    stream1["effective_can_push"] = server1Active;
    stream1["locked"] = isServerLocked(1);
    stream1["fail_open_forced"] = server1Active && controlPlaneFailOpen_ && !server1Configured;
    stream1["sleep"] = server1Enabled && server1OnDemand && !server1Active;
    stream1["connected"] = ntripServerConnected(1);
    stream1["bps"] = ntripServerBps(1);
    stream1["mountpoint"] = jsonString(service, "mountpoint1");
    JsonObject stream2 = streamState.createNestedObject("server2");
    const bool server2OnDemand = jsonBool(service, "server2_stream_on_demand",
                                          jsonBool(service, "stream_on_demand", false));
    const bool server2Enabled = jsonBool(service, "server2_enabled", false);
    const bool server2Configured = licenseValid_ && provisioned_
        && BackendAgent::effectiveStreamActive(service, 2, false);
    stream2["enabled"] = server2Enabled;
    stream2["on_demand"] = server2OnDemand;
    stream2["active"] = server2Active;
    stream2["can_push"] = server2Active;
    stream2["configured_can_push"] = server2Configured;
    stream2["effective_can_push"] = server2Active;
    stream2["locked"] = isServerLocked(2);
    stream2["fail_open_forced"] = server2Active && controlPlaneFailOpen_ && !server2Configured;
    stream2["sleep"] = server2Enabled && server2OnDemand && !server2Active;
    stream2["connected"] = ntripServerConnected(2);
    stream2["bps"] = ntripServerBps(2);
    stream2["mountpoint"] = jsonString(service, "mountpoint2");

    JsonObject antenna = doc.createNestedObject("antenna_status");
    antenna["state"] = gnssOk ? "OK" : "FAULT";
    if (!gnssOk) antenna["message"] = "Không nhận được dữ liệu GNSS trong 15 giây";

    JsonObject transport = doc.createNestedObject("transport_status");
    transport["mqtt_connected"] = mqttConnected();
    transport["websocket_connected"] = websocketConnected();
    transport["active_channel"] = websocketConnected() ? "websocket" : (mqttConnected() ? "mqtt" : "none");
    transport["mqtt_broker"] = readPreference("MQTT_SERVER", MQTT_SERVER);
    transport["websocket_host"] = readPreference("BACKEND_HOST", BACKEND_HOST);
    transport["control_plane_fail_open"] = controlPlaneFailOpen_;

    copyJsonObject(doc, "last_command_result", lastCommandResult_);
    JsonObject control = doc.createNestedObject("control_plane_failover");
    control["active"] = controlPlaneFailOpen_;
    control["stale_after_seconds"] = BACKEND_CONTROL_STALE_MS / 1000UL;
    control["recovery_after_seconds"] = BACKEND_CONTROL_RECOVERY_MS / 1000UL;
    JsonObject ackSequences = control.createNestedObject("last_ack_sequence");
    ackSequences["mqtt"] = lastMqttAckSequence_;
    ackSequences["websocket"] = lastWebsocketAckSequence_;

    doc["connected_via"] = isWifiConnection() ? "WIFI" : "GSM";
    doc["rssi_dbm"] = signal;
    doc["free_heap_bytes"] = ESP.getFreeHeap();
    doc["uptime_s"] = millis() / 1000UL;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

void BackendAgent::publishStatus(bool force) {
    if (serialNumber_.isEmpty()) return;
    if (statusDirty_) force = true;
    if (!force && millis() - lastStatusMs_ < BACKEND_STATUS_INTERVAL_MS) {
        return;
    }
    statusDirty_ = false;
    if (statusSequence_ == 0) controlPlaneArmedMs_ = millis();
    lastStatusMs_ = millis();
    ++statusSequence_;
    const String payload = buildStatusPayload();
    if (mqttConnected()) {
        const bool sent = mqtt.publish(statusTopic().c_str(), payload.c_str(), true);
        if (!sent || !statusPublishConfirmed_) {
            Serial.printf("[BACKEND][MQTT] STATUS %s topic=%s bytes=%u\n",
                          sent ? "SENT" : "FAILED", statusTopic().c_str(),
                          static_cast<unsigned>(payload.length()));
        }
        statusPublishConfirmed_ = sent;
    }
    if (websocketConnected()) {
        DynamicJsonDocument message(STATUS_DOCUMENT_SIZE + 512);
        message["type"] = "status_update";
        DynamicJsonDocument status(STATUS_DOCUMENT_SIZE);
        if (deserializeJson(status, payload) == DeserializationError::Ok) {
            message["payload"].set(status.as<JsonObjectConst>());
            String websocketPayload;
            serializeJson(message, websocketPayload);
            websocket_->sendText(websocketPayload);
        }
    }
}

String BackendAgent::lwtPayload() const {
    return buildLwtPayload();
}

void BackendAgent::onMqttConnected() {
    mqttConnected_ = true;
    statusPublishConfirmed_ = false;
    Serial.println("[BACKEND][MQTT] Kenh MQTT san sang nhan lenh.");
    publishStatus(true);
}

void BackendAgent::onMqttDisconnected() {
    mqttConnected_ = false;
}

void BackendAgent::enqueueNmea(const uint8_t *data, size_t length) {
    if (!nmeaQueue_ || !data || length == 0) return;
    NmeaPacket packet;
    packet.length = static_cast<uint16_t>(min(length, MAX_NMEA_PACKET - 1));
    memcpy(packet.data, data, packet.length);
    packet.data[packet.length] = 0;
    if (xQueueSend(nmeaQueue_, &packet, 0) != pdTRUE) {
        NmeaPacket discarded;
        xQueueReceive(nmeaQueue_, &discarded, 0);
        xQueueSend(nmeaQueue_, &packet, 0);
    }
}

void BackendAgent::enqueueRtcm(const uint8_t *data, size_t length) {
#if BACKEND_PUBLISH_RAW_RTCM
    if (!rtcmQueue_ || !data || length == 0 || length > MAX_RTCM_PACKET) return;
    DataPacket packet;
    packet.length = static_cast<uint16_t>(length);
    memcpy(packet.data, data, packet.length);
    if (xQueueSend(rtcmQueue_, &packet, 0) != pdTRUE) {
        DataPacket discarded;
        xQueueReceive(rtcmQueue_, &discarded, 0);
        xQueueSend(rtcmQueue_, &packet, 0);
    }
#else
    (void)data;
    (void)length;
#endif
}

void BackendAgent::drainDataQueues() {
    NmeaPacket nmea;
    while (nmeaQueue_ && xQueueReceive(nmeaQueue_, &nmea, 0) == pdTRUE) {
        bool delivered = false;
        if (mqttConnected()) {
            delivered = mqtt.publish(rawDataTopic().c_str(), nmea.data, nmea.length);
        }
        if (!delivered && websocketConnected()) {
            DynamicJsonDocument message(1024);
            message["type"] = "nmea_update";
            message["payload"] = reinterpret_cast<const char *>(nmea.data);
            String serialized;
            serializeJson(message, serialized);
            delivered = websocket_->sendText(serialized);
        }
        if (!delivered) {
            // Preserve telemetry across a short transport outage; the queue
            // has a bounded size and enqueueNmea() drops the oldest item when
            // it is full.
            xQueueSendToFront(nmeaQueue_, &nmea, 0);
            break;
        }
    }

#if BACKEND_PUBLISH_RAW_RTCM
    DataPacket rtcm;
    while (rtcmQueue_ && xQueueReceive(rtcmQueue_, &rtcm, 0) == pdTRUE) {
        if (mqttConnected()) {
            if (!mqtt.publish(rawDataTopic().c_str(), rtcm.data, rtcm.length)) {
                xQueueSendToFront(rtcmQueue_, &rtcm, 0);
                break;
            }
        } else {
            xQueueSendToFront(rtcmQueue_, &rtcm, 0);
            break;
        }
    }
#endif
}

void BackendAgent::setupWebsocket() {
#if BACKEND_WEBSOCKET_ENABLED
    if (!websocket_) return;
    const String host = readPreference("BACKEND_HOST", BACKEND_HOST);
    prefs.begin("myPrefs", true);
    const uint16_t port = prefs.getUShort("BACKEND_PORT", BACKEND_WEBSOCKET_PORT);
    prefs.end();
    const String path = String("/ws/pi/") + serialNumber_
        + (licenseToken_.isEmpty() ? String() : String("?token=") + urlEncode(licenseToken_));
    if (isWifiConnection()) {
        websocket_->begin(host, port, path, backendWebsocketWifiClient);
    } else {
        websocket_->begin(host, port, path, backendWebsocketGsmClient);
    }
#endif
}

void BackendAgent::loopWebsocket() {
#if BACKEND_WEBSOCKET_ENABLED
    if (websocket_) websocket_->loop();
#endif
}

void BackendAgent::evaluateControlPlaneFailover() {
    if (millis() - lastControlCheckMs_ < 2000UL) return;
    lastControlCheckMs_ = millis();
    if (!licenseValid_ || !provisioned_) {
        controlPlaneFailOpen_ = false;
        recoveryStartedMs_ = 0;
        return;
    }
    if (statusSequence_ <= 0) return;

    const uint32_t now = millis();
    const bool mqttStale = lastMqttAckMs_ == 0
        ? now - controlPlaneArmedMs_ >= BACKEND_CONTROL_STALE_MS
        : now - lastMqttAckMs_ >= BACKEND_CONTROL_STALE_MS;
    const bool websocketStale = lastWebsocketAckMs_ == 0
        ? now - controlPlaneArmedMs_ >= BACKEND_CONTROL_STALE_MS
        : now - lastWebsocketAckMs_ >= BACKEND_CONTROL_STALE_MS;

    if (!controlPlaneFailOpen_ && mqttStale && websocketStale) {
        controlPlaneFailOpen_ = true;
        failOpenEntrySequence_ = statusSequence_;
        recoveryStartedMs_ = 0;
        Serial.println("[BACKEND][FAIL-SAFE] MQTT va WebSocket ACK deu stale; mo stream RTCM.");
        setupNTRIP();
        connectNTRIP();
        return;
    }

    if (controlPlaneFailOpen_) {
        const bool freshAck = (lastMqttAckSequence_ > failOpenEntrySequence_)
            || (lastWebsocketAckSequence_ > failOpenEntrySequence_);
        if (freshAck && (!mqttStale || !websocketStale)) {
            if (recoveryStartedMs_ == 0) recoveryStartedMs_ = now;
            if (now - recoveryStartedMs_ >= BACKEND_CONTROL_RECOVERY_MS) {
                controlPlaneFailOpen_ = false;
                recoveryStartedMs_ = 0;
                Serial.println("[BACKEND][FAIL-SAFE] Backend ACK da tro lai; phuc hoi stream config.");
                setupNTRIP();
                connectNTRIP();
            }
        } else {
            recoveryStartedMs_ = 0;
        }
    }
}

void BackendAgent::loop() {
    if (!websocket_) setupWebsocket();
    loopWebsocket();
    drainDataQueues();
    evaluateControlPlaneFailover();

    // If GNSS chip is not yet identified (e.g. user hasn't plugged it in yet at boot),
    // retry probing Serial1 every 8 seconds until detected
    if (sensorType_ == "UNKNOWN" || sensorType_.isEmpty()) {
        const uint32_t now = millis();
        if (now - lastChipProbeMs_ >= 8000UL && !workflowActive()) {
            lastChipProbeMs_ = now;
            detectGnssChip(Serial1);
        }
    }

    publishStatus(false);
}

void BackendAgent::handleControlAck(const String &source, const String &message, bool retained) {
    if (retained) return;
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, message) != DeserializationError::Ok) return;
    if (doc["type"] != "control_plane_ack") return;
    if (doc["status_boot_id"].as<String>() != statusBootId_) return;
    const int32_t sequence = doc["status_sequence"] | -1;
    if (sequence < 0) return;
    if (source == "mqtt") {
        if (sequence > lastMqttAckSequence_) {
            lastMqttAckSequence_ = sequence;
            lastMqttAckMs_ = millis();
        }
    } else {
        if (sequence > lastWebsocketAckSequence_) {
            lastWebsocketAckSequence_ = sequence;
            lastWebsocketAckMs_ = millis();
        }
    }
}

void BackendAgent::onMqttMessage(const char *topic, const uint8_t *payload, size_t length) {
    if (!topic || !payload || length == 0) return;
    const String topicString(topic);
    String message;
    message.reserve(length + 1);
    for (size_t i = 0; i < length; ++i) message += static_cast<char>(payload[i]);

    if (topicString == controlAckTopic()) {
        handleControlAck("mqtt", message, false);
        return;
    }
    if (topicString == commandTopic()
        || topicString == String("pi/devices/") + serialNumber_ + "/commands"
        || topicString == String("pi/device/") + serialNumber_ + "/command") {
        handleCommand("mqtt", message);
    }
}

void BackendAgent::handleWebsocketText(const String &message) {
    DynamicJsonDocument envelope(1024);
    if (deserializeJson(envelope, message) != DeserializationError::Ok) return;
    if (envelope["type"] == "control_plane_ack") {
        handleControlAck("websocket", message, false);
        return;
    }
    handleCommand("websocket", message);
}

void BackendAgent::recordCommandResult(const String &command, const String &source,
                                       const String &status, const String &detail,
                                       const String &commandId) {
    DynamicJsonDocument doc(1024);
    doc["command"] = command;
    doc["source"] = source;
    doc["status"] = status;
    doc["detail"] = detail.substring(0, 300);
    doc["command_id"] = commandId;
    doc["timestamp"] = epochString().toInt();
    lastCommandResult_.clear();
    serializeJson(doc, lastCommandResult_);
    const bool saved = saveStatePreferences();
    Serial.printf("[BACKEND][CMD] %s id=%s source=%s status=%s; NVS=%s\n",
                  command.c_str(), commandId.c_str(), source.c_str(), status.c_str(),
                  saved ? "OK" : "LOI");
}

void BackendAgent::publishConfigState(const char *kind) {
    if (!mqttConnected()) return;
    DynamicJsonDocument doc(CONFIG_DOCUMENT_SIZE);
    doc["serial"] = serialNumber_;
    doc["timestamp"] = epochString().toInt();
    doc["type"] = kind;
    if (strcmp(kind, "base_config_state") == 0) {
        copyJsonObject(doc, "config", baseConfig_);
    } else {
        copyJsonObject(doc, "config", serviceConfig_);
    }
    String output;
    serializeJson(doc, output);
    const String topic = String("pi/devices/") + serialNumber_ + "/" + kind;
    mqtt.publish(topic.c_str(), output.c_str(), true);
}

void BackendAgent::applyServiceStreamCommand(JsonObject payload) {
    DynamicJsonDocument serviceDoc(CONFIG_DOCUMENT_SIZE);
    if (deserializeJson(serviceDoc, serviceConfig_) != DeserializationError::Ok
        || !serviceDoc.is<JsonObject>()) {
        serviceDoc.to<JsonObject>();
    }
    JsonObject service = serviceDoc.as<JsonObject>();
    const bool active = payload["active"] | false;
    String mountpoint = payload["mountpoint"] | String();
    String address = payload["address"] | String();
    if (address.isEmpty()) address = payload["caster_host"] | String();
    if (address.isEmpty()) address = payload["ip"] | String();
    if (address.isEmpty()) address = payload["host"] | String();
    mountpoint.trim();
    while (mountpoint.startsWith("/")) mountpoint.remove(0, 1);
    mountpoint.toLowerCase();
    address.trim();
    address.toLowerCase();
    int serverId = payload["server_id"] | 0;
    if (serverId != 1 && serverId != 2) {
        serverId = payload["server_idx"] | 0;
    }
    if (serverId != 1 && serverId != 2) {
        serverId = payload["server"] | 0;
    }

    auto setServerState = [&](const uint8_t id) {
        const String prefix = String("server") + String(id);
        const bool onDemand = jsonBool(
            service,
            prefix + "_stream_on_demand",
            jsonBool(service, "stream_on_demand", false));
        // An explicit stop must not disable an Always-On server, matching the
        // dashboard/backend stream-state helper.
        if (!active && !onDemand) return;
        service[(prefix + "_stream_active").c_str()] = active;
        if (active && mountpoint.length() > 0) {
            service["active_mountpoint"] = mountpoint;
        } else if (!active) {
            const String configured = jsonString(service, String("mountpoint") + String(id));
            if (mountpoint.isEmpty() || mountpoint.equalsIgnoreCase(configured)
                || jsonString(service, "active_mountpoint").equalsIgnoreCase(configured)) {
                service.remove("active_mountpoint");
            }
        }
    };

    bool targetedServer = false;
    if (serverId == 1 || serverId == 2) {
        setServerState(static_cast<uint8_t>(serverId));
        targetedServer = true;
    } else if (address.length() > 0) {
        for (uint8_t id = 1; id <= 2; ++id) {
            const String prefix = String("server") + String(id);
            String configured = jsonString(service, prefix + "_host",
                                           jsonString(service, String("serverhost") + String(id)));
            configured.trim();
            configured.toLowerCase();
            if (!configured.isEmpty()
                && (address.indexOf(configured) >= 0 || configured.indexOf(address) >= 0)) {
                setServerState(id);
                targetedServer = true;
            }
        }
    } else if (mountpoint.length() > 0) {
        for (uint8_t id = 1; id <= 2; ++id) {
            const String prefix = String("server") + String(id);
            String configured = jsonString(service, String("mountpoint") + String(id));
            while (configured.startsWith("/")) configured.remove(0, 1);
            configured.toLowerCase();
            if (!configured.isEmpty() && configured == mountpoint) {
                setServerState(id);
                targetedServer = true;
            }
        }
    } else if (!active) {
        for (uint8_t id = 1; id <= 2; ++id) {
            setServerState(id);
            targetedServer = true;
        }
    }

    if (!targetedServer) {
        service["stream_active"] = active;
        if (mountpoint.length() > 0 && active) service["active_mountpoint"] = mountpoint;
        if (!active) service.remove("active_mountpoint");
    }
    service["stream_active"] = BackendAgent::effectiveStreamActive(service, 1, false)
        || BackendAgent::effectiveStreamActive(service, 2, false);
    serviceConfig_.clear();
    serializeJson(serviceDoc, serviceConfig_);
    saveStatePreferences();
}

bool BackendAgent::executeRawCommands(JsonArray commands, String &error, const String &sensorType) {
    if (commands.isNull() || commands.size() == 0) {
        error = "missing payload.commands_b64";
        return false;
    }
    if (commands.size() > 200) {
        error = "too many raw commands";
        return false;
    }
    uint8_t decoded[3072] = {};
    for (JsonVariant item : commands) {
        const String encoded = item.as<String>();
        if (encoded.isEmpty() || encoded.length() > 4096) {
            error = "invalid raw command item";
            return false;
        }
        const size_t length = decodeBase64(encoded, decoded, sizeof(decoded));
        if (length == 0) {
            error = "raw command is not valid base64";
            return false;
        }
        uint32_t delayMs = 0;
        if (delayMarker(decoded, length, delayMs)) {
            delay(delayMs);
            continue;
        }
        if (sensorType == "Unicorecomm") routeUnicoreOutput(decoded, length, GNSS_UNICORE_PORT);
        else if (sensorType == "Ublox") UbxCmdBuilder::routeUbloxOutput(decoded, length);
        if (rtcmBufferMutex && xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            Serial1.write(decoded, length);
            Serial1.flush();
            xSemaphoreGive(rtcmBufferMutex);
        } else {
            error = "GNSS serial is busy";
            return false;
        }
        delay(50);
    }
    return true;
}

void BackendAgent::handleCommand(const String &source, const String &message) {
    Serial.printf("[BACKEND][RX] Nhan goi lenh qua %s (%u bytes).\n",
                  source.c_str(), static_cast<unsigned>(message.length()));
    DynamicJsonDocument doc(COMMAND_DOCUMENT_SIZE);
    if (deserializeJson(doc, message) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        recordCommandResult("UNKNOWN", source, "rejected", "command envelope must be an object", "");
        publishStatus(true);
        return;
    }

    const String command = doc["command"] | String();
    const JsonObject payload = doc["payload"].as<JsonObject>();
    String commandId = doc["command_id"] | String();
    if (commandId.isEmpty() && !payload.isNull()) commandId = payload["command_id"] | String();
    if (commandId.isEmpty()) commandId = String("esp32-") + String(millis());
    Serial.printf("[BACKEND][CMD] Nhan command=%s id=%s qua %s.\n",
                  command.c_str(), commandId.c_str(), source.c_str());
    if (command.isEmpty()) {
        recordCommandResult("UNKNOWN", source, "rejected", "missing command", commandId);
        publishStatus(true);
        return;
    }

    const bool allowed = command == "LOCK_DEVICE" || command == "UNLOCK_DEVICE"
        || command == "PROVISION_DEVICE" || command == "DEPLOY_LICENSE"
        || command == "TRIGGER_AUTO_BASE" || command == "TRIGGER_BASE_REFERENCE_CHECK"
        || command == "STOP_AUTO_BASE" || command == "APPLY_VN2000_PROVINCE"
        || command == "EXECUTE_RAW_COMMANDS" || command == "DEPLOY_SERVICE_CONFIG"
        || command == "SET_RTCM_STREAM_ACTIVE" || command == "DELETE_DEVICE"
        || command == "CHECK_BASE_STATUS" || command == "REBOOT_DEVICE";
    if (!allowed) {
        recordCommandResult(command, source, "rejected", "unsupported command", commandId);
        publishStatus(true);
        return;
    }
    if (stationLock_.all && command != "LOCK_DEVICE" && command != "UNLOCK_DEVICE") {
        recordCommandResult(command, source, "rejected", "device is locked", commandId);
        publishStatus(true);
        return;
    }
    if (!licenseValid_ && command != "LOCK_DEVICE" && command != "UNLOCK_DEVICE"
        && command != "PROVISION_DEVICE" && command != "DEPLOY_LICENSE") {
        recordCommandResult(command, source, "rejected", "license is missing or invalid", commandId);
        publishStatus(true);
        return;
    }

    recordCommandResult(command, source, "running", "", commandId);
    setAgentState("configuring");
    String error;
    bool success = true;
    String detail;
    bool restartAfterCommand = false;

    if (command == "LOCK_DEVICE") {
        if (payload.containsKey("locked_servers")) {
            JsonArrayConst ids = payload["locked_servers"].as<JsonArrayConst>();
            int serverIds[2] = {};
            size_t count = 0;
            if (ids.isNull()) {
                success = false;
            } else {
                for (JsonVariantConst id : ids) {
                    if (!id.is<int>() || id.as<int>() < 1 || id.as<int>() > 2) {
                        success = false;
                        break;
                    }
                    const int serverId = id.as<int>();
                    if (count == 0 || serverIds[0] != serverId) {
                        if (count < 2) serverIds[count++] = serverId;
                    }
                }
            }
            if (success) success = stationLock_.setScoped(serverIds, count);
            if (!success) error = "payload.locked_servers must contain only server IDs 1 or 2";
        } else {
            requestWorkflowStop();
            stationLock_.all = true;
            stationLock_.servers = 0;
        }
        if (success) {
            setAgentState(normalAgentState());
            // loopNTRIP applies the policy per endpoint. Rebuilding all sockets
            // here would interrupt the other host during a scoped lock.
            detail = "device lock updated";
        }
    } else if (command == "UNLOCK_DEVICE") {
        stationLock_.restore(false, 0);
        setAgentState(normalAgentState());
        detail = "device unlocked";
    } else if (command == "PROVISION_DEVICE") {
        const String name = payload["name"] | String();
        if (name.isEmpty()) {
            success = false;
            error = "missing payload.name";
        } else {
            deviceName_ = name;
            provisioned_ = true;
            setupNTRIP();
            connectNTRIP();
            detail = "device provisioned";
        }
    } else if (command == "DEPLOY_LICENSE") {
        const String license = payload["license_key"] | String();
        if (license.isEmpty()) {
            Serial.println("[BACKEND][LICENSE] Lenh DEPLOY_LICENSE khong co license_key.");
            success = false;
            error = "missing payload.license_key";
        } else if (!licenseMatchesSerial(serialNumber_, license)) {
            success = false;
            error = "invalid device token or legacy license";
        } else {
            Serial.println("[BACKEND][LICENSE] Da nhan license_key (khong in noi dung key).");
            const String previousToken = licenseToken_;
            const bool previousValid = licenseValid_;
            licenseToken_ = license;
            licenseValid_ = true;
            const bool saved = saveStatePreferences();
            Serial.printf("[BACKEND][LICENSE] Kiem tra: %s; luu NVS: %s.\n",
                          licenseValid_ ? "HOP LE" : "KHONG HOP LE",
                          saved ? "THANH CONG" : "THAT BAI");
            if (!saved) {
                licenseToken_ = previousToken;
                licenseValid_ = previousValid;
                success = false;
                error = "cannot persist device token";
            } else {
                detail = "license deployed; rebooting";
                restartAfterCommand = true;
                setAgentState("rebooting");
                if (websocket_) {
                    websocket_->disconnect(false);
                    setupWebsocket();
                }
            }
        }
    } else if (command == "DEPLOY_SERVICE_CONFIG") {
        if (payload.isNull() || payload.size() == 0) {
            success = false;
            error = "missing service config payload";
        } else {
            DynamicJsonDocument serviceDoc(CONFIG_DOCUMENT_SIZE);
            serviceDoc.set(payload);
            JsonObject service = serviceDoc.as<JsonObject>();
            const bool onDemand = jsonBool(service, "stream_on_demand", false);
            if (!onDemand) service["stream_active"] = true;
            else if (service["stream_active"].isNull()) service["stream_active"] = false;
            const bool globalActive = jsonBool(service, "stream_active", false);
            for (uint8_t id = 1; id <= 2; ++id) {
                const String prefix = String("server") + String(id);
                const String onDemandKey = prefix + "_stream_on_demand";
                const String activeKey = prefix + "_stream_active";
                const bool serverOnDemand = jsonBool(service, onDemandKey, onDemand);
                service[onDemandKey.c_str()] = serverOnDemand;
                if (!serverOnDemand) service[activeKey.c_str()] = true;
                else if (service[activeKey.c_str()].isNull()) {
                    service[activeKey.c_str()] = globalActive;
                }
            }
            serviceConfig_.clear();
            serializeJson(serviceDoc, serviceConfig_);
            detail = "service config deployed";
            setupNTRIP();
            connectNTRIP();
            publishConfigState("service_config_state");
        }
    } else if (command == "SET_RTCM_STREAM_ACTIVE") {
        if (!payload.containsKey("active")) {
            success = false;
            error = "missing payload.active";
        } else {
            applyServiceStreamCommand(payload);
            setupNTRIP();
            connectNTRIP();
            detail = (payload["active"] | false) ? "stream activated" : "stream set to standby";
        }
    } else if (command == "EXECUTE_RAW_COMMANDS") {
        JsonArray commands = payload["commands_b64"].as<JsonArray>();
        const JsonObject originalConfig = doc["original_config"].as<JsonObject>();
        const bool configuringBase = String(originalConfig["mode"] | "") == "BASE";
        const String sensorType = originalConfig["sensor_type"] | sensorType_;
        if (workflowActive()) {
            requestWorkflowStop();
            const uint32_t stopWaitMs = millis();
            while (workflowActive() && static_cast<uint32_t>(millis() - stopWaitMs) < 1500UL) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (workflowActive()) {
            success = false;
            error = "cannot stop active base workflow";
        } else if (configuringBase && sensorType != "Ublox" && sensorType != "Unicorecomm") {
            success = false;
            error = "unsupported GNSS sensor_type";
        } else {
            success = executeRawCommands(commands, error, configuringBase ? sensorType : String());
        }
        detail = success ? "raw commands executed" : error;
        if (success) {
            if (configuringBase) {
                sensorType_ = sensorType;
                autoBaseProgress_.clear();
                setAgentState(normalAgentState());
            }
            if (!originalConfig.isNull() && originalConfig.containsKey("params")) {
                DynamicJsonDocument baseDoc(CONFIG_DOCUMENT_SIZE);
                baseDoc.set(originalConfig["params"]);
                JsonObject base = baseDoc.as<JsonObject>();
                if (originalConfig.containsKey("mode")) base["mode"] = originalConfig["mode"];
                baseConfig_.clear();
                serializeJson(baseDoc, baseConfig_);
                publishConfigState("base_config_state");
            }
        }
    } else if (command == "CHECK_BASE_STATUS") {
        if (sensorType_ == "Ublox") {
            // UBX-CFG-TMODE3 poll (the stream reader owns incoming bytes).
            const UbxCmdBuilder::Command poll = {0xB5, 0x62, 0x06, 0x71, 0x00, 0x00, 0x77, 0x6B};
            success = writeGnssCommands({poll});
            if (success) detail = "base status requested from GNSS";
            else error = "GNSS serial is busy";
        } else if (sensorType_ == "Unicorecomm" && rtcmBufferMutex
            && xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
            Serial1.print("mode\r\n");
            delay(250);
            Serial1.print("log\r\n");
            xSemaphoreGive(rtcmBufferMutex);
            detail = "base status requested from GNSS";
        } else {
            success = false;
            error = "GNSS serial is busy";
        }
    } else if (command == "TRIGGER_AUTO_BASE") {
        const String sensorType = payload["sensor_type"] | sensorType_;
        const int requestedPort = payload["port"] | 0;
        const bool hasConnection = !String(payload["ip"] | String()).isEmpty()
            && !String(payload["user"] | String()).isEmpty()
            && !String(payload["password"] | String()).isEmpty()
            && !String(payload["mountpoint"] | String()).isEmpty()
            && requestedPort >= 1 && requestedPort <= 65535;
        if (workflowActive()) {
            success = false;
            error = "another base workflow is already running";
        } else if (sensorType != "Ublox" && sensorType != "Unicorecomm") {
            success = false;
            error = "select Ublox or Unicorecomm in CORS before starting auto base";
        } else if (!hasConnection) {
            success = false;
            error = "missing or invalid CORS connection fields";
        } else {
            sensorType_ = sensorType;
            String workflowPayload;
            serializeJson(payload, workflowPayload);
            if (!startWorkflow("auto_base", workflowPayload)) {
                success = false;
                error = "cannot start auto base workflow";
            } else {
                DynamicJsonDocument progress(CONFIG_DOCUMENT_SIZE);
                progress["ntrip_host"] = payload["ip"];
                progress["ntrip_port"] = payload["port"];
                progress["mountpoint"] = payload["mountpoint"];
                progress["note"] = "ESP32 forwards opaque RTCM and uses GNSS RTK/GGA; no RTCM decoder is running on the ESP32.";
                String progressJson;
                serializeJson(progress, progressJson);
                setAgentState("auto_setup_queued");
                setWorkflowProgress("queued", 0, 4, progressJson);
                detail = "auto base workflow queued";
            }
        }
    } else if (command == "TRIGGER_BASE_REFERENCE_CHECK") {
        const JsonObject reference = payload["reference_coords"].as<JsonObject>();
        const int requestedPort = payload["port"] | 0;
        const bool validReference = !reference.isNull()
            && !reference["lat"].isNull() && !reference["lon"].isNull()
            && !reference["alt"].isNull();
        const bool hasConnection = !String(payload["ip"] | String()).isEmpty()
            && !String(payload["user"] | String()).isEmpty()
            && !String(payload["password"] | String()).isEmpty()
            && !String(payload["mountpoint"] | String()).isEmpty()
            && requestedPort >= 1 && requestedPort <= 65535;
        if (workflowActive()) {
            success = false;
            error = "another base workflow is already running";
        } else if (!validReference || !hasConnection) {
            success = false;
            error = "missing or invalid reference/CORS configuration";
        } else {
            String workflowPayload;
            serializeJson(payload, workflowPayload);
            if (!startWorkflow("reference_check", workflowPayload)) {
                success = false;
                error = "cannot start reference check workflow";
            } else {
                setAgentState("reference_check_queued");
                setWorkflowProgress("reference_check_queued", 0, 4);
                detail = "base reference check queued";
            }
        }
    } else if (command == "STOP_AUTO_BASE") {
        if (workflowActive()) {
            requestWorkflowStop();
            setWorkflowProgress("stopping", 0, 4);
            detail = "base workflow stop requested";
        } else {
            setWorkflowProgress("stopped", 0, 4);
                setAgentState(normalAgentState());
            detail = "auto base stopped";
        }
    } else if (command == "APPLY_VN2000_PROVINCE") {
        String province = payload["province_code"] | String();
        province.toUpperCase();
        double centralMeridianDeg = 0.0;
        double scale = 0.0;
        String provinceName;
        DynamicJsonDocument baseDoc(CONFIG_DOCUMENT_SIZE);
        const bool baseParsed = deserializeJson(baseDoc, baseConfig_) == DeserializationError::Ok
            && baseDoc.is<JsonObject>();
        JsonObject base = baseDoc.as<JsonObject>();
        JsonObject rawCoordinates = base["auto_base_raw_itrf_llh"].as<JsonObject>();
        if (rawCoordinates.isNull()) rawCoordinates = base["coords"].as<JsonObject>();
        if (rawCoordinates.isNull()) rawCoordinates = base;
        const double rawLatitude = rawCoordinates["lat"] | NAN;
        const double rawLongitude = rawCoordinates["lon"] | NAN;
        const double rawAltitude = rawCoordinates["alt"] | NAN;
        double localLatitude = rawLatitude;
        double localLongitude = rawLongitude;
        double localAltitude = rawAltitude;
        double northing = 0.0;
        double easting = 0.0;
        if (province.isEmpty()) {
            success = false;
            error = "missing payload.province_code";
        } else if (!getProvinceProjection(province, centralMeridianDeg, scale, provinceName)) {
            success = false;
            error = "invalid province_code";
        } else if (!baseParsed || !std::isfinite(rawLatitude) || !std::isfinite(rawLongitude)
                   || !std::isfinite(rawAltitude)
                   || !transformToVn2000(rawLatitude, rawLongitude, rawAltitude,
                                         centralMeridianDeg, scale, localLatitude,
                                         localLongitude, localAltitude, northing, easting)) {
            success = false;
            error = "base coordinates are missing or cannot be transformed";
        } else if (!applyBaseCoordinates(localLatitude, localLongitude, localAltitude)) {
            success = false;
            error = "cannot apply transformed coordinates to GNSS";
        } else {
            JsonObject coordinates = base["coords"].as<JsonObject>();
            if (coordinates.isNull()) coordinates = base.createNestedObject("coords");
            coordinates["lat"] = localLatitude;
            coordinates["lon"] = localLongitude;
            coordinates["alt"] = localAltitude;
            base["itrf_vn2000_transform_applied"] = true;
            base["central_meridian_deg"] = centralMeridianDeg;
            base["k0"] = scale;
            base["province_code"] = province;
            base["province_name"] = provinceName;
            base["projection_source"] = "manual_province_override";
            base["vn2000_northing"] = northing;
            base["vn2000_easting"] = easting;
            baseConfig_.clear();
            serializeJson(baseDoc, baseConfig_);
            saveStatePreferences();
            publishConfigState("base_config_state");
            setWorkflowProgress("reprojected", 4, 4);
            detail = "VN2000 province transform applied";
        }
    } else if (command == "REBOOT_DEVICE") {
        requestWorkflowStop();
        setAgentState("rebooting");
        recordCommandResult(command, source, "success", "device reboot requested", commandId);
        saveStatePreferences();
        publishStatus(true);
        delay(500);
        mqtt.disconnect();
        ESP.restart();
    } else if (command == "DELETE_DEVICE") {
        requestWorkflowStop();
        setAgentState("rebooting_for_reset");
        recordCommandResult(command, source, "success", "device reset requested", commandId);
        saveStatePreferences();
        publishStatus(true);
        delay(500);
        prefs.begin("myPrefs", false);
        prefs.clear();
        prefs.end();
        if (beginBackendPreferences(false)) {
            backendPreferences().clear();
            backendPreferences().end();
        }
        mqtt.disconnect();
        ESP.restart();
    }

    if (success) {
        const bool workflowState = command == "TRIGGER_AUTO_BASE"
            || command == "TRIGGER_BASE_REFERENCE_CHECK"
            || command == "STOP_AUTO_BASE" && workflowActive();
        if (command != "REBOOT_DEVICE" && command != "DELETE_DEVICE"
            && command != "DEPLOY_LICENSE" && !workflowState) {
            setAgentState(normalAgentState());
        }
        recordCommandResult(command, source, "success", detail, commandId);
    } else {
        setAgentState(normalAgentState());
        recordCommandResult(command, source, "error", error, commandId);
    }
    saveStatePreferences();
    publishStatus(true);
    if (success && restartAfterCommand) {
        delay(500);
        mqtt.disconnect();
        ESP.restart();
    }
}

void BackendAgent::publishTextFallback(const String &message) {
    if (websocketConnected()) websocket_->sendText(message);
}

bool backendDeviceLocked(uint8_t serverId) { return serverId == 0 ? backendAgent.isLocked() : backendAgent.isServerLocked(serverId); }
bool backendControlPlaneFailOpen() { return backendAgent.controlPlaneFailOpen(); }
bool backendDeviceProvisioned() { return backendAgent.isProvisioned(); }
bool backendLicenseValid() { return backendAgent.licenseValid(); }
