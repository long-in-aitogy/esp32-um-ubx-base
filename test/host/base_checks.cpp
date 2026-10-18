#include "functions/Station_Lock.h"
#include "functions/Unicore_Output.h"
#include "functions/Base_Sample.h"
#include "functions/Device_Token.h"
#include "functions/ubx_cmd_builder.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace UbxCmdBuilder;

void checkPackets(const Command &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        assert(bytes.size() - offset >= 8);
        assert(bytes[offset] == 0xB5 && bytes[offset + 1] == 0x62);
        const size_t size = bytes[offset + 4] | (size_t(bytes[offset + 5]) << 8);
        assert(offset + size + 8 <= bytes.size());
        uint8_t a = 0, b = 0;
        for (size_t i = offset + 2; i < offset + size + 6; ++i) { a += bytes[i]; b += a; }
        assert(bytes[offset + size + 6] == a && bytes[offset + size + 7] == b);
        offset += size + 8;
    }
}

int main(int argc, char **argv) {
    const auto tokenValid = [](const std::string &token) {
        return isDeviceBearerToken(token.data(), token.size());
    };
    assert(tokenValid("dt_" + std::string(40, 'a')));
    assert(tokenValid("dt_" + std::string(64, 'Z')));
    assert(tokenValid("dt_" + std::string(38, 'a') + "_-"));
    assert(!tokenValid("dt_" + std::string(39, 'a')));
    assert(!tokenValid("dt_" + std::string(65, 'a')));
    assert(!tokenValid("DT_" + std::string(40, 'a')));
    assert(!tokenValid("dt_" + std::string(39, 'a') + "!"));
    assert(!tokenValid("dt_" + std::string(39, 'a') + "\n"));
    assert(!isDeviceBearerToken(nullptr, 43));
    assert(freshRtkSample(5000, 4000, "RTK_FIXED"));
    assert(freshRtkSample(5000, 4000, "RTK_FLOAT"));
    assert(!freshRtkSample(5000, 0, "RTK_FIXED"));
    assert(!freshRtkSample(5000, 1000, "RTK_FIXED"));
    assert(!freshRtkSample(5000, 4000, "GPS_FIX"));
    assert(freshRtkSample(100, UINT32_MAX - 100, "RTK_FIXED"));
    StationLock lock;
    const int second[] = {2};
    assert(lock.setScoped(second, 1));
    assert(!lock.locked() && !lock.locked(1) && lock.locked(2));
    StationLock rebooted;
    rebooted.restore(lock.all, lock.servers);
    assert(!rebooted.locked(1) && rebooted.locked(2));
    const int invalid[] = {3};
    assert(!lock.setScoped(invalid, 1) && lock.locked(2));
    const int both[] = {1, 2};
    assert(lock.setScoped(both, 2) && !lock.locked() && lock.locked(1) && lock.locked(2));
    lock.restore(true, 0);
    assert(lock.locked() && lock.locked(1) && lock.locked(2));
    assert(lock.setScoped(nullptr, 0) && !lock.locked(1) && !lock.locked(2));
    lock.restore(false, 255);
    assert(lock.locked() && lock.locked(1) && lock.locked(2));

    std::string ascii = "rtcm1074 com3 1\r\n";
    routeUnicoreOutput(reinterpret_cast<uint8_t *>(&ascii[0]), ascii.size(), 2);
    assert(ascii == "rtcm1074 com2 1\r\n");
    std::string other = "config com3 115200\r\n";
    routeUnicoreOutput(reinterpret_cast<uint8_t *>(&other[0]), other.size(), 2);
    assert(other == "config com3 115200\r\n");
    for (size_t length = 0; length < ascii.size(); ++length) {
        auto shortened = ascii.substr(0, length);
        routeUnicoreOutput(reinterpret_cast<uint8_t *>(shortened.data()), length, 2);
    }
    GnssOptions usb, uart;
    uart.ports = {"UART1"};
    for (int type : {GNSS_MODULE_TYPE_UBLOX, GNSS_MODULE_TYPE_UNICORE}) {
        const auto survey = buildBaseSurveyInCommand(300, 1.0, uart, type);
        const auto fixed = buildBaseFixedLlaCommand(10.75, 106.5, 12.5, 0.01, uart, type);
        const auto rover = buildRoverCommand(type);
        assert(!survey.empty() && !fixed.empty() && !rover.empty());
        if (type == GNSS_MODULE_TYPE_UBLOX) {
            assert(survey[0][8] == 1 && fixed[0][8] == 2 && fixed[0][9] == 1);
            assert(survey[0][30] == 44 && survey[0][31] == 1);
            assert(rover[1][8] == 0);
            for (const auto &commands : {survey, fixed, rover}) for (const auto &packet : commands) checkPackets(packet);
        } else {
            const auto text = commandListToBytes(fixed);
            const std::string output(text.begin(), text.end());
            assert(output.find("mode base 10.7500000000 106.5000000000 12.5000\r\n") != std::string::npos);
            assert(output.find("com3") == std::string::npos);
            assert(output.find("rtcm1074 com2 1") != std::string::npos);
        }
    }
    auto output = buildUbloxOutputConfigCommand(usb);
    routeUbloxOutput(output.data(), output.size());
    checkPackets(output);
    assert(output == buildUbloxOutputConfigCommand(uart));

    // Optional stdin bridge lets the Python check use real CORS command bytes.
    if (argc > 1) {
        const bool ublox = std::string(argv[1]) == "Ublox";
        std::string hex;
        while (std::cin >> hex) {
            Command packet;
            for (size_t i = 0; i < hex.size(); i += 2) packet.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
            if (ublox) {
                routeUbloxOutput(packet.data(), packet.size());
                checkPackets(packet);
            } else {
                routeUnicoreOutput(packet.data(), packet.size(), 2);
            }
            for (uint8_t byte : packet) printf("%02x", byte);
            printf("\n");
        }
    } else {
        std::cout << "PASS: both receiver command sets, checksums, UART routing, scoped/persistent locks\n";
    }
}
