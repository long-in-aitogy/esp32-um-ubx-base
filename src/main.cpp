#include "helper.h"
#include "functions/Backend_Agent.h"
#include "functions/RTCM_Receiver.h"
#include "functions/cmd_handler.h"

#include <time.h>
#include "esp_bt.h"

// ================= ĐỊNH NGHĨA CÁC BIẾN TOÀN CỤC =================
Preferences prefs;

extern PubSubClient mqtt;

namespace {
    constexpr uint8_t CONNECTION_FAIL_LIMIT = 5;
    constexpr uint32_t GNSS_DATA_TIMEOUT_MS = 5000;

    // The MQTT task also drives BackendAgent's WebSocket parser and command
    // dispatcher.  Four KiB is insufficient once it enters the networking and
    // JSON library call chains, and an overflow corrupts the FreeRTOS task
    // frame before the panic handler can print a useful application backtrace.
    constexpr uint32_t SERIAL_COMMAND_TASK_STACK_BYTES = 4096;
    constexpr uint32_t MQTT_TASK_STACK_BYTES = 12288;
    constexpr uint32_t NTRIP_TASK_STACK_BYTES = 6144;
    constexpr uint32_t HEALTH_TASK_STACK_BYTES = 4096;

    struct RtcmState {
        String latest;
        uint32_t lastGnssReceptionMs = 0;
        bool hasGnssReception = false;
        RtcmMessageCounts messageCounts;
    };

    class DeviceRuntime {
    public:
        inline static unsigned long lastHealthCheck = 0;
        inline static uint8_t mqttDisconnectCount = 0;
        inline static uint8_t ntripDisconnectCount = 0;
        inline static uint8_t gsmDisconnectCount = 0;
    };
    using deviceHealth = DeviceRuntime;
}

SemaphoreHandle_t rtcmBufferMutex = nullptr;
SemaphoreHandle_t tcpStreamMutex = nullptr;

void initPrefs();
static void serviceMqtt(const bool reconnect);
static void settleModemBeforeNtrip();
__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void *parameter);
__attribute__((noreturn)) void healthCheckTask([[maybe_unused]] void *parameter);
__attribute__((noreturn)) void taskMQTT([[maybe_unused]] void *parameter);
__attribute__((noreturn)) void taskSerialCommand([[maybe_unused]] void *parameter);

