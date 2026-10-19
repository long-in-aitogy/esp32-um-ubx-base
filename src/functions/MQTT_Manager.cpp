#include "functions/MQTT_Manager.h"
#include "Top_Lvl_Config.h"
#include "Prog_Config.h"
#include "functions/Backend_Agent.h"
#include "functions/cmd_handler.h"
#include <Preferences.h>
#include <cstring>
#include "helper.h"

// ================= ĐỊNH NGHĨA CÁC ĐỐI TƯỢNG CẦN CHO KẾT NỐI =================
#include "hardware/Wifi_handler.h"
#include "hardware/Sim_handler.h"

namespace {
  WiFiClient wifiMqttClient;
}

extern TinyGsm modem;
namespace {
  TinyGsmClient gsmMqttClient(modem, 1);
}
PubSubClient mqtt(wifiMqttClient);

extern Preferences prefs;

namespace {
  String& mqttServerHost() {
    static String serverHost;
    return serverHost;
  }

  uint32_t lastMqttConnectAttemptMs = 0;
}

Client& activeMqttClient() {
  return isWifiConnection() ? static_cast<Client&>(wifiMqttClient)
                            : static_cast<Client&>(gsmMqttClient);
}

// ================= ĐỊNH NGHĨA HÀM =================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  backendAgent.onMqttMessage(topic, payload, length);

  // Keep the legacy serial-command topic working for installations that have
  // not migrated their dashboard yet.  The CORS backend uses the pi/devices
  // command topics handled above.
  if (strcmp(topic, TOPIC_SUB_CMD) == 0) {
    Serial.printf("[MQTT DOWNLINK] Nhan lenh legacy (%u bytes).\n", length);
    String cmd = "";
    for (int i = 0; i < length; i++) cmd += (char)payload[i];
    
    #if PROGRAM_DEBUG
    Serial.print("\n[MQTT DOWNLINK] Lenh: ");
    Serial.println(cmd);
    #endif

    if (cmd.isEmpty()) {
      #if PROGRAM_DEBUG
      Serial.println("[MQTT DOWNLINK] Lenh rong, khong xu ly.");
      #endif
      return;
    }
    
    // Đẩy lệnh xuống UM980 qua Serial1
    std::vector<String> cmdWords = splitCommand(cmd);
    cmd_action_t action = handleCommand(cmdWords);
    if (action == CMD_ACTION_ESP_RESTART) {
      Serial.println("[MQTT DOWNLINK] Yeu cau ESP32 khoi dong lai.");
      shutdownTcpTransportBeforeRestart();
      ESP.restart();
    }
  }
}

int setupMQTT() {
  mqtt.setClient(activeMqttClient());
  mqtt.setBufferSize(16384);
  prefs.begin("myPrefs", false);
  mqttServerHost() = prefs.getString("MQTT_SERVER", String(MQTT_SERVER));
  if (mqttServerHost().equalsIgnoreCase("aitogy.asia")) {
    // Migrate the old firmware default while preserving any user-supplied
    // broker hostname/IP.
    mqttServerHost() = MQTT_SERVER;
    prefs.putString("MQTT_SERVER", mqttServerHost());
  }
  uint16_t mqttServerPort = prefs.getUShort("MQTT_PORT", MQTT_PORT);
  prefs.end();
  mqtt.setServer(mqttServerHost().c_str(), mqttServerPort);
  mqtt.setCallback(mqttCallback);
  return 0;
}

int connectMQTT() {
  if (!mqtt.connected()) {
    if (lastMqttConnectAttemptMs != 0
        && millis() - lastMqttConnectAttemptMs < 5000UL) {
      return -1;
    }
    lastMqttConnectAttemptMs = millis();
    Serial.println("\n[MQTT] Dang ket noi Broker...");
    String clientId = "agent-" + backendAgent.serial() + "-" + String(random(0xffff), HEX);
    prefs.begin("myPrefs", false);
    String mqttUser = prefs.getString("MQTT_USER", String(MQTT_USER));
    String mqttPass = prefs.getString("MQTT_PASS", String(MQTT_PASS));
    String topicSubCmd = prefs.getString("TPC_SUB_CMD", String(TOPIC_SUB_CMD));
    prefs.end();
    const String statusTopic = String("pi/devices/") + backendAgent.serial() + "/status";
    const String lwtPayload = backendAgent.lwtPayload();
    if (mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                    statusTopic.c_str(), 1, true, lwtPayload.c_str())) {
      Serial.println("[MQTT] Da ket noi thanh cong!");
      const String commandTopic = String("pi/devices/") + backendAgent.serial() + "/command";
      const String commandsTopic = String("pi/devices/") + backendAgent.serial() + "/commands";
      const String legacyCommandTopic = String("pi/device/") + backendAgent.serial() + "/command";
      const bool commandSubSent = mqtt.subscribe(commandTopic.c_str(), 1);
      const bool commandsSubSent = mqtt.subscribe(commandsTopic.c_str(), 1);
      const bool legacySubSent = mqtt.subscribe(legacyCommandTopic.c_str(), 1);
      const bool oldSubSent = mqtt.subscribe(topicSubCmd.c_str());
      mqtt.subscribe((String("pi/devices/") + backendAgent.serial() + "/control_ack").c_str(), 1);
      Serial.printf("[MQTT] SUBSCRIBE command=%s, commands=%s, legacy=%s, old=%s\n",
                    commandSubSent ? "OK" : "LOI", commandsSubSent ? "OK" : "LOI",
                    legacySubSent ? "OK" : "LOI", oldSubSent ? "OK" : "LOI");
      backendAgent.onMqttConnected();
      return 0;
    } else {
      Serial.print("[MQTT] Loi rc=");
      Serial.print(mqtt.state());
      Serial.println(" -> Thu lai sau 5s");
      backendAgent.onMqttDisconnected();
      return -1;
    }
  }
  return 0;
}

int publishHealth(const String& payload) {
  if (payload.isEmpty()) return -1;

  // The backend-compatible status packet is produced by BackendAgent.  The
  // old health topic is still published below for backward compatibility.
  backendAgent.publishStatus(true);

  const uint32_t start = millis();
  while (!mqtt.connected()) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (millis() - start > 5000) {
      #if PROGRAM_DEBUG
      Serial.println("[MQTT] publishHealth: MQTT not connected, aborting publish");
      #endif
      return -1;
    }
  }

  if (tcpStreamMutex != nullptr && xSemaphoreTake(tcpStreamMutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS))) {
    prefs.begin("myPrefs", false);
    String topicPubHealth = prefs.getString("TPC_HEALTH", String(TOPIC_PUB_HEALTH));
    prefs.end();
    bool ok = mqtt.publish(topicPubHealth.c_str(), payload.c_str());
    xSemaphoreGive(tcpStreamMutex);
    if (ok) {
      #if PROGRAM_DEBUG
      Serial.print("[MQTT] Da gui thong tin suc khoe len topic: ");
      Serial.println(topicPubHealth);
      #endif
      return 0;
    }
    #if PROGRAM_DEBUG
    Serial.println("[MQTT] publishHealth that bai, kich thuoc payload: " + String(payload.length()));
    #endif
    return -1;
  }
  #if PROGRAM_DEBUG
  Serial.println("[MQTT] publishHealth: khong lay duoc tcpStreamMutex");
  #endif
  return -1;
}

bool isMqttConnected() {
  return mqtt.connected();
}
