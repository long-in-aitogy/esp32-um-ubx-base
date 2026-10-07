#include "functions/ubx_cmd_builder.h"
#include <array>

#include <algorithm>
#include <cstring>

namespace UbxCmdBuilder
{

    constexpr std::array<uint8_t, 21> UBLOX_SAVE_CONFIG = {
        0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
        0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x1D, 0xAB};

    struct PortKeys
    {
        uint32_t uart1;
        uint32_t uart2;
        uint32_t usb;
        uint32_t i2c = 0;
        uint32_t spi = 0;
    };
    struct MessageKeys
    {
        const char *name;
        PortKeys keys;
    };

    constexpr std::array<MessageKeys, 16> NMEA_KEYS = {
        {
            {"GGA", {0x209100BB, 0x209100BC, 0x209100BD}},
            {"GSA", {0x209100C0, 0x209100C1, 0x209100C2}},
            {"GSV", {0x209100C5, 0x209100C6, 0x209100C7}},
            {"GST", {0x209100D4, 0x209100D5, 0x209100D6}},
            {"DTM", {0x209100A7, 0x209100A8, 0x209100A9}},
            {"RMC", {0x209100AC, 0x209100AD, 0x209100AE}},
            {"GNS", {0x209100B6, 0x209100B7, 0x209100B8}},
            {"VTG", {0x209100B1, 0x209100B2, 0x209100B3}},
            {"GLL", {0x209100CA, 0x209100CB, 0x209100CC}},
            {"GRS", {0x209100CF, 0x209100D0, 0x209100D1}},
            {"ZDA", {0x209100D9, 0x209100DA, 0x209100DB}},
            {"GBS", {0x209100DE, 0x209100DF, 0x209100E0}},
            {"VLW", {0x209100E8, 0x209100E9, 0x209100EA}},
            {"PUBX00", {0x209100ED, 0x209100EE, 0x209100EF}},
            {"PUBX03", {0x209100F2, 0x209100F3, 0x209100F4}},
            {"PUBX04", {0x209100F7, 0x209100F8, 0x209100F9}},
        }
    };
    
    constexpr std::array<PortKeys, 2> NAV_KEYS = {
        {
            {0x20910007, 0x20910008, 0x20910009}, {0x20910034, 0x20910035, 0x20910036}
        }    
    };

    constexpr std::array<MessageKeys, 9> RTCM_KEYS = {
        {
            {"1005", {0x209102BE, 0x209102BF, 0x209102C0, 0x209102BD, 0x209102C1}},
            {"1074", {0x2091035F, 0x20910360, 0x20910361, 0x2091035E, 0x20910362}},
            {"1084", {0x20910364, 0x20910365, 0x20910366, 0x20910363, 0x20910367}},
            {"1094", {0x20910368, 0x20910369, 0x2091036A, 0x2091036B, 0x2091036C}},
            {"1124", {0x2091036E, 0x2091036F, 0x20910370, 0x2091036D, 0x20910371}},
            {"1077", {0x209102CD, 0x209102CE, 0x209102CF, 0x209102CC, 0x209102D0}},
            {"1087", {0x209102D2, 0x209102D3, 0x209102D4, 0x209102D1, 0x209102D5}},
            {"1097", {0x209102D7, 0x209102D8, 0x209102D9, 0x209102D6, 0x209102DA}},
            {"1230", {0x20910304, 0x20910305, 0x20910306, 0x20910303, 0x20910307}},
        }
    };

    inline bool isValidRtcmPort(const String &port)
    {
        return port == "I2C" || port == "UART1" || port == "UART2" ||
               port == "USB" || port == "SPI";
    }

    inline bool isValidPort(const String &port) { 
        return port == "UART1" || port == "UART2" || port == "USB";
    }

    inline bool isValidUnicorePort(const String &port)
    {
        return port == "COM1" || port == "COM2" || port == "COM3";
    }

    uint32_t keyForPort(const PortKeys &keys, const String &port)
    {
        if (port == "UART1")
            return keys.uart1;
        else if (port == "UART2")
            return keys.uart2;
        else if (port == "USB")
            return keys.usb;
        else if (port == "I2C")
            return keys.i2c;
        else if (port == "SPI")
            return keys.spi;
        return 0;
    }

