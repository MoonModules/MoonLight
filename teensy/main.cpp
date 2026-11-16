/// The Teensy entry point: open USB serial, then hand control to the shared application main, which never returns.

#include <Arduino.h>
#include <CrashReport.h>

#include <atomic>
#include <cstdint>

#include "main.h"                // mm_main
#include "platform/platform.h"   // bootRecord: the reset cause, read before the crash report clears it

static std::atomic<bool> running{true};   // a board runs until it resets

void setup() {
    Serial.begin(115200);
    // A moment for a monitor to attach after a flash's reboot, so the banner is seen; longer after a crash, whose report is worth the wait.
    const uint32_t wait = CrashReport ? 10000 : 2000;
    const uint32_t start = millis();
    while (!Serial && millis() - start < wait) {}
    // The reset cause first: printing the report clears its registers, and a crash read as a normal boot never reaches safe mode.
    mm::platform::bootRecord();
    // The fault the last run died of, kept across the reboot: the address and cause a hang or crash leaves.
    if (CrashReport) Serial.print(CrashReport);
    mm_main(running, 80);
}

void loop() {}
