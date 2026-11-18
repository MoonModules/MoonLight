/// The device facts its summary and its row on the map share, written in one place, as DeviceSummary.h declares.
#include "core/system/DeviceSummary.h"

#include "core/module/Scheduler.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/SystemModule.h"
#include "core/util/JsonSink.h"

namespace mm {

void writeDeviceFacts(JsonSink& sink) {
    const Scheduler* sched = Scheduler::instance();
    const MoonModule* sys = sched ? sched->topOfType("SystemModule") : nullptr;
    sink.appendf(",\"mode\":%u", static_cast<unsigned>(sys ? static_cast<const SystemModule*>(sys)->mode() : 0));
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