    inline void appendU32(Command &data, uint32_t value)
    {
        for (uint8_t i = 0; i < 4; ++i)
            data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

    inline void writeU32(Command &data, size_t index, uint32_t value)
    {
        for (uint8_t i = 0; i < 4; ++i)
            data[index + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    inline Command asciiCommand(const String &text)
    {
        return Command(text.begin(), text.end());
    }

    inline Command delayCommand(uint16_t delayMs) {
        return asciiCommand("$DELAY_" + String(delayMs) + "$");
    }

    Command ubxPacket(uint8_t messageClass, uint8_t messageId, const Command &payload)
    {
        Command result = {0xB5, 0x62, messageClass, messageId,
                          static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
        result.insert(result.end(), payload.begin(), payload.end());
        uint8_t ckA = 0;
        uint8_t ckB = 0;
        for (size_t i = 2; i < result.size(); ++i)
        {
            ckA += result[i];
            ckB += ckA;
        }
        result.push_back(ckA);
        result.push_back(ckB);
        return result;
    }
    
    Command ubxCfgValsetU1(const std::vector<std::pair<uint32_t, bool>> &entries)
    {
        Command payload = {0x00, 0x07, 0x00, 0x00};
        for (const auto &[key, value] : entries)
        {
            appendU32(payload, key);
            payload.push_back(value ? 1 : 0);
        }
        return ubxPacket(0x06, 0x8A, payload);
    }

    Command ubxCfgMsg(uint8_t messageClass, uint8_t messageId, const std::vector<String> &ports, uint8_t rate)
    {
        Command payload = {messageClass, messageId, 0, 0, 0, 0, 0, 0};
        for (const String &port : ports)
        {
            if (port == "I2C")
                payload[2] = rate;
            else if (port == "UART1")
                payload[3] = rate;
            else if (port == "UART2")
                payload[4] = rate;
            else if (port == "USB")
                payload[5] = rate;
            else if (port == "SPI")
                payload[6] = rate;
        }
        return ubxPacket(0x06, 0x01, payload);
    }
    
    inline bool isDiagnosticNmea(const char *name)
    {
        return !strcmp(name, "GGA") || !strcmp(name, "GSA") || !strcmp(name, "GSV") || !strcmp(name, "GST");
    }

    bool rtcmEnabled(const char *message, const GnssOptions &o)
    {
        if (!strcmp(message, "1005"))
            return true;
        const bool msm4 = o.msmLevel == MsmLevel::Msm4 || o.msmLevel == MsmLevel::Both;
        const bool msm7 = o.msmLevel == MsmLevel::Msm7 || o.msmLevel == MsmLevel::Both;
        if (!strcmp(message, "1074"))
            return msm4 && o.gps;
        if (!strcmp(message, "1084"))
            return msm4 && o.glo;
        if (!strcmp(message, "1094"))
            return msm4 && o.gal;
        if (!strcmp(message, "1124"))
            return msm4 && o.bds;
        if (!strcmp(message, "1077"))
            return msm7 && o.gps;
        if (!strcmp(message, "1087"))
            return msm7 && o.glo;
        if (!strcmp(message, "1097"))
            return msm7 && o.gal;
        return !strcmp(message, "1230") && o.glo;
    }
    
#if GNSS_MODULE_TYPE == 1
    void appendUnicoreOutputCommands(CommandList &commands, const String &port, const GnssOptions &options)
    {
        const GnssOptions o = normalizeGnssOptions(options);

        struct Messages
        {
            bool enabled;
            const char *const *items;
            size_t count;
        };

        static constexpr std::array<const char*, 4> GPS = {
            {"gpgga", "gpgsa", "gpgsv", "gpgst"}
        };

        static constexpr std::array<const char*, 3> GLO = {
            {"glgga", "glgsa", "glgsv"}
        };

        static constexpr std::array<const char*, 3> GAL = {
            {"gagga", "gagsa", "gagsv"}
        };

        static constexpr std::array<const char*, 3> BDS = {
            {"bdgga", "bdgsa", "bdgsv"}
        };

        const std::array<Messages, 4> nmea = {{
            {o.gps, GPS.data(), GPS.size()},
            {o.glo, GLO.data(), GLO.size()},
            {o.gal, GAL.data(), GAL.size()},
            {o.bds, BDS.data(), BDS.size()},
        }};

        const auto appendNmeaGroup = [&](const Messages &group) {
            if (!group.enabled)
                return;

            for (size_t i = 0; i < group.count; ++i)
            {
                commands.push_back(asciiCommand(String(group.items[i]) + " " + port + " 1\r\n"));
                commands.push_back(delayCommand(200));
            }
        };

        if (o.outputMode != OutputMode::RtcmOnly)
            for (const auto &group : nmea)
                appendNmeaGroup(group);
                    
        std::vector<String> rtcm = {"1006", "1033"};
        auto add = [&](bool enabled, const std::array<const char *, 3> &messages)
        {
            if (!enabled)
                return;
            for (const char *item : messages)
            {
                const String message(item);
                const bool ephemeris = message == "1019" || message == "1020" || message == "1042" || message == "1044" || message == "1045";
                const bool matches = (o.msmLevel == MsmLevel::Both && (message.endsWith("4") || message.endsWith("7"))) || message.endsWith(String(static_cast<uint8_t>(o.msmLevel)));
                if (ephemeris || matches)
                    rtcm.push_back(message);
            }
        };
        static constexpr std::array<const char *, 3> GPS_R = {
            {"1074", "1077", "1019"}
        };
        static constexpr std::array<const char *, 3> GLO_R = {
            {"1084", "1087", "1020"}
        };
        static constexpr std::array<const char *, 3> GAL_R = {
            {"1094", "1097", "1045"}
        };
        static constexpr std::array<const char *, 3> BDS_R = {
            {"1124", "1127", "1042"}
        };
        static constexpr std::array<const char *, 3> QZSS_R = {
            {"1114", "1117", "1044"}
        };

        add(o.gps, GPS_R);
        add(o.glo, GLO_R);
        add(o.gal, GAL_R);
        add(o.bds, BDS_R);
        add(o.qzss, QZSS_R);
        for (size_t i = 0; i < rtcm.size(); ++i)
            if (std::find(rtcm.begin(), rtcm.begin() + i, rtcm[i]) == rtcm.begin() + i)
            {
                commands.push_back(asciiCommand("rtcm" + rtcm[i] + " " + port + " 1\r\n"));
                commands.push_back(delayCommand(200));
            }
    }
#endif

    Command buildTmode3Message() { return Command({0xB5, 0x62, 0x06, 0x71, 0x28, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); }
    void finishTmode3Checksum(Command &message)
    {
        uint8_t a = 0;
        uint8_t b = 0;
        for (size_t i = 2; i < 46; ++i)
        {
            a += message[i];
            b += a;
        }
        message[46] = a;
        message[47] = b;
    }

    GnssOptions normalizeGnssOptions(GnssOptions options)
    {
        std::vector<String> ports;
        for (String port : options.ports)
        {
            port.trim();
            port.toUpperCase();
            if (isValidPort(port) && std::find(ports.begin(), ports.end(), port) == ports.end())
                ports.push_back(port);
        }
        options.ports = ports.empty() ? std::vector<String>{"USB"} : ports;
        if (!options.gps && !options.glo && !options.gal && !options.bds && !options.qzss && !options.sbas)
            options.gps = true;
        if (options.outputMode != OutputMode::RtcmOnly && options.msmLevel == MsmLevel::Both)
            options.msmLevel = MsmLevel::Msm4;
        return options;
    }

#if GNSS_MODULE_TYPE == 0
    Command buildUbloxOutputConfigCommand(const GnssOptions &options)
    {
        const GnssOptions o = normalizeGnssOptions(options);
        const bool diagnostics = o.outputMode != OutputMode::RtcmOnly;
        std::vector<std::pair<uint32_t, bool>> entries;
        constexpr std::array<PortKeys, 3> protocols = {{
            {0x10740001, 0x10760001, 0x10780001},
            {0x10740002, 0x10760002, 0x10780002},
            {0x10740004, 0x10760004, 0x10780004},
        }};
        for (const String &port : o.ports)
        {
            entries.emplace_back(keyForPort(protocols[0], port), true);
            entries.emplace_back(keyForPort(protocols[1], port), diagnostics);
            entries.emplace_back(keyForPort(protocols[2], port), true);
        }
        for (const auto &message : NMEA_KEYS)
            for (const String &port : o.ports)
                entries.emplace_back(keyForPort(message.keys, port), diagnostics && isDiagnosticNmea(message.name));
        for (const auto &keys : NAV_KEYS)
            for (const String &port : o.ports)
                entries.emplace_back(keyForPort(keys, port), diagnostics);
        for (const auto &message : RTCM_KEYS)
            for (const String &port : o.ports)
                entries.emplace_back(keyForPort(message.keys, port), rtcmEnabled(message.name, o));
        Command command = ubxCfgValsetU1(entries);
        struct MsmId
        {
            bool constellation;
            uint8_t msm4;
            uint8_t msm7;
        };
        const std::array<MsmId, 4> ids = {{
            {o.gps, 0x4A, 0x4D}, {o.glo, 0x54, 0x57},
            {o.gal, 0x5E, 0x61}, {o.bds, 0x7C, 0x7F},
        }};
        for (const auto &id : ids)
            for (uint8_t level : {(uint8_t)4, (uint8_t)7})
            {
                const bool enabled = id.constellation && (o.msmLevel == MsmLevel::Both || static_cast<uint8_t>(o.msmLevel) == level);
                Command packet = ubxCfgMsg(0xF5, level == 4 ? id.msm4 : id.msm7, enabled ? o.ports : std::vector<String>{}, enabled ? 1 : 0);
                command.insert(command.end(), packet.begin(), packet.end());
            }
        return command;
    }
#endif

    CommandList buildBaseSurveyInCommand(uint32_t duration, float accuracy, const GnssOptions &options)
    {
        CommandList commands;
        // if (sensorType == "Ublox")
        // {
        #if GNSS_MODULE_TYPE == 0
            Command message = buildTmode3Message();
            message[8] = 1;
            writeU32(message, 30, duration);
            writeU32(message, 34, static_cast<uint32_t>(accuracy * 10000));
            finishTmode3Checksum(message);
            commands.push_back(message);
            commands.push_back(buildUbloxOutputConfigCommand(options));
            commands.emplace_back(std::begin(UBLOX_SAVE_CONFIG), std::end(UBLOX_SAVE_CONFIG));
        // }
        // else if (sensorType == "Unicorecomm")
        // {
        #elif GNSS_MODULE_TYPE == 1
            commands.push_back(asciiCommand("unlogall\r\n"));
            commands.push_back(delayCommand(1000));
            const String cmd = "mode base time " + String(duration) + "\r\n";
            Serial.printf("Survey-In command: %s", cmd.c_str());
            commands.push_back(asciiCommand(cmd));
            commands.push_back(delayCommand(2000));
            Serial.println("Configuring Unicore output messages on COM3");
            appendUnicoreOutputCommands(commands, "com3", options);
            commands.push_back(asciiCommand("saveconfig\r\n"));
        // }
        #endif
        return commands;
    }

    CommandList buildBaseFixedLlaCommand(double lat, double lon, double alt, float accuracy, const GnssOptions &options)
    {
        CommandList commands;
        #if GNSS_MODULE_TYPE == 0
        // if (sensorType == "Ublox")
        // {
            Command message = buildTmode3Message();
            message[8] = 2;
            message[9] = 1;
            const std::array<double, 3> values = {lat * 10000000.0, lon * 10000000.0, alt * 100.0};
            constexpr std::array<size_t, 3> offsets = {10, 14, 18};
            for (uint8_t i = 0; i < 3; ++i)
            {
                const auto whole = static_cast<int32_t>(values[i]);
                writeU32(message, offsets[i], static_cast<uint32_t>(whole));
                message[22 + i] = static_cast<uint8_t>(static_cast<int8_t>((values[i] - whole) * 100));
            }
            const auto accuracyValue = static_cast<uint32_t>(accuracy * 10000);
            writeU32(message, 26, accuracyValue);
            finishTmode3Checksum(message);
            commands.push_back(message);
            commands.push_back(buildUbloxOutputConfigCommand(options));
            commands.emplace_back(std::begin(UBLOX_SAVE_CONFIG), std::end(UBLOX_SAVE_CONFIG));
        // }
        // else if (sensorType == "Unicorecomm")
        // {
        #elif GNSS_MODULE_TYPE == 1
            commands.push_back(asciiCommand("unlogall\r\n"));
            commands.push_back(delayCommand(1000));
            const String cmd = "mode base " + String(lat, 10) + " " + String(lon, 10) + " " + String(alt, 4) + "\r\n";
            Serial.printf("Fixed position command: %s", cmd.c_str());
            commands.push_back(asciiCommand(cmd));
            commands.push_back(delayCommand(2000));
            Serial.println("Configuring Unicore output messages on COM3");
            appendUnicoreOutputCommands(commands, "com3", options);
            commands.push_back(asciiCommand("saveconfig\r\n"));
        // }
        #endif
        return commands;
    }

    CommandList buildBaseRtcmOutputCommand(const String &message, const String &port, bool enabled)
    {
        String normalizedMessage = message;
        String normalizedPort = port;
        normalizedMessage.trim();
        normalizedMessage.toUpperCase();
        normalizedPort.trim();
        normalizedPort.toUpperCase();

        CommandList commands;
#if GNSS_MODULE_TYPE == 0
    if (!isValidRtcmPort(normalizedPort))
            return commands;

        for (const auto &rtcm : RTCM_KEYS)
        {
            if (normalizedMessage == rtcm.name)
            {
                commands.push_back(ubxCfgValsetU1({
                    {keyForPort(rtcm.keys, normalizedPort), enabled}
                }));
                commands.emplace_back(std::begin(UBLOX_SAVE_CONFIG), std::end(UBLOX_SAVE_CONFIG));
                return commands;
            }
        }
#elif GNSS_MODULE_TYPE == 1
        if (isValidUnicorePort(normalizedPort) && normalizedMessage.length() == 4)
        {
            for (uint8_t i = 0; i < normalizedMessage.length(); ++i)
                if (!isDigit(normalizedMessage[i]))
                    return commands;

            commands.push_back(asciiCommand("rtcm" + normalizedMessage + " " + normalizedPort +
                                            " " + String(enabled ? 1 : 0) + "\r\n"));
            commands.push_back(asciiCommand("saveconfig\r\n"));
        }
#endif
        return commands;
    }

    CommandList buildGeotekLteUnicoreConfig(const String &setupMethod, uint32_t duration, double lat, double lon, double alt)
    {
        CommandList commands = {asciiCommand("FRESET\r\n"), asciiCommand("unlogall\r\n")};
        String method = setupMethod;
        method.toUpperCase();
        commands.push_back(method == "SURVEY_IN" ? asciiCommand("mode base time " + String(duration) + "\r\n") : asciiCommand("mode base " + String(lat, 10) + " " + String(lon, 10) + " " + String(alt, 4) + "\r\n"));
        static constexpr std::array<const char *, 17> RTCM = {
            "1006", "1033", "1074", "1124", "1084", "1094", "1114", "1077", "1127",
            "1087", "1097", "1117", "1042", "1019", "1020", "1045", "1044",
        };
        for (const char *message : RTCM)
            commands.push_back(asciiCommand("rtcm" + String(message) + " com2 1\r\n"));
        commands.push_back(asciiCommand("saveconfig\r\n"));
        return commands;
    }

    Command commandListToBytes(const CommandList &commands)
    {
        size_t totalSize = 0;
        for (const Command &command : commands)
            totalSize += command.size();

        Command bytes;
        bytes.reserve(totalSize);
        for (const Command &command : commands)
            bytes.insert(bytes.end(), command.begin(), command.end());

        return bytes;
    }

    #if PROGRAM_DEBUG
    String debugCommand(const Command &command)
    {
        String result;
        for (uint8_t byte : command)
        {
            char hex[4];
            snprintf(hex, sizeof(hex), "%02x", byte);
            if (!result.isEmpty())
                result += ' ';
            result += hex;
        }
        return result;
    }
    #endif

} // namespace UbxCmdBuilder
