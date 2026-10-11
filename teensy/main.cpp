/// The Teensy entry point: open USB serial, then hand control to the shared application main, which never returns.

#include <Arduino.h>
#include <CrashReport.h>

#include <cstdint>

extern void mm_main(volatile bool& keepRunning, uint16_t httpPort);

static volatile bool running = true;

void setup() {
    Serial.begin(115200);
    // A moment for a monitor to attach after the reboot that a flash ends with, so the boot banner is seen.
    const uint32_t start = millis();
    while (!Serial && millis() - start < 2000) {}
    // The fault the last run died of, kept across the reboot: the address and cause a hang or crash leaves.
    if (CrashReport) Serial.print(CrashReport);
    mm_main(running, 80);
}

void loop() {}
