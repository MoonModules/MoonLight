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

/// A MIDI control desk driving the control surface, decoded by its `profile`: Mackie Control, or the Akai APC40 mkII.
/// @card MidiService.png
///
/// The desk plugs into the computer showing the interface; the browser reads it with the Web MIDI API and writes each batch of messages into the hidden, live `midi` control.
/// A batch is each message in hex, decoded as it is written, so two identical batches, such as two equal knob turns, both count.
///
/// The way back is a surface: the hidden `desk` control holds what the desk shows, one message per fader motor, button light and knob ring.
/// It is state rather than a log, so the browser sends only the slots that changed, and a page opened later still finds the motors' positions.
/// The hidden `hello` control holds what the browser sends whenever the desk connects, such as the SysEx that hands an APC40's lights to the host.
class MidiService : public MoonModule, public ControlSurface {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Which desk this is, since each desk lays its controls out in its own messages.
    uint8_t profile = 0;

    /// Declare the desk's profile, the inbound batch, the desk's state and its greeting.
    void defineControls() override {
        controls_.addSelect("profile", profile, kProfiles, kProfileCount);
        controls_.addText("midi", midi_, sizeof(midi_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addText("desk", desk_, sizeof(desk_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setReadOnly(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addText("hello", hello_, sizeof(hello_));
        controls_.setHidden(controls_.count() - 1, true);
        controls_.setReadOnly(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        writeHello();
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

    /// Show one surface control on the desk, in the profile's messages.
    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override {
        uint8_t m[3];
        uint8_t slot;
        if (profile == kApc40 ? !apcMessage(kind, index, value, m, slot) : !mackieMessage(kind, index, value, m, slot)) return;
        writeSlot(slot, m);
    }

    /// Decode each batch the interface writes, and rebuild the desk's state if a client overwrote it or the profile changed.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "desk") == 0 || std::strcmp(name, "profile") == 0) {
            desk_[0] = 0;
            writeHello();
            if (auto* c = ControlModule::active(); c && attached_) c->resendTo(this);
            return;
        }
        if (std::strcmp(name, "midi") != 0) return;
        statusBuf_[0] = 0;
        decodeBatch(midi_);
        if (statusBuf_[0]) setStatus(statusBuf_);
    }

    /// Decode one message of up to three bytes, as the profile lays the desk out.
    void decode(const uint8_t* m, uint8_t len) {
        if (len < 3) return;
        if (profile == kApc40) decodeApc(m);
        else decodeMackie(m);
    }

private:
    static constexpr uint8_t kApc40 = 1;
    static constexpr uint8_t kProfileCount = 2;
    static constexpr const char* kProfiles[kProfileCount] = {"Mackie Control", "Akai APC40 mkII"};

    /// A Mackie desk's message for one surface control: a fader's motor, a switch's SELECT light, an encoder's ring.
    static bool mackieMessage(SurfaceControl kind, uint8_t index, uint8_t value, uint8_t m[3], uint8_t& slot) {
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
            return false;
        }
        return true;
    }

    // The APC40 runs in Alternate Ableton Live mode, set by `hello`, so every light is the host's to set.
    /// An APC40's message for one surface control: an activator's light, a knob's ring, a preset pad's color.
    static bool apcMessage(SurfaceControl kind, uint8_t index, uint8_t value, uint8_t m[3], uint8_t& slot) {
        if (kind == SurfaceControl::Switch && index < ControlModule::kSwitchCount) {
            m[0] = static_cast<uint8_t>((value ? 0x90 : 0x80) | index); m[1] = kApcActivatorNote; m[2] = value ? 0x7F : 0x00;
            slot = static_cast<uint8_t>(kDeskFaders + index);
        } else if (kind == SurfaceControl::Encoder && index < ControlModule::kEncoderCount) {
            m[0] = 0xB0; m[1] = static_cast<uint8_t>(kApcKnobCC + index); m[2] = static_cast<uint8_t>(value >> 1);
            slot = static_cast<uint8_t>(kDeskFaders + ControlModule::kSwitchCount + index);
        } else if (kind == SurfaceControl::Pad && index < kApcPads) {
            static constexpr uint8_t kColor[] = {0, kApcStoredColor, kApcActiveColor};
            m[0] = 0x90; m[1] = apcNoteOfPad(index); m[2] = kColor[value < 3 ? value : 0];
            slot = static_cast<uint8_t>(kPadSlot + index);
        } else {
            return false;   // the faders have no motors
        }
        return true;
    }

    // The APC40 counts its pads from the bottom-left, where the Control card counts from the top-left.
    /// The note of the pad on preset cell `pad`.
    static uint8_t apcNoteOfPad(uint8_t pad) {
        return static_cast<uint8_t>((kApcRows - 1 - pad / kApcCols) * kApcCols + pad % kApcCols);
    }

    /// Store one message in its slot of the desk's state, which the browser sends on.
    void writeSlot(uint8_t slot, const uint8_t m[3]) {
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

    /// What the browser sends when the desk connects: for an APC40, Alternate Ableton Live mode and rings that fill like a meter.
    void writeHello() {
        hello_[0] = 0;
        if (profile != kApc40) return;
        int n = mm::formatTo(hello_, sizeof(hello_), "f0477f29600004%02x010000f7", kApcHostLightsMode);
        for (uint8_t i = 0; i < ControlModule::kEncoderCount && n + 7 < static_cast<int>(sizeof(hello_)); i++)
            n += mm::formatTo(hello_ + n, sizeof(hello_) - static_cast<size_t>(n), " b0%02x%02x", kApcRingTypeCC + i, kApcRingVolume);
    }

    /// An APC40 message: a control change from a fader or knob, or a press.
    void decodeApc(const uint8_t* m) {
        const uint8_t status = m[0] & 0xF0, channel = m[0] & 0x0F;
        if (status == 0xB0) apcControlChange(channel, m[1], m[2]);
        else if (status == 0x90 && m[2] > 0) apcPress(channel, m[1]);   // the release does nothing
    }

    /// A track fader or a track knob, both reading 0..127, stretched so the top of the travel is the surface's 255.
    void apcControlChange(uint8_t channel, uint8_t cc, uint8_t raw) {
        const uint8_t value = static_cast<uint8_t>(raw * 255 / 127);
        if (cc == kApcFaderCC && channel < ControlModule::kFaderCount) writeSurface("fader", channel, value);
        else if (channel == 0 && cc >= kApcKnobCC && cc < kApcKnobCC + ControlModule::kEncoderCount)
            writeSurface("encoder", static_cast<uint8_t>(cc - kApcKnobCC), value);
    }

    /// An activator flips its switch, and a clip pad applies the preset on its cell.
    void apcPress(uint8_t channel, uint8_t note) {
        if (note == kApcActivatorNote && channel < ControlModule::kSwitchCount) {
            toggleSwitch(channel);
            return;
        }
        if (channel != 0 || note >= kApcPads) return;
        const uint8_t pad = apcNoteOfPad(note);   // its own inverse: the row flip undoes itself
        auto* c = ControlModule::active();
        const bool applied = c && c->pressPad(pad);
        mm::formatTo(statusBuf_, sizeof(statusBuf_), applied ? "pad %u" : "pad %u is empty", static_cast<unsigned>(pad) + 1u);
    }

    /// A Mackie Control message: a fader, a fader touch, a channel button or a knob turn.
    void decodeMackie(const uint8_t* m) {
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

    static constexpr uint8_t kTouchNote = 0x68;    ///< the first fader's touch sensor
    static constexpr uint8_t kSelectNote = 0x18;   ///< the first channel's SELECT button
    static constexpr uint8_t kVPotCC = 0x10;       ///< the first knob's rotation
    static constexpr uint8_t kRingCC = 0x30;       ///< the first knob's light ring
    static constexpr uint8_t kRingFill = 0x20;     ///< the ring mode that fills up to the position
    static constexpr uint8_t kApcFaderCC = 0x07;       ///< a track fader, one channel per track
    static constexpr uint8_t kApcKnobCC = 0x30;        ///< the first track knob, and its ring
    static constexpr uint8_t kApcRingTypeCC = 0x38;    ///< the first track knob's ring style
    static constexpr uint8_t kApcRingVolume = 2;       ///< the ring style that fills like a meter
    static constexpr uint8_t kApcActivatorNote = 0x32; ///< a track's activator button, one channel per track
    static constexpr uint8_t kApcHostLightsMode = 0x42; ///< Alternate Ableton Live mode: the host sets every light
    static constexpr uint8_t kApcCols = 8, kApcRows = 5;
    static constexpr uint8_t kApcPads = kApcCols * kApcRows;
    static_assert(kApcCols == ControlModule::kGridCols, "a pad row is a row of the Control card's preset grid");
    static constexpr uint8_t kApcStoredColor = 2;      ///< a dim white, from the APC40's palette
    static constexpr uint8_t kApcActiveColor = 21;     ///< green
    static constexpr uint8_t kDeskFaders = ControlModule::kFaderCount;
    static constexpr uint8_t kPadSlot = ControlModule::kFaderCount + ControlModule::kSwitchCount + ControlModule::kEncoderCount;
    static constexpr uint8_t kDeskSlots = kPadSlot + kApcPads;
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
    char desk_[kDeskSlots * kSlotChars] = {};   ///< the faders, the switches, the encoders, then the preset pads, as the desk should show them
    char hello_[96] = {};     ///< what the browser sends when the desk connects
    char statusBuf_[40] = {}; ///< what the last message did
    bool attached_ = false;
};

}  // namespace mm
