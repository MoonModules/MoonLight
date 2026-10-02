#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/ControlModule.h"   // the surface a desk's faders, knobs and buttons land on
#include "core/util/ControlSurface.h"      // the way back: motors, rings and button lights
#include "core/util/format.h"              // formatTo: nonblocking formatting into a fixed buffer

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
///
/// The way back is a surface: the hidden `desk` control holds what the desk shows, one message per fader motor, button light and knob ring.
/// It is state rather than a log, so the browser sends only the slots that changed, and a page opened later still finds the motors' positions.
class MidiService : public MoonModule, public ControlSurface {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the inbound batch and the desk's state.
    void defineControls() override {
        controls_.addText("midi", midi_, sizeof(midi_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addText("desk", desk_, sizeof(desk_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setReadOnly(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        MoonModule::defineControls();
    }

    /// Attach to the surface once it exists, which seeds every slot of the desk.
    void tick20ms() MM_NONBLOCKING override {
        if (attached_) return;
        if (auto* c = ControlModule::active()) { attached_ = true; c->addSurface(this); }
    }

    /// Detach, since the surface list is walked from the render thread.
    void release() override {
        if (auto* c = ControlModule::active()) c->removeSurface(this);
        attached_ = false;
        MoonModule::release();
    }

    /// Show one surface control on the desk: a fader's motor, a switch's SELECT light, an encoder's ring.
    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override {
        uint8_t m[3];
        uint8_t slot;
        if (kind == SurfaceControl::Fader && index < ControlModule::kFaderCount) {
            // The byte back to 14 bits, so the desk's own pitch bend decodes to this byte again.
            const uint16_t v14 = static_cast<uint16_t>((value << 6) | (value >> 2));
            m[0] = static_cast<uint8_t>(0xE0 | index); m[1] = v14 & 0x7F; m[2] = static_cast<uint8_t>(v14 >> 7);
            slot = index;
        } else if (kind == SurfaceControl::Switch && index < ControlModule::kSwitchCount) {
            m[0] = 0x90; m[1] = static_cast<uint8_t>(kSelectNote + index); m[2] = value ? 0x7F : 0x00;
            slot = static_cast<uint8_t>(kDeskFaders + index);
        } else if (kind == SurfaceControl::Encoder && index < ControlModule::kEncoderCount) {
            // The ring fills from the left, its eleven lights showing the 0..255 position.
            m[0] = 0xB0; m[1] = static_cast<uint8_t>(kRingCC + index); m[2] = static_cast<uint8_t>(kRingFill | (1 + value * 10 / 255));
            slot = static_cast<uint8_t>(kDeskFaders + ControlModule::kSwitchCount + index);
        } else {
            return;
        }
        if (!desk_[0]) {
            // Every slot is written in the same call that attaches, before the state is read, so no placeholder reaches a desk.
            for (uint8_t i = 0; i < kDeskSlots; i++) std::memcpy(desk_ + i * kSlotChars, "000000 ", kSlotChars);
            desk_[kDeskSlots * kSlotChars - 1] = 0;
        }
        char hex[7];
        mm::formatTo(hex, sizeof(hex), "%02x%02x%02x", m[0], m[1], m[2]);
        std::memcpy(desk_ + slot * kSlotChars, hex, 6);
        // A motor following a moving value steps once a second on the periodic patch, so the desk asks to go out now.
        notifyValuesChanged();
    }

    /// Decode each batch the interface writes, and rebuild the desk's state if a client overwrote it.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "desk") == 0) {
            desk_[0] = 0;
            if (auto* c = ControlModule::active(); c && attached_) c->resendTo(this);
            return;
        }
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
                    c->setTouched(this, SurfaceControl::Fader, static_cast<uint8_t>(note - kTouchNote), noteOn);
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
    static constexpr uint8_t kRingCC = 0x30;       ///< the first knob's light ring
    static constexpr uint8_t kRingFill = 0x20;     ///< the ring mode that fills up to the position
    static constexpr uint8_t kDeskFaders = ControlModule::kFaderCount;
    static constexpr uint8_t kDeskSlots = ControlModule::kFaderCount + ControlModule::kSwitchCount + ControlModule::kEncoderCount;
    static constexpr uint8_t kSlotChars = 7;       ///< "e07f7f " per message

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
    char desk_[kDeskSlots * kSlotChars] = {};   ///< the faders, then the switches, then the encoders, as the desk should show them
    char statusBuf_[40] = {}; ///< what the last message did
    bool attached_ = false;
};

}  // namespace mm
