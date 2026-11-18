#pragma once

/// A scheduler holding the Control module and an OSC service of a test's type, feedback on: the rig the OSC tests share.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/services/OscService.h"
#include "core/system/ControlModule.h"

namespace osc_rig {

/// The Control module and an `Osc` named "Osc" under one scheduler, set up, feedback on.
template <class Osc>
struct OscRig {
    mm::Scheduler scheduler;   ///< the scheduler both modules run under
    mm::ControlModule* control = new mm::ControlModule;   ///< the Control module, whose surface the OSC service serves
    Osc* osc = new Osc;        ///< the OSC service under test
    /// Set up both modules, feedback on.
    OscRig() {
        control->setName("Control");
        osc->setName("Osc");
        scheduler.addModule(control);
        scheduler.addModule(osc);
        scheduler.setup();
        REQUIRE(scheduler.setControl("Osc", "feedback", "{\"value\":true}") == mm::Scheduler::SetControlResult::Ok);
    }
    /// Release the scheduler and its modules.
    ~OscRig() { scheduler.release(); }
    /// One rig per test: its modules belong to it.
    OscRig(const OscRig&) = delete;
    /// One rig per test: its modules belong to it.
    OscRig& operator=(const OscRig&) = delete;
};

}  // namespace osc_rig
