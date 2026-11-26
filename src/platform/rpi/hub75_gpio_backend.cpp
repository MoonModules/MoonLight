// SPDX-License-Identifier: GPL-3.0-or-later

/// The `GpioBackend` factory with its real-backend gate, the host refusal, the SoC's addresses and clock, and the start check both backends' start() run, in a file every host but Windows builds so they are tested wherever the stub runs.
///
/// The stub when asked for it, else the hardware backend, or null when the build has none or the gate is closed (hub75_gpio_backend.h, Real-backend gate).

#include "platform/rpi/hub75_gpio_backend.h"

#include <atomic>
#include <string>

#include "platform/rpi/hub75_rpi_common.h"   // OptionRule: the check is a table of rules

namespace mm::platform {

namespace {

// A program's gate is open; the test binary closes it before every case (hub75_gpio_backend.h, Real-backend gate).
std::atomic<bool> gateOpen_{true};
std::atomic<uint32_t> realOpens_{0};   // real backends created

// What a backend refuses to start with once it is open, in the order they are checked; the bounds are the seam's, which the options' rules read too.
constexpr OptionRule<GpioSubmitConfig> kSubmitConfigRules[] = {
    {[](const GpioSubmitConfig& c) { return c.laneWidth == 0 || c.scanRows == 0 || c.pairs == 0; }, "GPIO submit config has a zero dimension"},
    {[](const GpioSubmitConfig& c) { return hub75Outside(c.bitDepth, 2, kHub75EncodeMaxBitDepth); }, "GPIO bitDepth must be 2..8"},
    {[](const GpioSubmitConfig& c) { return hub75Outside(c.lsbNs, kHub75GpioPwmLsbMinNs, kHub75GpioPwmLsbMaxNs); }, "GPIO LSB time must be 50..3000 ns"},
    {[](const GpioSubmitConfig& c) { return hub75Outside(c.slowdown, -1, kHub75GpioSlowdownMax); }, "GPIO slowdown must be -1 (auto) or 0..4"},
};

}  // namespace

uint32_t hub75GpioPeripheralBase(PiSoc soc) {
    switch (soc) {
        case PiSoc::Bcm2837: return 0x3F000000;
        case PiSoc::Bcm2711: return 0xFE000000;
        case PiSoc::Other: return 0;
    }
    return 0;
}

uint32_t hub75GpioPlldHz(PiSoc soc) {
    return soc == PiSoc::Bcm2711 ? 750'000'000 : 500'000'000;
}

const char* hub75GpioHostRefusal() {
    if (machine() == Machine::Pi5) return kGpioPi5Refusal;
    return piSoc() == PiSoc::Other ? kGpioNotAPiRefusal : nullptr;
}

bool hub75GpioCheckStart(bool opened, const GpioSubmitConfig& config, std::string& error) {
    if (!opened) {
        error = "GPIO backend not open";
        return false;
    }
    return hub75OptionsPass(config, kSubmitConfigRules, error);
}

GpioBackend* createGpioBackend(bool preferStub) {
    if (preferStub) return createGpioStubBackend();
    if (!gateOpen_.load()) return nullptr;   // refused here, before /dev/mem is looked at
#ifdef MM_HUB75_GPIO_HAVE_HW
    realOpens_++;
    return createGpioHwBackend();
#else
    return nullptr;   // no hardware backend in this build: the engine says why
#endif
}

const char* gpioBackendRefusal() { return gateOpen_.load() ? kGpioNoBackendRefusal : kGpioTestRefusal; }

bool gpioBackendGateOpen() { return gateOpen_.load(); }

void setGpioBackendGateOpen(bool open) { gateOpen_.store(open); }

uint32_t gpioBackendRealOpens() { return realOpens_.load(); }

}  // namespace mm::platform
