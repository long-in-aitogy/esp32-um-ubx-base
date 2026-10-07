#include "functions/cmd_handler.h"
#include <vector>
#include <Preferences.h>
#include "functions/ubx_cmd_builder.h"

extern Preferences prefs;

namespace cmd_helper {

    struct PreferenceMapping {
        const char *command;
        const char *preferenceKey;
    };

    inline bool hasExactArgumentCount(const std::vector<String> &cmdWords, size_t expectedCount,
                            const char *commandName)
    {
        if (cmdWords.size() == expectedCount) {
            return true;
        }

        Serial.println("[MQTT COMMAND DOWNLINK] Error: Lenh " + String(commandName) +
                    (cmdWords.size() < expectedCount ? " khong day du." : " qua dai!"));
        return false;
    }

    inline cmd_action_t saveStringPreference(const char *key, const String &value)
    {
        prefs.begin("myPrefs", false);
        prefs.putString(key, value);
        prefs.end();
        return CMD_ACTION_NONE;
    }

    inline cmd_action_t saveUShortPreference(const char *key, uint16_t value)
    {
        prefs.begin("myPrefs", false);
        prefs.putUShort(key, value);
        prefs.end();
        return CMD_ACTION_NONE;
    }

    inline cmd_action_t saveIntPreference(const char *key, int value)
    {
        prefs.begin("myPrefs", false);
        prefs.putInt(key, value);
        prefs.end();
        return CMD_ACTION_NONE;
    }

    inline const char *findPreferenceKey(const String &command,
                                const PreferenceMapping *mappings, size_t mappingCount)
    {
        for (size_t i = 0; i < mappingCount; ++i) {
            if (command == mappings[i].command) {
                return mappings[i].preferenceKey;
            }
        }
        return nullptr;
    }

    inline void sendGnssCommands(const UbxCmdBuilder::CommandList &commands)
    {
        const UbxCmdBuilder::Command commandBytes = UbxCmdBuilder::commandListToBytes(commands);
        if (!commandBytes.empty()) {
            Serial1.write(commandBytes.data(), commandBytes.size());
        }
        Serial1.flush();
    }

} // namespace cmd_helper

std::vector<String> splitCommand(const String &command) {
    std::vector<String> cmdWords;

    int start = 0;
    const int commandLength = command.length();

    while (start <= commandLength) {
        const int end = command.indexOf(' ', start);
        String word = end == -1 ? command.substring(start) : command.substring(start, end);
        word.trim();

        if (!word.isEmpty()) {
            cmdWords.emplace_back(word);
        }

        if (end == -1) {
            break;
        }

        start = end + 1;
    }

    return cmdWords;
}