void setup() {
    static RtcmState rtcmState;
    Serial.begin(115200);

    Serial.println("[DEBUG] Serial initialized");
    Serial.println("\n=========================================");
    Serial.println("     ESP32 GNSS GATEWAY KHOI DONG        ");
    Serial.println("=========================================");

    // Release Bluetooth DRAM since BLE/BT is not used, returning ~30-40KB to FreeRTOS heap
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

    prefs.begin("myPrefs");
    const bool needReset = prefs.getBool("NEED_RESET", true);
    const int restartCount = prefs.getInt("RSTRT_COUNT", 0);
    prefs.putInt("RSTRT_COUNT", restartCount + 1);
    if (needReset) {
        Serial.println("[SETUP] Khoi tao Preferences lan dau tien...");
        initPrefs();
    }
    prefs.putString("WIFI_SSID", WIFI_SSID);
    prefs.putString("WIFI_PASS", WIFI_PASSWORD);
    prefs.putString("CONNECTION_TYPE", "WIFI");

    loadConnectionTypeFromPrefs();
    const int gnssTX = TX_GNSS;
    const int gnssRX = RX_GNSS;
    const int rx2ModemTX = RX_TO_MODEM_TX;
    const int tx2ModemRX = TX_TO_MODEM_RX;
    prefs.end();

    (void)restartCount;
    Serial.println("[SETUP] Khoi dong giao tiep UM980: TX=" + String(gnssTX)
                   + ", RX=" + String(gnssRX));
    Serial1.begin(GNSS_BAUD, SERIAL_8N1, static_cast<uint8_t>(gnssRX), static_cast<uint8_t>(gnssTX));
    Serial1.setTimeout(100);

    // Load the serial, token, service configuration and persistent agent state
    // before either transport starts publishing status.
    backendAgent.begin();

    backendAgent.detectGnssChip(Serial1, true);

    // Khởi động mạng
    // Keep the local console responsive even if Wi-Fi/GSM cannot connect.
    Serial.println("[SETUP] Starting Serial Command task before network setup.");
    xTaskCreatePinnedToCore(taskSerialCommand, "Serial CMD Task", SERIAL_COMMAND_TASK_STACK_BYTES,
                            nullptr, 1, nullptr, 1);

#ifndef NATIVE_BUILD
    if (isGsmConnection()) {
        SerialAT.begin(115200, SERIAL_8N1,
                       static_cast<uint8_t>(rx2ModemTX), static_cast<uint8_t>(tx2ModemRX));
        delay(500);
    }
#endif

    bool networkConnected = false;
    while (!networkConnected) {
        if (isWifiConnection()) {
            Serial.println("[SETUP] Su dung ket noi WIFI");
            networkConnected = setupWiFi();
        } else {
            Serial.println("[SETUP] Su dung ket noi SIM/GSM");
            DeviceRuntime::gsmDisconnectCount = 0;
            networkConnected = startSIM() && connectGSM();
        }
        if (networkConnected) {
            Serial.println("[SETUP] Ket noi mang thanh cong!");
            setupNTRIP();
            DeviceRuntime::ntripDisconnectCount = 0;
            if (isGsmConnection()) {
                settleModemBeforeNtrip();
            }
            connectNTRIP();
            DeviceRuntime::mqttDisconnectCount = 0;
            setupMQTT();
        } else {
            Serial.println("[ERROR] Khong the ket noi mang. Vui long kiem tra cau hinh va thu lai.");
        }
    }

    Serial.println("[SETUP] Ket noi mang thanh cong!");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    rtcmBufferMutex = xSemaphoreCreateMutex();
    while (rtcmBufferMutex == nullptr) {
        Serial.println("[ERROR] Tao mutex rtcmDataMutex that bai! Dang thu lai...");
        rtcmBufferMutex = xSemaphoreCreateMutex();
    }
    Serial.println("[SETUP] Tao mutex rtcmDataMutex thanh cong!");

    tcpStreamMutex = xSemaphoreCreateMutex();
    while (tcpStreamMutex == nullptr) {
        Serial.println("[ERROR] Tao mutex tcpStreamMutex that bai! Dang thu lai...");
        tcpStreamMutex = xSemaphoreCreateMutex();
    }
    Serial.println("[SETUP] Tao mutex tcpStreamMutex thanh cong!");

    Serial.println("[SETUP] Task MQTT: Quan ly ket noi MQTT va callback.");
    xTaskCreatePinnedToCore(taskMQTT, "MQTT Task", MQTT_TASK_STACK_BYTES, nullptr, 3, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task MQTT!");

    Serial.println("[SETUP] Task NTRIP: Gui du lieu RTCM qua NTRIP.");
    xTaskCreatePinnedToCore(taskNtrip, "NTRIP Task", NTRIP_TASK_STACK_BYTES, &rtcmState, 2, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task NTRIP!");

    Serial.println("[SETUP] Task Health: Gui thong tin suc khoe thiet bi len MQTT moi 30s");
    xTaskCreatePinnedToCore(healthCheckTask, "Health Task", HEALTH_TASK_STACK_BYTES,
                            &rtcmState, 1, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task Health!");

    Serial.println("=========================================");
    Serial.println("        KHOI DONG HOAN TAT               ");
    Serial.println("=========================================\n");
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
}

__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void *parameter) {
    auto* const rtcmState = static_cast<RtcmState*>(parameter);
    Serial.println("[NTRIP TASK] Bat dau task NTRIP...");
    int loopStatus = 0;
    String rtcmRead = "";
    while (true) {
        if (rtcmBufferMutex && xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            rtcmRead = receiveRtcmFromGnss();
            if (rtcmState) {
                rtcmState->latest = rtcmRead;
                if (!rtcmRead.isEmpty()) {
                    rtcmState->lastGnssReceptionMs = millis();
                    rtcmState->hasGnssReception = true;
                }
            }
            xSemaphoreGive(rtcmBufferMutex);
        }

        if (isGsmConnection()) {
            bool gprsConnected = false;
            if (tcpStreamMutex && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
                gprsConnected = modem.isGprsConnected();
                xSemaphoreGive(tcpStreamMutex);
            }
            if (!gprsConnected) {
                if (++DeviceRuntime::gsmDisconnectCount >= CONNECTION_FAIL_LIMIT) {
                    Serial.println("[GSM TASK][ERROR] GPRS mat ket noi qua 5 lan, khoi dong lai ESP32...");
                    shutdownTcpTransportBeforeRestart();
                    ESP.restart();
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            DeviceRuntime::gsmDisconnectCount = 0;
        }
        if (tcpStreamMutex && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            if (loopStatus == 504) {
                Serial.println("[NTRIP TASK] Dang thu ket noi lai NTRIP...");
                connectNTRIP();
            }
            loopStatus = loopNTRIP(rtcmRead);
            xSemaphoreGive(tcpStreamMutex);
            ++DeviceRuntime::ntripDisconnectCount;
            if (
                (loopStatus == 500 || loopStatus == 504 || !isNtripConnected())
                && DeviceRuntime::ntripDisconnectCount >= CONNECTION_FAIL_LIMIT
            ) {
                    Serial.println("[NTRIP TASK][ERROR] NTRIP mat ket noi qua 5 lan, khoi dong lai ESP32...");
                    shutdownTcpTransportBeforeRestart();
                    ESP.restart();
            } else {
                DeviceRuntime::ntripDisconnectCount = 0;
            }
            #if PROGRAM_DEBUG
            Serial.println("[NTRIP TASK] loopNTRIP() tra ve: " + String(loopStatus));
            #endif
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

__attribute__((noreturn)) void healthCheckTask([[maybe_unused]] void* const parameter) {
    auto* const rtcmState = static_cast<RtcmState*>(parameter);
    String healthPayload = "";
    uint32_t loopStartTime = 0;
    while (true) {
        const uint32_t loopStartTime = millis();
        int32_t signalQualityDbm = -1;
        if (rtcmBufferMutex && xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)))
        {
            if (isGsmConnection()) {
                if (tcpStreamMutex && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
                    signalQualityDbm = modem.getSignalQuality();
                    xSemaphoreGive(tcpStreamMutex);
                }
            }
            const bool gnssDataOk = rtcmState && rtcmState->hasGnssReception
                && (millis() - rtcmState->lastGnssReceptionMs <= GNSS_DATA_TIMEOUT_MS);
            const RtcmMessageCounts messageCounts = rtcmState ? rtcmState->messageCounts : RtcmMessageCounts{};
            if (rtcmState) {
                rtcmState->messageCounts = {};
            }
            healthPayload = formDeviceHealthString(signalQualityDbm, gnssDataOk,
                                                   messageCounts.values);
            xSemaphoreGive(rtcmBufferMutex);
        }
        if (!healthPayload.isEmpty()) publishHealth(healthPayload);
        const uint32_t elapsed = millis() - loopStartTime;
        vTaskDelay(pdMS_TO_TICKS(elapsed < HEALTH_INTERVAL ? HEALTH_INTERVAL - elapsed : 1));
    }
}

__attribute__((noreturn)) void taskMQTT([[maybe_unused]] void *parameter) {
    Serial.println("[MQTT TASK] Bat dau task MQTT...");
    while (true) {
        if (tcpStreamMutex && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            serviceMqtt(true);
            backendAgent.loop();
            xSemaphoreGive(tcpStreamMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

__attribute__((noreturn)) void taskSerialCommand([[maybe_unused]] void *parameter) {
    SerialCommandProcessor commandProcessor;
    Serial.println("[SERIAL COMMAND TASK] Bat dau task lang nghe Serial...");
    while (true) {
        commandProcessor.processPending();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void loop() {
    if (isGsmConnection() && tcpStreamMutex
        && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
        if (!modem.isGprsConnected()) {
            digitalWrite(LED_PIN, HIGH);
#if RTCM_COMMUNICATION_PROTOCOL == TCP_IP
            stopNtrip();
#endif
            if (connectGSM()) {
                DeviceRuntime::gsmDisconnectCount = 0;
#if RTCM_COMMUNICATION_PROTOCOL == TCP_IP
                setupNTRIP();
                connectNTRIP();
#endif
            } else {
                ++DeviceRuntime::gsmDisconnectCount;
            }
        } else {
            DeviceRuntime::gsmDisconnectCount = 0;
            digitalWrite(LED_PIN, LOW);
        }
        xSemaphoreGive(tcpStreamMutex);
    }

    if (isWifiConnection() && WiFiClass::status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);
        setupWiFi();
        digitalWrite(LED_PIN, LOW);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void settleModemBeforeNtrip() {
    Serial.println("[SETUP][NTRIP] Cho modem on dinh truoc khi bat tay NTRIP...");
    const uint32_t settleStart = millis();
    const uint32_t settleDurationMs = 600;

    while (millis() - settleStart < settleDurationMs) {
        modem.maintain();
        delay(20);
    }

    Serial.println("[SETUP][NTRIP] Modem da on dinh, bat dau ket noi NTRIP.");
}

void initPrefs() {
    prefs.clear();
    prefs.putBool("NEED_RESET", false);
    prefs.putInt("RSTRT_COUNT", 0); // chưa cấu hình được, lấy được
    prefs.putString("CONNECTION_TYPE", "WIFI");
    // Hai giá trị hợp lệ: "4G" và "WIFI". Giá trị này được nạp khi khởi động.
    prefs.putString("GNSS_MDL_TYPE", "UBLOX");

    prefs.putString("APN", APN); // cấu hình đc, chưa lấy đc
    prefs.putString("WIFI_SSID", WIFI_SSID); // cấu hình đc, lấy đc
    prefs.putString("WIFI_PASS", WIFI_PASSWORD); // cấu hình đc, lấy đc
    prefs.putString("GPRS_USER", ""); // cấu hình đc, chưa lấy đc
    prefs.putString("GPRS_PASS", ""); // cấu hình đc, chưa lấy đc
    prefs.putString("NTRIP_SERVER", NTRIP_CASTER_ADDRESS); // cấu hình được, lấy được
    prefs.putUShort("NTRIP_PORT", NTRIP_CASTER_PORT); // cấu hình được, lấy được
    prefs.putString("NTRIP_MPT", NTRIP_MOUNTPOINT); // cấu hình được, lấy được
    prefs.putString("NT_AUTH_BS", NTRIP_AUTH_BASE_STATION); // cấu hình được, lấy được

    prefs.putString("MQTT_SERVER", MQTT_SERVER); // cấu hình được, lấy được
    prefs.putUShort("MQTT_PORT", MQTT_PORT); // cấu hình được, lấy được
    prefs.putString("MQTT_USER", MQTT_USER); // cấu hình đc, lấy được
    prefs.putString("MQTT_PASS", MQTT_PASS); // cấu hình đc, lấy được

    prefs.putString("TPC_SUB_CMD", TOPIC_SUB_CMD); // cấu hình được, lấy được
    prefs.putString("TPC_HEALTH", TOPIC_PUB_HEALTH); // cấu hình đc, lấy được
}

static void serviceMqtt(const bool reconnect) {
    if (mqtt.connected()) {
        DeviceRuntime::mqttDisconnectCount = 0;
        mqtt.loop();
        return;
    }

    backendAgent.onMqttDisconnected();
    if (reconnect) {
        if (DeviceRuntime::mqttDisconnectCount < 255) ++DeviceRuntime::mqttDisconnectCount;
        connectMQTT();
    }
}
