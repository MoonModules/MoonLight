#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // instance(): the tree whose modules name the devices a scan finds
#include "platform/platform.h"  // i2cBusOpen, i2cScan

#include <cstdint>
#include <cstdio>
#include <cstring>  // strcmp

namespace mm {

/// The board's I2C bus: its two pins, opened once and shared by every device on it, with the standard detect scan.
///
/// A device on the bus, such as AudioService's codec, names only its own address; the wires are stated here, once per board.
/// A fixed System module, wired by code.
/// @card I2cBusModule.png
///
/// @moreinfo
///
/// ## One bus, many devices
///
/// Two drivers each opening a bus on the same port collide the moment both are active, so the bus is opened here and the devices attach to it.
/// Moving it to other pins releases what is attached first, and each device attaches again on its own.
/// A scan names each answering address by the module driving it, as `0x18 Audio (ES8311)`; a claimed address that stays silent reads `no answer`.
///
/// ## The pins
///
/// They default to unused, so a board without an I2C device claims no GPIO.
/// A board with a bus sets them from its catalog entry; on one whose bus pins drive something else, such as LED outputs, they stay unset.
///
/// Prior art: the scan mirrors MoonLight's, and the probe range follows Linux i2c-tools.
class I2cBusModule : public MoonModule {
public:
    // It respects the enabled toggle, so switching it off closes the bus and frees its pins.

    /// Declare the two bus pins, the scan button and the result readout.
    void defineControls() override {
        controls_.addPin("sda", sda_);
        controls_.addPin("scl", scl_);
        controls_.addButton("scan");
        controls_.addReadOnly("result", resultStr_, sizeof(resultStr_));
        MoonModule::defineControls();
    }

    /// A pin change moves the bus, which the build sweep does through prepare.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "sda") == 0 || std::strcmp(name, "scl") == 0;
    }

    /// Open the bus on the set pins, or close it while either is unset.
    void prepare() override {
        if (sda_ < 0 || scl_ < 0) { close(); setStatus(""); return; }
        open_ = platform::i2cBusOpen(static_cast<uint16_t>(sda_), static_cast<uint16_t>(scl_));
        if (open_) setStatus("");
        else setStatus("bus could not be opened on these pins", Severity::Warning);
    }

    /// Close the bus this instance opened, the devices on it first.
    void release() override {
        close();
        MoonModule::release();
    }

    /// Run a scan when the button is pressed.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) scan();
    }

private:
    // Unused by default, so a board without a bus claims no GPIO in the pin map.
    int8_t sda_ = -1;            ///< the data pin, or -1 for unused
    int8_t scl_ = -1;            ///< the clock pin, or -1 for unused
    char resultStr_[64] = "";    ///< the addresses found, each with the module that drives it, cut off cleanly past about three
    // The bus is the board's one, so only the instance that opened it closes it; a type probe released unprepared leaves the live bus alone.
    bool open_ = false;          ///< this instance opened the bus

    /// Close the bus if this instance opened it.
    void close() {
        if (open_) platform::i2cBusClose();
        open_ = false;
    }
    // It backs the scan's status, which must outlive the call that sets it.
    char statusBuf_[24] = "idle";

    /// A device a module reports on the bus, with that module's name.
    struct Claim { uint8_t addr; const char* owner; const char* role; };

    /// Gather the devices the enabled modules report, as the pin map gathers their pins.
    static void collect(MoonModule* m, Claim* out, uint8_t& n) {
        if (!m) return;
        if (m->effectivelyEnabled()) {
            MoonModule::I2cDevice devs[4];
            const uint8_t d = m->i2cDevices(devs, 4);
            for (uint8_t i = 0; i < d && n < kMaxAddrs; i++) out[n++] = {devs[i].addr, m->name(), devs[i].role};
        }
        // Always recurse: a child is judged on its own enabled state.
        for (uint8_t i = 0; i < m->childCount(); i++) collect(m->child(i), out, n);
    }

    /// Append one entry to the result, as `0x18 Audio (ES8311)`, false once the buffer is full.
    bool appendEntry(int& pos, uint8_t addr, const Claim* c, bool answered) {
        const int w = c ? std::snprintf(resultStr_ + pos, sizeof(resultStr_) - pos, "%s0x%02x %s (%s)%s",
                                        pos ? ", " : "", addr, c->owner, c->role, answered ? "" : ": no answer")
                        : std::snprintf(resultStr_ + pos, sizeof(resultStr_) - pos, "%s0x%02x", pos ? ", " : "", addr);
        if (w <= 0 || pos + w >= static_cast<int>(sizeof(resultStr_))) { resultStr_[pos] = '\0'; return false; }
        pos += w;
        return true;
    }

    /// Probe the bus and report what answered, named by the module that drives it, or why it could not.
    void scan() {
        uint8_t found[kMaxAddrs];
        const size_t n = platform::i2cScan(found, kMaxAddrs);
        resultStr_[0] = '\0';
        if (n == platform::kI2cBusUnavailable) {
            setStatus(sda_ < 0 || scl_ < 0 ? "set sda + scl pins first" : "bus not open on these pins", Severity::Warning);
            markDirty();
            return;
        }
        Claim claims[kMaxAddrs];
        uint8_t nc = 0;
        if (Scheduler* s = Scheduler::instance())
            for (uint8_t i = 0; i < s->moduleCount(); i++) collect(s->module(i), claims, nc);
        auto claimOf = [&](uint8_t addr) -> const Claim* {
            for (uint8_t i = 0; i < nc; i++) if (claims[i].addr == addr) return &claims[i];
            return nullptr;
        };
        auto answered = [&](uint8_t addr) {
            for (size_t i = 0; i < n; i++) if (found[i] == addr) return true;
            return false;
        };
        // What answered, in address order, then what a module expects that did not, which is a wrong address or wiring.
        int pos = 0;
        bool room = true;
        for (size_t i = 0; i < n && room; i++) room = appendEntry(pos, found[i], claimOf(found[i]), true);
        unsigned missing = 0;
        for (uint8_t i = 0; i < nc; i++) {
            if (answered(claims[i].addr)) continue;
            missing++;
            if (room) room = appendEntry(pos, claims[i].addr, &claims[i], false);
        }
        // Cast so the compiler can see the counts fit, since format truncation is an error here.
        const unsigned found8 = static_cast<uint8_t>(n);
        if (missing) std::snprintf(statusBuf_, sizeof(statusBuf_), "%u found, %u missing", found8, static_cast<unsigned>(static_cast<uint8_t>(missing)));
        else std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s found", found8, n == 1 ? "" : "s");
        setStatus(statusBuf_, missing ? Severity::Warning : Severity::Status);
        markDirty();   // push the updated result + status to the UI
    }

    static constexpr size_t kMaxAddrs = 16;  ///< plenty for one bus
};

} // namespace mm
