/// The out-of-line half of Demand: telling the provider through the Scheduler, whose definition the header does not include.

#include "core/module/Demand.h"

#include <cstring>

#include "core/module/Scheduler.h"

namespace mm {

void Demand::set(bool on) {
    if (on == on_) return;
    on_ = on;
    if (Scheduler* tree = Scheduler::instance()) tree->demandChanged(provider_);
}

uint8_t Demand::demandsOf(const char* provider) const {
    return on_ && std::strcmp(provider_, provider) == 0 ? bit_ : 0;
}

} // namespace mm
