"""Exercise the real startup method with real JSON/builders and mocked receiver I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/functions/Backend_Agent_Startup.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
json_headers = root / '.pio/libdeps/tdm240x_dev_general/ArduinoJson/src'
if not json_headers.exists():
    raise SystemExit('Run pio run -e tdm240x_dev_general to install ArduinoJson first')
harness = r'''
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>
#include "functions/Ubx_Base_Status.h"
#include "functions/gnss_cmd_builder.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace BackendAgentInternal { constexpr size_t CONFIG_DOCUMENT_SIZE = 4096; }
uint32_t millis() { return 10000; }
using namespace GnssCmdBuilder;
String receiverMode = "rover";
bool reject = false, queryFails = false, persistFails = false, wrongPosition = false;
unsigned writes = 0, queries = 0;
CommandList sent;
namespace UnicoreReadback {
bool readMode(String &mode, String &error) {
    ++queries; if (queryFails) { error = "timeout"; return false; } mode = receiverMode; return true;
}
const char *modeKind(const String &mode) { return mode.c_str(); }
}
namespace UbloxReadback {
bool readBaseStatus(UbxBaseStatus &status, String &error) {
    ++queries; if (queryFails) { error = "timeout"; return false; }
    status.mode = receiverMode == "survey_in" ? 1 : receiverMode == "base" ? 2 : 0;
    return true;
}
}
class BackendAgent {
public:
    bool initializeBaseAtBoot(GnssSensorType detected);
    String baseStartupState_ = "pending", sensorType_ = "UNKNOWN", baseStartupError_;
    bool baseStartupAttempted_ = false;
    String baseConfig_ = "{}", receiverBaseReadback_;
    struct { bool all = false; } stationLock_;
    uint32_t surveyInTargetSeconds_ = 0, surveyInStartedMs_ = 0, lastReceiverPollMs_ = 0;
    bool surveyInMonitoring_ = false, surveyInResultPending_ = false;
    void markStatusDirty() {}
    bool saveStatePreferences() { return !persistFails; }
    bool writeGnssCommands(const CommandList &commands) {
        ++writes; sent = commands;
        if (reject) return false;
        for (const auto &c : commands) {
            std::string text(c.begin(), c.end());
            if (text.find("mode base time ") == 0) receiverMode = "survey_in";
            else if (text.find("mode base ") == 0) receiverMode = "base";
            else if (c.size() == 48 && c[0] == 0xB5 && c[2] == 6 && c[3] == 0x71)
                receiverMode = c[8] == 1 ? "survey_in" : "base";
        }
        return true;
    }
    bool refreshReceiverReadback(String &) {
        DynamicJsonDocument d(2048); d["mode_kind"] = receiverMode;
        JsonObject p = d.createNestedObject("bestpos"); p["state"] = "ok"; p["position_type"] = "FIXEDPOS";
        p["lat"] = wrongPosition ? "22.0" : "21.08498763692";
        p["lon"] = "105.78671418823"; p["alt"] = "2.9194";
        p["raw"] = wrongPosition ? "#BESTNAVA;SOL_COMPUTED,FIXEDPOS,22,105.78671418823,2.9194,0,WGS84,0"
            : "#BESTNAVA;SOL_COMPUTED,FIXEDPOS,21.08498763692,105.78671418823,2.9194,0,WGS84,0";
        receiverBaseReadback_.clear(); serializeJson(d, receiverBaseReadback_); return true;
    }
};
'''
checks = r'''
int main() {
 for (const auto chip : {GnssSensorType::Unicore, GnssSensorType::Ublox}) {
    writes = 0; queries = 0; receiverMode = "rover";
    BackendAgent fresh;
    assert(fresh.initializeBaseAtBoot(chip) && writes == 1 && fresh.baseStartupState_ == "survey_started");
    DynamicJsonDocument config(4096); assert(!deserializeJson(config, fresh.baseConfig_.c_str()));
    assert(config["base_setup_method"] == "SURVEY_IN" && config["survey_in_duration"] == 120);
    assert(config["gnss_options"]["output_mode"] == "rtcm_only" && config["gnss_options"]["msm_level"] == "both");
    assert(fresh.initializeBaseAtBoot(chip) && writes == 1); // One setup per ESP boot.
    if (chip == GnssSensorType::Unicore) {
        std::string bytes; for (const auto &c : sent) bytes.append(c.begin(), c.end());
        assert(bytes.find("config undulation 0.0") < bytes.find("mode base time 120"));
        assert(bytes.find("gpgga") == std::string::npos && bytes.find("rtcm1074") != std::string::npos
               && bytes.find("rtcm1077") != std::string::npos);
    }
    BackendAgent resumed; resumed.baseConfig_ = fresh.baseConfig_; receiverMode = "survey_in"; writes = 0;
    assert(resumed.initializeBaseAtBoot(chip) && writes == 1 && resumed.baseStartupState_ == "survey_started");
    if (chip == GnssSensorType::Unicore) {
        std::string bytes; for (const auto &c : sent) bytes.append(c.begin(), c.end());
        assert(bytes.find("mode base time 120 0\r\n") != std::string::npos);
    } else {
        bool surveyCommand = false;
        for (const auto &c : sent) if (c.size() == 48 && c[2] == 6 && c[3] == 0x71 && c[8] == 1) surveyCommand = true;
        assert(surveyCommand);
    }
    BackendAgent cold; cold.baseConfig_ = fresh.baseConfig_; receiverMode = "rover"; writes = 0;
    assert(cold.initializeBaseAtBoot(chip) && writes == 1); // Restore after receiver state loss.
    BackendAgent fixed; receiverMode = "survey_in"; writes = 0;
    fixed.baseConfig_ = R"({"base_setup_method":"FIXED_LLA","coords":{"lat":"21.08498763692","lon":"105.78671418823","alt":"2.9194"},"accuracy":"0.015","altitude_reference":"ELLIPSOID"})";
    assert(fixed.initializeBaseAtBoot(chip) && writes == 1 && fixed.baseStartupState_ == "fixed_restored");
    std::string bytes; for (const auto &c : sent) bytes.append(c.begin(), c.end());
    assert(bytes.find("mode base time") == std::string::npos);
    if (chip == GnssSensorType::Unicore) assert(bytes.find("mode base 21.0849876369 105.7867141882 2.9194") != std::string::npos);
    BackendAgent mismatch; mismatch.baseConfig_ = fixed.baseConfig_; wrongPosition = true;
    assert(!mismatch.initializeBaseAtBoot(chip) && mismatch.baseStartupState_ == "error"); wrongPosition = false;
    BackendAgent malformed; malformed.baseConfig_ = R"({"base_setup_method":"FIXED_LLA","coords":{"lat":"oops","lon":105,"alt":0}})"; writes = 0;
    assert(!malformed.initializeBaseAtBoot(chip) && writes == 0);
    BackendAgent locked; locked.stationLock_.all = true; queries = 0;
    assert(locked.initializeBaseAtBoot(chip) && writes == 0 && queries == 0);
    BackendAgent autoBase; autoBase.baseConfig_ = R"({"base_setup_method":"AUTO_CORS"})";
    assert(autoBase.initializeBaseAtBoot(chip) && writes == 0 && queries == 0);
    BackendAgent timeout; queryFails = true;
    assert(!timeout.initializeBaseAtBoot(chip) && writes == 0); queryFails = false;
    BackendAgent refused; reject = true;
    assert(!refused.initializeBaseAtBoot(chip) && refused.baseStartupState_ == "error"); reject = false;
 }
 BackendAgent missing;
 assert(!missing.initializeBaseAtBoot(GnssSensorType::Invalid) && !missing.baseStartupAttempted_);
}
'''
with tempfile.TemporaryDirectory(prefix='base-startup-') as directory:
    path = Path(directory)
    (path / 'check.cpp').write_text(harness + source + checks)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined', '-g',
                    '-I'+str(root/'test/host'), '-I'+str(root/'include'), '-I'+str(json_headers),
                    str(path/'check.cpp'), str(root/'src/functions/gnss_cmd_builder.cpp'),
                    str(root/'src/functions/ublox_commands.cpp'), str(root/'src/functions/unicore_commands.cpp'),
                    '-o', str(path/'check')], check=True)
    subprocess.run([str(path/'check')], check=True)
print('PASS: actual startup method for both chips: defaults, ESP-only reset, receiver reset, precise Fixed LLA, locks, Auto Base, malformed config and receiver failures')