cmd_action_t handleGnssBaseCommand(const std::vector<String> &cmdWords) {
    UbxCmdBuilder::GnssOptions options;
    UbxCmdBuilder::CommandList commands;

    if (cmdWords.empty()) {
        Serial.println("[MQTT COMMAND DOWNLINK] Error: Lenh GNSS BASE khong day du.");
        return CMD_ACTION_NONE;
    }

    if (cmdWords[0] == "SURVEY_IN") {
        Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh GNSS BASE SURVEY_IN");
        if (!cmd_helper::hasExactArgumentCount(cmdWords, 3, "GNSS BASE SURVEY_IN")) {
            return CMD_ACTION_NONE;
        }
        uint32_t duration = cmdWords[1].toInt();
        float accuracy = cmdWords[2].toFloat();
        
        commands = UbxCmdBuilder::buildBaseSurveyInCommand(duration, accuracy, options);
        cmd_helper::sendGnssCommands(commands);
        Serial.println("[MQTT COMMAND DOWNLINK] Da gui lenh cau hinh GNSS BASE SURVEY_IN.");

        return CMD_ACTION_NONE;
    } else if (cmdWords[0] == "FIXED") {
        Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh GNSS FIXED LLA");
        if (!cmd_helper::hasExactArgumentCount(cmdWords, 5, "GNSS BASE FIXED")) {
            return CMD_ACTION_NONE;
        }
        double lat = cmdWords[1].toDouble();
        double lon = cmdWords[2].toDouble();
        double alt = cmdWords[3].toDouble();
        float accuracy = cmdWords[4].toFloat();

        commands = UbxCmdBuilder::buildBaseFixedLlaCommand(lat, lon, alt, accuracy, options);
        cmd_helper::sendGnssCommands(commands);
        Serial.println("[MQTT COMMAND DOWNLINK] Da gui lenh cau hinh GNSS BASE FIXED LLA.");

        return CMD_ACTION_NONE;
    } else if (cmdWords.size() == 3) {
        String action = cmdWords[2];
        action.toUpperCase();
        if (action != "ON" && action != "OFF") {
            Serial.println("[MQTT COMMAND DOWNLINK] LOI - Trang thai RTCM phai la ON hoac OFF.");
            return CMD_ACTION_NONE;
        }
        const bool enabled = action == "ON";
        Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau " + action + " RTCM " + cmdWords[0] +
                       " tren cong " + cmdWords[1]);
        commands = UbxCmdBuilder::buildBaseRtcmOutputCommand(cmdWords[0], cmdWords[1], enabled);
        if (commands.empty()) {
            Serial.println("[MQTT COMMAND DOWNLINK] LOI - RTCM message hoac cong khong hop le.");
            return CMD_ACTION_NONE;
        }
        cmd_helper::sendGnssCommands(commands);
        Serial.println("[MQTT COMMAND DOWNLINK] Da gui lenh RTCM.");
        return CMD_ACTION_NONE;
    } else {
        Serial.println("[MQTT COMMAND DOWNLINK] LOI - Lenh khong kha dung: " + cmdWords[0]);
        return CMD_ACTION_NONE;
    }
}

cmd_action_t handleEspCommand(const std::vector<String> &cmdWords) {
    if (cmdWords.empty()) {
        Serial.println("[MQTT COMMAND DOWNLINK] Error: Lenh ESP khong day du.");
        return CMD_ACTION_NONE;
    }

    if (cmdWords[0] == "RESTART") {
        Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau khoi dong lai ESP32");
        return CMD_ACTION_ESP_RESTART;
    }

    if (cmdWords[0] == "SET" && cmd_helper::hasExactArgumentCount(cmdWords, 3, "ESP SET")) {
        if (cmdWords[1] == "CONNECTION" && cmdWords[2] == "4G") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh 4G");
            cmd_helper::saveStringPreference("CONNECTION_TYPE", "4G");
            return CMD_ACTION_ESP_RESTART;
        }
        if (cmdWords[1] == "CONNECTION" && cmdWords[2] == "WIFI") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh WIFI");
            cmd_helper::saveStringPreference("CONNECTION_TYPE", "WIFI");
            return CMD_ACTION_ESP_RESTART;
        }
    }

    if (cmdWords[0] == "SET" && cmd_helper::hasExactArgumentCount(cmdWords, 4, "ESP SET")) {
        if (cmdWords[1] == "WIFI" && cmdWords[2] == "SSID") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh WIFI SSID");
            return cmd_helper::saveStringPreference("WIFI_SSID", cmdWords[3]);
        }
        if (cmdWords[1] == "WIFI" && cmdWords[2] == "PASS") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh WIFI PASS");
            return cmd_helper::saveStringPreference("WIFI_PASS", cmdWords[3]);
        }
        if (cmdWords[1] == "4G" && cmdWords[2] == "APN") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh 4G APN");
            return cmd_helper::saveStringPreference("APN", cmdWords[3]);
        }
        if (cmdWords[1] == "4G" && cmdWords[2] == "USER") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh 4G USER");
            return cmd_helper::saveStringPreference("GPRS_USER", cmdWords[3]);
        }
        if (cmdWords[1] == "4G" && cmdWords[2] == "PASS") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau cau hinh 4G PASS");
            return cmd_helper::saveStringPreference("GPRS_PASS", cmdWords[3]);
        }
    }
    
    Serial.println("[MQTT COMMAND DOWNLINK] This command is not yet implemented.");
    return CMD_ACTION_NONE;
}

