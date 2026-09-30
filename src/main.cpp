#include "helper.h"
#include "functions/RTCM_Receiver.h"
#include "functions/cmd_handler.h"

// ================= ĐỊNH NGHĨA CÁC BIẾN TOÀN CỤC =================
Preferences prefs;

extern PubSubClient mqtt;

namespace {
    inline constexpr uint8_t CONNECTION_FAIL_LIMIT = 5;
    inline constexpr uint32_t GNSS_DATA_TIMEOUT_MS = 5000;

    struct RtcmState {
        String latest;
        uint32_t lastGnssReceptionMs = 0;
        bool hasGnssReception = false;
        RtcmMessageCounts messageCounts;
    };

    class deviceHealth {
    public:
        inline static unsigned long lastHealthCheck = 0;
        inline static uint8_t mqttDisconnectCount = 0;
        inline static uint8_t ntripDisconnectCount = 0;
        inline static uint8_t gsmDisconnectCount = 0;
    };
}

// Semaphore
SemaphoreHandle_t rtcmBufferMutex = nullptr;
SemaphoreHandle_t tcpStreamMutex = nullptr;

/* ===================== NGUYÊN MẪU HÀM ======================== */

void initPrefs();
static void serviceMqtt(const bool reconnect);
static void settleModemBeforeNtrip();

__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void* const parameter);
__attribute__((noreturn)) void healthCheckTask([[maybe_unused]] void* const parameter);
__attribute__((noreturn)) void taskMQTT([[maybe_unused]] void* const parameter);
__attribute__((noreturn)) void taskSerialCommand([[maybe_unused]] void* const parameter);

/* ==================SETUP VÀ LOOP======================== */

