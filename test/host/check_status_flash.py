"""Check MQTT startup and two health intervals on a connected ESP32.

Run just after flashing, with Serial Monitor closed:
  ~/.platformio/penv/bin/python test/host/check_status_flash.py /dev/cu.wchusbserial110
"""
import sys
import time
from pathlib import Path

import serial


def main():
    port = sys.argv[1]
    log = Path(sys.argv[2] if len(sys.argv) > 2 else "/tmp/esp32-status-flash.log")
    deadline = time.monotonic() + 180
    connected_at = None
    failures = []
    with serial.Serial(port, 115200, timeout=0.5) as device, log.open("w") as output:
        device.dtr = False
        device.rts = False
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", "replace")
            output.write(line)
            output.flush()
            if "[BACKEND][MQTT]" in line and connected_at is None:
                connected_at = time.monotonic()
                deadline = connected_at + 65
                print("MQTT ready; observing 65 seconds (health interval: 30 seconds).", flush=True)
            if any(marker in line for marker in (
                "bootloader_mmap", "esp_image:", "Guru Meditation", "panic'ed", "Brownout",
            )):
                failures.append(line.strip())
                print(line.strip(), flush=True)
            if "rst:" in line and connected_at is not None:
                failures.append("Unexpected reset after MQTT connected: " + line.strip())
    assert connected_at is not None, f"MQTT startup not observed; inspect {log}"
    assert not failures, "\n".join(failures)
    print(f"PASS: no flash mapping error or reset during 65 seconds after MQTT startup. Log: {log}")


if __name__ == "__main__":
    main()
