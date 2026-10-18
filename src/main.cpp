#include "helper.h"
#include "functions/Backend_Agent.h"
#include "functions/RTCM_Receiver.h"
#include "functions/cmd_handler.h"

#include <time.h>

// ================= ĐỊNH NGHĨA CÁC BIẾN TOÀN CỤC =================
Preferences prefs;

extern PubSubClient mqtt;

namespace {
    class DeviceRuntime {
    public:
        inline static uint8_t mqttDisconnectCount = 0;
        inline static uint8_t gsmDisconnectCount = 0;
    };
}

SemaphoreHandle_t rtcmBufferMutex = nullptr;
SemaphoreHandle_t tcpStreamMutex = nullptr;

void initPrefs();
static void serviceMqtt(bool reconnect);

__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void *parameter);
__attribute__((noreturn)) void taskMQTT([[maybe_unused]] void *parameter);
__attribute__((noreturn)) void taskSerialCommand([[maybe_unused]] void *parameter);

void setup() {
    Serial.begin(115200);
#if BOARD_HELTEC
    Mcu.begin(HELTEC_BOARD, SLOW_CLK_TPYE);
#endif
    delay(100);

    Serial.println("[DEBUG] Serial initialized");
    Serial.println("\n=========================================");
    Serial.println("     ESP32 GNSS GATEWAY KHOI DONG        ");
    Serial.println("=========================================");

    prefs.begin("myPrefs");
    const bool needReset = prefs.getBool("NEED_RESET", true);
    const int restartCount = prefs.getInt("RSTRT_COUNT", 0);
    prefs.putInt("RSTRT_COUNT", restartCount + 1);
    if (needReset) {
        Serial.println("[SETUP] Khoi tao Preferences lan dau tien...");
        initPrefs();
    }

    loadConnectionTypeFromPrefs();
    const int gnssTX = prefs.getInt("GNSS_TX", TX_GNSS);
    const int gnssRX = prefs.getInt("GNSS_RX", RX_GNSS);
    const int rx2ModemTX = prefs.getInt("RX_TO_MODEM_TX", RX_TO_MODEM_TX);
    const int tx2ModemRX = prefs.getInt("TX_TO_MODEM_RX", TX_TO_MODEM_RX);
    prefs.end();

    (void)restartCount;
    Serial.println("[SETUP] Khoi dong giao tiep UM980: TX=" + String(gnssTX)
                   + ", RX=" + String(gnssRX));
    Serial1.begin(GNSS_BAUD, SERIAL_8N1, static_cast<uint8_t>(gnssRX), static_cast<uint8_t>(gnssTX));
    Serial1.setTimeout(100);

    // CORS supplies the receiver type with the base configuration. Do not
    // send Unicore commands to an unknown (possibly u-blox) receiver at boot.

    // Load the serial, token, service configuration and persistent agent state
    // before either transport starts publishing status.
    backendAgent.begin();

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
        if (!networkConnected) {
            Serial.println("[ERROR] Khong the ket noi mang, thu lai sau 2 giay.");
            delay(2000);
        }
    }

    Serial.println("[SETUP] Ket noi mang thanh cong!");
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    rtcmBufferMutex = xSemaphoreCreateMutex();
    while (!rtcmBufferMutex) rtcmBufferMutex = xSemaphoreCreateMutex();
    tcpStreamMutex = xSemaphoreCreateMutex();
    while (!tcpStreamMutex) tcpStreamMutex = xSemaphoreCreateMutex();

    setupMQTT();
#if RTCM_COMMUNICATION_PROTOCOL == TCP_IP
    setupNTRIP();
#endif

    xTaskCreatePinnedToCore(taskMQTT, "MQTT Task", 8192, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskSerialCommand, "Serial CMD Task", 3072, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskNtrip, "NTRIP Task", 6144, nullptr, 2, nullptr, 1);

    Serial.println("=========================================");
    Serial.println("        KHOI DONG HOAN TAT               ");
    Serial.println("=========================================\n");
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
}

__attribute__((noreturn)) void taskNtrip([[maybe_unused]] void *parameter) {
    Serial.println("[NTRIP TASK] Bat dau task NTRIP...");
    while (true) {
        String rtcmRead;
        if (rtcmBufferMutex && xSemaphoreTake(rtcmBufferMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            rtcmRead = receiveRtcmFromGnss();
            xSemaphoreGive(rtcmBufferMutex);
        }

#if RTCM_COMMUNICATION_PROTOCOL == TCP_IP
        if (tcpStreamMutex && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
            if (isGsmConnection() && !modem.isGprsConnected()) {
                DeviceRuntime::gsmDisconnectCount++;
                stopNtrip();
                if (connectGSM()) DeviceRuntime::gsmDisconnectCount = 0;
            } else {
                DeviceRuntime::gsmDisconnectCount = 0;
                loopNTRIP(rtcmRead);
            }
            xSemaphoreGive(tcpStreamMutex);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
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

void initPrefs() {
    prefs.clear();
    prefs.putBool("NEED_RESET", false);
    prefs.putInt("RSTRT_COUNT", 0);
    prefs.putUChar("TX_TO_MODEM_RX", 17);
    prefs.putUChar("RX_TO_MODEM_TX", 16);
    prefs.putUChar("MODEM_DC_PIN", 15);
    prefs.putUChar("MODEM_DTR_PIN", 4);

    prefs.putString("CONNECTION_TYPE", "4G");
    prefs.putString("APN", APN);
    prefs.putString("WIFI_SSID", WIFI_SSID);
    prefs.putString("WIFI_PASS", WIFI_PASSWORD);
    prefs.putString("GPRS_USER", "");
    prefs.putString("GPRS_PASS", "");
    prefs.putInt("GNSS_RX", RX_GNSS);
    prefs.putInt("GNSS_TX", TX_GNSS);

    prefs.putString("NTRIP_SERVER", NTRIP_CASTER_IP);
    prefs.putUShort("NTRIP_PORT", NTRIP_CASTER_PORT);
    prefs.putString("NTRIP_MPT", NTRIP_MOUNTPOINT);
    prefs.putString("NT_AUTH_BS", NTRIP_AUTH_BASE_STATION);

    prefs.putString("MQTT_SERVER", MQTT_SERVER);
    prefs.putUShort("MQTT_PORT", MQTT_PORT);
    prefs.putString("MQTT_USER", MQTT_USER);
    prefs.putString("MQTT_PASS", MQTT_PASS);
    prefs.putString("TPC_SUB_CMD", TOPIC_SUB_CMD);
    prefs.putString("TPC_HEALTH", TOPIC_PUB_HEALTH);
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
