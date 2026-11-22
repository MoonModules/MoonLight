/// The device facts its summary and its row on the map share, written in one place, as DeviceSummary.h declares.
#include "core/system/DeviceSummary.h"

#include "core/module/Scheduler.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/SystemModule.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"   // chipRevision, chipPackage, flashChipSize, psramFoundBytes, psramType: the board facts

#include <cstring>

namespace mm {

// What the board tells about itself, from which a client names the firmware it takes (measuredFirmware in install-picker-devices.js); an ESP32 only.
static void writeHardware(JsonSink& sink) {
    const char* rev = platform::chipRevision();
    if (!rev[0]) return;
    sink.append(",\"hardware\":{\"revision\":");
    sink.writeJsonString(rev);
    sink.append(",\"package\":");
    sink.writeJsonString(platform::chipPackage());
    // The interface the build drives, "octal, 80 MHz" or "quad, 40 MHz", is the found PSRAM's: its first word, and empty with none found.
    const size_t psram = platform::psramFoundBytes();
    const char* mode = psram ? platform::psramType() : "";
    const char* word = std::strncmp(mode, "octal", 5) == 0 ? "octal" : std::strncmp(mode, "quad", 4) == 0 ? "quad" : "";
    sink.appendf(",\"flashMB\":%u,\"psramMB\":%u,\"psramMode\":\"%s\"}",
                 static_cast<unsigned>(platform::flashChipSize() >> 20), static_cast<unsigned>(psram >> 20), word);
}

void writeDeviceFacts(JsonSink& sink) {
    const Scheduler* sched = Scheduler::instance();
    const MoonModule* sys = sched ? sched->topOfType("SystemModule") : nullptr;
    sink.appendf(",\"mode\":%u", static_cast<unsigned>(sys ? static_cast<const SystemModule*>(sys)->mode() : 0));
    sink.append(",\"assetPrefix\":");   // the release image names this device updates from (kReleaseAssetUrlFormat)
    sink.writeJsonString(kReleaseAssetPrefix);
    if (kMoonBaseFamily[0]) {   // the MoonBase image it takes, shared-moonbase-<family>.bin
        sink.append(",\"moonbase\":");
        sink.writeJsonString(kMoonBaseFamily);
    }
    writeHardware(sink);
    const MoonModule* fw = sched ? sched->topOfType("FirmwareUpdateModule") : nullptr;
    if (const char* moonbase = fw ? static_cast<const FirmwareUpdateModule*>(fw)->moonbaseMismatch() : nullptr) {
        sink.append(",\"moonbaseMismatch\":");
        sink.writeJsonString(moonbase);
    }
    if (const char* coprocessor = FirmwareUpdateModule::coprocessorMismatch()) {
        sink.append(",\"coprocessorMismatch\":");
        sink.writeJsonString(coprocessor);
    }
}

}  // namespace mm
