#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/ControlModule.h"   // the surface a desk's faders, knobs and buttons land on

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// A MIDI control desk driving the control surface, decoded as Mackie Control: faders, their touch sensors, knobs and channel buttons.
/// @card MidiService.png
///
/// The desk plugs into the computer showing the interface; the browser reads it with the Web MIDI API and writes each batch of messages into the hidden, live `midi` control.
/// A batch is each message in hex, decoded as it is written, so two identical batches, such as two equal knob turns, both count.
class MidiService : public MoonModule {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the inbound batch.
    void defineControls() override {
        controls_.addText("midi", midi_, sizeof(midi_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        MoonModule::defineControls();
    }

    /// Decode each batch the interface writes.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "midi") != 0) return;
        statusBuf_[0] = 0;
        decodeBatch(midi_);
        if (statusBuf_[0]) setStatus(statusBuf_);
    }

    /// Decode one message of up to three bytes, as Mackie Control lays a desk out.
    void decode(const uint8_t* m, uint8_t len) {
        if (len < 3) return;
        const uint8_t status = m[0] & 0xF0, channel = m[0] & 0x0F;
        if (status == 0xE0) {
            // A fader: 14-bit pitch bend, one channel per fader; channel 8 is the master, which the surface has no slot for.
            if (channel >= ControlModule::kFaderCount) return;
            const uint16_t v14 = static_cast<uint16_t>(m[1] | (m[2] << 7));
            writeSurface("fader", channel, static_cast<uint8_t>(v14 >> 6));
            return;
        }
        const bool noteOn = status == 0x90 && m[2] > 0;
        if (status == 0x90 || status == 0x80) {
            const uint8_t note = m[1];
            if (note >= kTouchNote && note < kTouchNote + ControlModule::kFaderCount) {
                // A hand on a fader, so its motor holds off.
                if (auto* c = ControlModule::active())
                    c->setTouched(SurfaceControl::Fader, static_cast<uint8_t>(note - kTouchNote), noteOn);
                return;
            }
            if (noteOn && note >= kSelectNote && note < kSelectNote + ControlModule::kSwitchCount) {
                // A channel button is momentary, so a press flips its switch.
                toggleSwitch(static_cast<uint8_t>(note - kSelectNote));
            }
            return;
        }
        if (status == 0xB0 && m[1] >= kVPotCC && m[1] < kVPotCC + ControlModule::kEncoderCount) {
            // A knob turn: bit 6 is the direction, the low six bits how far.
            const int steps = m[2] & 0x3F;
            const int8_t delta = static_cast<int8_t>((m[2] & 0x40) ? -steps : steps);
            if (auto* c = ControlModule::active()) c->applyEncoderDelta(static_cast<uint8_t>(m[1] - kVPotCC), delta);
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "encoder %u %+d", static_cast<unsigned>(m[1] - kVPotCC + 1), delta);
        }
    }

private:
    static constexpr uint8_t kTouchNote = 0x68;    ///< the first fader's touch sensor
    static constexpr uint8_t kSelectNote = 0x18;   ///< the first channel's SELECT button
    static constexpr uint8_t kVPotCC = 0x10;       ///< the first knob's rotation

    /// Read "hex hex...", each token one message; a malformed token ends the batch.
    void decodeBatch(const char* s) {
        const char* p = s;
        while (*p) {
            while (*p == ' ') p++;
            uint8_t m[3] = {};
            uint8_t len = 0;
            while (std::isxdigit(static_cast<unsigned char>(p[0])) && std::isxdigit(static_cast<unsigned char>(p[1]))) {
                const char hex[3] = {p[0], p[1], 0};
                if (len < sizeof(m)) m[len] = static_cast<uint8_t>(std::strtoul(hex, nullptr, 16));
                len++;
                p += 2;
            }
            if (len == 0) return;
            decode(m, len > 3 ? 3 : len);
        }
    }

    /// Write one surface control through the primitive every transport uses.
    void writeSurface(const char* bank, uint8_t index, uint8_t value) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char control[16], body[24];
        mm::formatTo(control, sizeof(control), "%s%u", bank, static_cast<unsigned>(index) + 1u);
        mm::formatTo(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl("Control", control, body);
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "%s %u", control, static_cast<unsigned>(value));
    }

    /// Flip one switch of the surface.
    void toggleSwitch(uint8_t index) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char control[16];
        mm::formatTo(control, sizeof(control), "switch%u", static_cast<unsigned>(index) + 1u);
        uint8_t now = 0;
        sched->getControl("Control", control, now);
        sched->setControl("Control", control, now ? "{\"value\":false}" : "{\"value\":true}");
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "%s %s", control, now ? "off" : "on");
    }

    char midi_[512] = {};     ///< the newest batch the interface wrote
    char statusBuf_[40] = {}; ///< what the last message did
};

}  // namespace mm