void setup()
{
    static RtcmState rtcmState;

    Serial.begin(115200);
    #if BOARD_HELTEC
    Mcu.begin(HELTEC_BOARD,SLOW_CLK_TPYE);
    #endif
    delay(100);

    // Debug marker: confirm Serial is working immediately after begin()
    Serial.println("[DEBUG] Serial initialized");
    Serial.println("\n=========================================");
    Serial.println("     ESP32 GNSS GATEWAY KHOI DONG        ");
    Serial.println("=========================================");

    delay(1000);

    // Khởi tạo Preferences
    prefs.begin("myPrefs"); // false: read/write mode
    bool needReset = prefs.getBool("NEED_RESET", true);
    int restartCount = prefs.getInt("RSTRT_COUNT", 0);
    prefs.putInt("RSTRT_COUNT", restartCount + 1);
    if (needReset) {
        Serial.println("[SETUP] Khoi tao Preferences lan dau tien...");
        initPrefs();
    }
    else {
        Serial.println("[SETUP] Preferences da duoc khoi tao truoc do, khong can khoi tao lai.");
    }

    loadConnectionTypeFromPrefs();
    const int gnssTX = TX_GNSS;
    const int gnssRX = RX_GNSS;
    const int rx2ModemTX = RX_TO_MODEM_TX;
    const int tx2ModemRX = TX_TO_MODEM_RX;
    prefs.end();

    // Khởi tạo giao tiếp với UM980
    #if PROGRAM_DEBUG
    Serial.println("[SETUP] Khoi dong giao tiep voi UM980: TX=" + String(gnssTX) + ", RX=" + String(gnssRX));
    #endif
    
    Serial1.begin(GNSS_BAUD, SERIAL_8N1, (uint8_t)gnssRX, (uint8_t)gnssTX);
    Serial1.setTimeout(100);

    if (needReset) {
        Serial.println("[SETUP] Cau hinh UM980 lan dau tien...");
        bootstrapUM980();
        Serial.println("[SETUP] Da cau hinh lai tat ca ve mac dinh, khoi dong lai de ap dung cau hinh.");
        ESP.restart(); // Khởi động lại để áp dụng cấu hình mới
    } else {
        Serial.println("[SETUP] Da cau hinh GNSS chip, khong can cau hinh lai.");
    }

    // Khởi động mạng
    bool networkConnected = false;

    #ifndef NATIVE_BUILD
    if (isGsmConnection()) {
        SerialAT.begin(115200, SERIAL_8N1, (uint8_t)rx2ModemTX, (uint8_t)tx2ModemRX);
        delay(500);
    }
    #endif

    while (!networkConnected) {
        if (isWifiConnection()) {
            Serial.println("[SETUP] Su dung ket noi WIFI");
            networkConnected = setupWiFi();
        } else {
            Serial.println("[SETUP] Su dung ket noi SIM/GSM");
            deviceHealth::gsmDisconnectCount = 0;
            if (startSIM() && connectGSM()) {
                networkConnected = true;
            }
        }
        if (networkConnected) {
            Serial.println("[SETUP] Ket noi mang thanh cong!");
            setupNTRIP();
            deviceHealth::ntripDisconnectCount = 0;
            if (isGsmConnection()) {
                settleModemBeforeNtrip();
            }
            connectNTRIP();
            deviceHealth::mqttDisconnectCount = 0;
            setupMQTT();
        } else {
            Serial.println("[ERROR] Khong the ket noi mang. Vui long kiem tra cau hinh va thu lai.");
        }
    }

    Serial.println("[SETUP] Khoi dong cac task...");

    Serial.println("[Setup] Tao mutex de dong bo hoa tai nguyen chung");
    
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
    xTaskCreatePinnedToCore(taskMQTT, "MQTT Task", 4096, nullptr, 3, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task MQTT!");

    Serial.println("[SETUP] Task Serial Command: Lang nghe lenh tu Serial moi 500 ms.");
    xTaskCreatePinnedToCore(taskSerialCommand, "Serial CMD Task", 3072, nullptr, 1, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task Serial Command!");

    Serial.println("[SETUP] Task NTRIP: Gui du lieu RTCM qua NTRIP.");
    xTaskCreatePinnedToCore(taskNtrip, "NTRIP Task", 4096, &rtcmState, 2, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task NTRIP!");


    Serial.println("[SETUP] Task Health: Gui thong tin suc khoe thiet bi len MQTT moi 30s");
    xTaskCreatePinnedToCore(healthCheckTask, "Health Task", 4096, &rtcmState, 1, nullptr, 1);
    Serial.println("[SETUP] Da khoi dong Task Health!");

    Serial.println("=========================================");
    Serial.println("        KHOI DONG HOAN TAT               ");
    Serial.println("=========================================\n");

    digitalWrite(LED_PIN, HIGH);

    delay(1000);

    digitalWrite(LED_PIN, LOW);
}

__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void* const parameter) {
    auto* const rtcmState = static_cast<RtcmState*>(parameter);
    Serial.println("[NTRIP TASK] Bat dau task NTRIP...");
    int loopStatus = 0;
    String rtcmRead = "";
    while (true) {
        rtcmRead = receiveRtcmFromGnss();
        if (xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)))
        {
            rtcmState->latest = rtcmRead;
            const RtcmMessageCounts receivedCounts = consumeRtcmMessageCounts();
            for (size_t i = 0; i < RTCM_SUPPORTED_MESSAGE_TYPE_COUNT; ++i) {
                rtcmState->messageCounts.values[i] += receivedCounts.values[i];
            }
            if (!rtcmRead.isEmpty()) {
                rtcmState->lastGnssReceptionMs = millis();
                rtcmState->hasGnssReception = true;
            }
            xSemaphoreGive(rtcmBufferMutex);
        }

        if (isGsmConnection()) {
            bool gprsConnected = false;
            if (xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
                gprsConnected = modem.isGprsConnected();
                xSemaphoreGive(tcpStreamMutex);
            }
            if (!gprsConnected) {
                if (++deviceHealth::gsmDisconnectCount >= CONNECTION_FAIL_LIMIT) {
                    Serial.println("[GSM TASK][ERROR] GPRS mat ket noi qua 5 lan, khoi dong lai ESP32...");
                    shutdownTcpTransportBeforeRestart();
                    ESP.restart();
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            deviceHealth::gsmDisconnectCount = 0;
        }
        if (xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            if (loopStatus == 504) {
                Serial.println("[NTRIP TASK] Dang thu ket noi lai NTRIP...");
                connectNTRIP();
            }
            loopStatus = loopNTRIP(rtcmRead);
            xSemaphoreGive(tcpStreamMutex);
            deviceHealth::ntripDisconnectCount++;
            if (
                (loopStatus == 500 || loopStatus == 504 || !isNtripConnected())
                && deviceHealth::ntripDisconnectCount >= CONNECTION_FAIL_LIMIT
            ) {
                    Serial.println("[NTRIP TASK][ERROR] NTRIP mat ket noi qua 5 lan, khoi dong lai ESP32...");
                    shutdownTcpTransportBeforeRestart();
                    ESP.restart();
            } else {
                deviceHealth::ntripDisconnectCount = 0;
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
    // Có tranh chấp tài nguyên với task RTCM và NTRIP publish
    String healthPayload = "";
    uint32_t loopStartTime = 0;
    uint32_t remainingWait = 0;
    while (true) {
        loopStartTime = millis();
        int32_t signalQualityDbm = -1;
        if (xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)))
        {
            if (isGsmConnection()) {
                if (xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
                    signalQualityDbm = modem.getSignalQuality();
                    xSemaphoreGive(tcpStreamMutex);
                }
            }
            const bool gnssDataOk = rtcmState->hasGnssReception
                && millis() - rtcmState->lastGnssReceptionMs <= GNSS_DATA_TIMEOUT_MS;
            const RtcmMessageCounts messageCounts = rtcmState->messageCounts;
            rtcmState->messageCounts = {};
            healthPayload = formDeviceHealthString(signalQualityDbm, gnssDataOk,
                                                   messageCounts.values);
            xSemaphoreGive(rtcmBufferMutex);
        }
        vTaskDelay(1);
        Serial.print("[HEALTH CHECK] ");
        Serial.println(healthPayload);

        if (healthPayload.isEmpty())
        {
            if (HEALTH_INTERVAL > (millis() - loopStartTime)) {
                remainingWait = HEALTH_INTERVAL - (millis() - loopStartTime);
            } else {
                remainingWait = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(remainingWait));
            continue;
        }

        #if PROGRAM_DEBUG
        Serial.println("[HEALTH CHECK] Kiem tra ket noi MQTT de gui thong tin suc khoe...");
        #endif

        // The helper waits for MQTT connection and takes the tcpStreamMutex.
        publishHealth(healthPayload);
        vTaskDelay(1);
        if (HEALTH_INTERVAL > (millis() - loopStartTime)) {
            remainingWait = HEALTH_INTERVAL - (millis() - loopStartTime);
        } else {
            remainingWait = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(remainingWait));
    }
}

__attribute__((noreturn)) void taskMQTT([[maybe_unused]] void* const parameter) {
    Serial.println("[MQTT TASK] Bat dau task MQTT...");
    while (true) {
        // Prefer to take tcpStreamMutex when available to serialize network operations
        if (tcpStreamMutex != nullptr) {
            if (xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
                serviceMqtt(true);
                xSemaphoreGive(tcpStreamMutex);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // no tcpStreamMutex available, just keep the loop running
        serviceMqtt(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

__attribute__((noreturn)) void taskSerialCommand([[maybe_unused]] void* const parameter) {
    SerialCommandProcessor commandProcessor;
    Serial.println("[SERIAL COMMAND TASK] Bat dau task lang nghe Serial...");
    while (true) {
        commandProcessor.processPending();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void loop() {
    if (isGsmConnection() && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
        if (!modem.isGprsConnected()) {
            digitalWrite(LED_PIN, HIGH);
            Serial.println("[LOOP] GPRS mat ket noi, dang thu ket noi lai...");
            activeNtripClient().stop();
            if (++deviceHealth::gsmDisconnectCount >= CONNECTION_FAIL_LIMIT) {
                Serial.println("[LOOP][ERROR] GPRS mat ket noi qua 5 lan, khoi dong lai ESP32...");
                shutdownTcpTransportBeforeRestart();
                ESP.restart();
            }
            connectGSM();
        } else {
            deviceHealth::gsmDisconnectCount = 0;
            digitalWrite(LED_PIN, LOW);
        }
        xSemaphoreGive(tcpStreamMutex);
    }
    if (isWifiConnection() && WiFiClass::status() != WL_CONNECTED) {
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[LOOP] WiFi mat ket noi, dang thu ket noi lai...");
        setupWiFi();
        digitalWrite(LED_PIN, LOW);
    }
    // MQTT loop handled in dedicated task `taskMQTT`
    vTaskDelay(pdMS_TO_TICKS(1000)); // loop trống, tất cả logic đã được xử lý trong các task
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
    prefs.putString("CONNECTION_TYPE", "4G");
    // Hai giá trị hợp lệ: "4G" và "WIFI". Giá trị này được nạp khi khởi động.

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
        deviceHealth::mqttDisconnectCount = 0;
        mqtt.loop();
        return;
    }

    if (reconnect) {
        Serial.println("[MQTT TASK] MQTT mat ket noi, dang ket noi lai...");
    }
    
    if (
        const bool restartRequired 
            = ++deviceHealth::mqttDisconnectCount >= CONNECTION_FAIL_LIMIT;
        restartRequired
    ) {
        Serial.println("[MQTT TASK][ERROR] MQTT mat ket noi qua 5 lan, khoi dong lai ESP32...");
        shutdownTcpTransportBeforeRestart();
        ESP.restart();
    }
    if (reconnect) {
        connectMQTT();
    }
}