cmd_action_t handleMqttCommand(const std::vector<String> &cmdWords) {
    if (cmd_helper::hasExactArgumentCount(cmdWords, 3, "MQTT" ) && cmdWords[0] == "SET") {
        static constexpr cmd_helper::PreferenceMapping stringSettings[] = {
            {"SERVER", "MQTT_SERVER"}, {"USER", "MQTT_USER"},
            {"PASS", "MQTT_PASS"}, {"PUBTPCHEALTH", "TPC_HEALTH"},
            {"SUBTPCCMD", "TPC_SUB_CMD"},
        };
        const char *key = cmd_helper::findPreferenceKey(cmdWords[1], stringSettings,
                                            sizeof(stringSettings) / sizeof(stringSettings[0]));
        if (key != nullptr) {
            return cmd_helper::saveStringPreference(key, cmdWords[2]);
        }
        if (cmdWords[1] == "PORT") {
            return cmd_helper::saveUShortPreference("MQTT_PORT", static_cast<uint16_t>(cmdWords[2].toInt()));
        }
    }
    Serial.println("[MQTT COMMAND DOWNLINK] Lenh sai hoac khong kha dung.");
    return CMD_ACTION_NONE;
}

cmd_action_t handleNtripCommand(const std::vector<String> &cmdWords) {
    if (cmd_helper::hasExactArgumentCount(cmdWords, 3, "NTRIP") && cmdWords[0] == "SET") {
        static constexpr cmd_helper::PreferenceMapping stringSettings[] = {
            {"CSTRADDR", "NTRIP_SERVER"}, {"MNTPNT", "NTRIP_MPT"},
            {"CSTRAUTH", "NT_AUTH_BS"},
        };
        const char *key = cmd_helper::findPreferenceKey(cmdWords[1], stringSettings,
                                            sizeof(stringSettings) / sizeof(stringSettings[0]));
        if (key != nullptr) {
            return cmd_helper::saveStringPreference(key, cmdWords[2]);
        }
        if (cmdWords[1] == "CSTRPORT") {
            return cmd_helper::saveUShortPreference("NTRIP_PORT", static_cast<uint16_t>(cmdWords[2].toInt()));
        }
    }
    Serial.println("[MQTT COMMAND DOWNLINK] Lenh sai hoac khong kha dung.");
    return CMD_ACTION_NONE;
}

cmd_action_t handleCommand(std::vector<String> &cmdWords) {
    if (cmdWords.size() < 2 || cmdWords[0] != "ATG") {
        Serial.println("[MQTT COMMAND DOWNLINK] Error: Empty or invalid command format. Command must start with 'ATG'.");
        return CMD_ACTION_NONE;
    }
    if (cmdWords[1] == "GNSS") {
        if (cmdWords.size() < 3) {
            Serial.println("[MQTT COMMAND DOWNLINK] Error: Incomplete GNSS command.");
            return CMD_ACTION_NONE;
        }
        if (cmdWords[2] == "BASE") {
            cmdWords.erase(cmdWords.begin(), cmdWords.begin() + 3);
            return handleGnssBaseCommand(cmdWords);
        }
        Serial.println("[MQTT COMMAND DOWNLINK] Error: Unknown GNSS command: " + cmdWords[2]);
        return CMD_ACTION_NONE;
    }    
    if (cmdWords[1] == "ESP") {
        cmdWords.erase(cmdWords.begin(), cmdWords.begin() + 2);
        return handleEspCommand(cmdWords);
    }
    if (cmdWords[1] == "MQTT") {
        cmdWords.erase(cmdWords.begin(), cmdWords.begin() + 2);
        return handleMqttCommand(cmdWords);
    }
    if (cmdWords[1] == "NTRIP") {
        cmdWords.erase(cmdWords.begin(), cmdWords.begin() + 2);
        return handleNtripCommand(cmdWords);
    }
    if (cmdWords[1] == "CONFIG") {
        if (cmdWords.size() >= 3 && cmdWords[2] == "RESET") {
            Serial.println("[MQTT COMMAND DOWNLINK] Lenh yeu cau reset ve cau hinh mac dinh");
            prefs.begin("myPrefs", false);
            prefs.putBool("NEED_RESET", true);
            prefs.end();
            return CMD_ACTION_ESP_RESTART;
        }
    }
    Serial.println("[MQTT COMMAND DOWNLINK] Unknown command: " + cmdWords[1]);
    return CMD_ACTION_NONE;
}
