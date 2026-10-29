"""Exercise actual fallback functions with deterministic modem/Wi-Fi I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/main.cpp').read_text()
def function(name):
    start = source.index('static ' + name)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

# Select definitions, not forward declarations.
functions = '\n'.join(function(name) for name in (
    'bool tryWifiFallback(bool protocolsRunning) {',
    'void checkSimInternetFallback() {',
))
harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <string>
enum class ConnectionType { GSM, WIFI };
ConnectionType connectionType = ConnectionType::GSM;
bool isGsmConnection() { return connectionType == ConnectionType::GSM; }
bool wifiAssociated = true, wifiInternet = true, ntripConnected = false;
uint32_t now = 0;
uint32_t millis() { return now; }
std::vector<std::string> events;
struct SerialMock { void println(const char*) {} } Serial;
struct String { String(const char*) {} bool equalsIgnoreCase(const char*) const { return false; } const char* c_str() const { return "broker.test"; } };
constexpr const char* MQTT_SERVER = "broker.test";
constexpr uint16_t MQTT_PORT = 1883;
struct Prefs { void begin(const char*,bool) {} String getString(const char*,String value) { return value; } uint16_t getUShort(const char*,uint16_t value) { return value; } void end() {} } prefs;
struct WifiMock { void disconnect() { events.push_back("wifi-disconnect"); } } WiFi;
bool setupWiFi() { events.push_back("wifi-join"); return wifiAssociated; }
struct Probe {
    bool connect(const char*, int, int) { return wifiInternet; }
    void stop() {}
}; using WiFiClient = Probe;
struct MqttMock {
    bool alive = false;
    bool connected() { return alive; }
    void disconnect() { assert(isGsmConnection()); events.push_back("mqtt-stop-old"); }
} mqtt;
void stopNtrip() { assert(isGsmConnection()); events.push_back("ntrip-stop-old"); }
void setupMQTT() { assert(!isGsmConnection()); events.push_back("mqtt-wifi"); }
void setupNTRIP() { assert(!isGsmConnection()); events.push_back("ntrip-wifi"); }
void connectNTRIP() { assert(!isGsmConnection()); events.push_back("ntrip-connect"); }
bool isNtripConnected() { return ntripConnected; }
struct Agent { void onNetworkChanged() { assert(!isGsmConnection()); events.push_back("agent-wifi"); } } backendAgent;
'''
tests = r'''
int main() {
    wifiAssociated = false;
    assert(!tryWifiFallback(true));
    assert(isGsmConnection());
    wifiAssociated = true; wifiInternet = false; events.clear();
    assert(!tryWifiFallback(true));
    assert(isGsmConnection());
    assert(events.back() == "wifi-disconnect");
    wifiInternet = true; events.clear();
    assert(tryWifiFallback(false)); // Startup changes agent transport, no old sockets.
    assert((events == std::vector<std::string>{"wifi-join", "agent-wifi"}));
    connectionType = ConnectionType::GSM; events.clear();
    assert(tryWifiFallback(true));
    assert((events == std::vector<std::string>{"wifi-join", "mqtt-stop-old", "ntrip-stop-old", "agent-wifi", "mqtt-wifi", "ntrip-wifi", "ntrip-connect"}));
    connectionType = ConnectionType::GSM; events.clear();
    now = 15000;
    checkSimInternetFallback(); assert(isGsmConnection());
    now = 30000;
    checkSimInternetFallback(); assert(isGsmConnection());
    now = 45000;
    checkSimInternetFallback(); assert(!isGsmConnection());
    connectionType = ConnectionType::GSM; mqtt.alive = true; events.clear(); now = 60000;
    checkSimInternetFallback(); assert(events.empty());
}
'''
with tempfile.TemporaryDirectory() as folder:
    code = Path(folder) / 'fallback.cpp'
    binary = Path(folder) / 'fallback'
    code.write_text(harness + functions + tests)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(code),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: startup/runtime fallback, no Internet association, old socket ordering, service timeout fallback, healthy MQTT guard')
