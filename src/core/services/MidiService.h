#pragma once

#include "core/module/MoonModule.h"
#include "core/system/ControlModule.h"   // the surface a desk's faders, knobs and buttons land on
#include "core/util/ControlSurface.h"      // the way back: motors, rings and button lights
#include "core/util/UsbMidi.h"              // a desk on this device's own USB port
#include "core/services/RtpMidiSession.h"   // a desk on the network
#include "core/util/HostResolver.h"         // a desk named rather than numbered
#include "core/util/Ipv4.h"                 // parseDottedQuad: a desk's address
#include "platform/platform.h"
#include "core/util/format.h"              // formatTo: nonblocking formatting into a fixed buffer

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace mm {

/// A MIDI control desk driving the control surface, decoded by its `profile`: Mackie Control, or the Akai APC40 mkII.
/// @card MidiService.png
///
/// The desk plugs into the computer showing the interface; the browser reads it with the Web MIDI API and writes each batch of messages into the hidden, live `midi` control.
/// A batch is each message in hex, decoded as it is written, so two identical batches, such as two equal knob turns, both count.
///
/// The way back is a surface: the hidden `desk` control holds what the desk shows, one message per fader motor, button light and knob ring.
/// It is state rather than a log: the browser sends only the slots that changed, and a page opened later finds the motors' positions.
/// The hidden `hello` control holds what the browser sends whenever the desk connects, such as the SysEx that hands an APC40's lights to the host.
/// `source` puts the desk on this device's USB port or on the network as an RTP-MIDI session; `share` passes a USB desk on to the network.
class MidiService : public MoonModule, public ControlSurface {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// The desk layouts `profile` picks from, in its option order.
    enum Profile : uint8_t { kProfileMackie, kProfileApc40, kProfileCount };
    /// The places `source` picks from, in its option order.
    enum Source : uint8_t { kSourceBrowser, kSourceUsb, kSourceNetwork, kSourceCount };

    /// Which desk this is, since each desk lays its controls out in its own messages.
    uint8_t profile = kProfileMackie;
    /// Where the desk is plugged in: the computer showing the interface, this device's own USB port, which then carries no serial log or USB flashing, or the network.
    uint8_t source = kSourceBrowser;
    /// Whether the desk on the USB port is passed on over the network rather than driving this device.
    bool share = false;
    /// The RTP-MIDI port this device's session listens on, data on the one above.
    uint16_t port = rtpmidi::kDefaultPort;

    /// Declare the desk's profile, its port, the inbound batch, the desk's state and its greeting.
    void defineControls() override {
        controls_.addSelect("source", source, kSources, kSourceCount);
        controls_.addControl("share", share);
        controls_.setHidden(controls_.count() - 1, !onUsb());
        controls_.addSelect("profile", profile, kProfiles, kProfileCount);
        controls_.setHidden(controls_.count() - 1, sharing());   // the host the desk is shared with decodes it
        controls_.addText("host", host_, sizeof(host_));
        controls_.setHidden(controls_.count() - 1, !onNetwork());
        controls_.addControl("port", port, 1, 65535);
        controls_.setHidden(controls_.count() - 1, !onNetwork() && !sharing());
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

    /// Attach to the surface, which seeds every slot of the desk, and take the USB port or the network port when asked.
    void prepare() override {
        followSurface();
        // Against what followSource last set up, not what has come up: a USB port still being handed back is retried by the tick.
        if (source != followed_ || share != sharedFollowed_) followSource();
    }

    /// Serve a desk on the USB port or the network, or pass a shared one on.
    void tick20ms() MM_NONBLOCKING override {
        if (sharing()) serveShare();
        else if (onUsb() || onNetwork()) serveDesk();
    }

    /// Detach, since the surface list is walked from the render thread.
    void release() override {
        if (auto* c = ControlModule::active()) c->removeSurface(this);
        attached_ = false;
        if (usbBegun_) platform::usbMidiEnd();
        usbBegun_ = false;
        closeSession();
        followed_ = 0xFF;   // the next prepare sets the port up again
        MoonModule::release();
    }

    /// Show one surface control on the desk, in the profile's messages.
    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override {
        if (kind == SurfaceControl::Encoder) value = ringShare(index, value);
        uint8_t m[3];
        uint8_t slot;
        if (profile == kProfileApc40 ? !apcMessage(kind, index, value, m, slot) : !mackieMessage(kind, index, value, m, slot)) return;
        writeSlot(slot, m);
        // At once rather than on this service's next tick, so a desk released after a move lands where it ended without a step back.
        if (connected_ && greeted_) sendChangedSlots();
    }

    /// An encoder's value as its share of what its target takes, on 0..255, so a ring is full at the target's top whatever its range.
    static uint8_t ringShare(uint8_t index, uint8_t value) {
        int lo = 0, hi = 255;
        auto* c = ControlModule::active();
        if (!c || !c->targetRange(SurfaceControl::Encoder, index, lo, hi) || hi <= lo) return value;
        const int64_t v = value < lo ? lo : (value > hi ? hi : value);
        return static_cast<uint8_t>((v - lo) * 255 / (static_cast<int64_t>(hi) - lo));   // wide, since an int target's range can span the whole int
    }

    /// Show the Control card's display line on a Mackie desk's LCD, at once when the desk is there and on its greeting otherwise.
    void sendDisplay(const char* text) override {
        mm::formatTo(lcdText_, sizeof(lcdText_), "%s", text ? text : "");
        lcdSent_ = false;
        if (connected_ && greeted_) sendLcd();
    }

    /// Decode each batch the interface writes, and rebuild the desk's state if a client overwrote it or the profile changed.
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "source") == 0 || std::strcmp(name, "port") == 0 || std::strcmp(name, "share") == 0) {
            followSurface();
            followSource();
            return;
        }
        if (std::strcmp(name, "host") == 0) {
            parseHost();
            advertise();
            return;
        }
        if (std::strcmp(name, "desk") == 0 || std::strcmp(name, "profile") == 0) {
            desk_[0] = 0;
            greeted_ = false;
            writeHello();
            if (auto* c = ControlModule::active(); c && attached_) c->resendTo(this);
            return;
        }
        if (std::strcmp(name, "midi") != 0) return;
        decodeBatch(midi_);
        showLastMessage();
    }

    /// Decode one message of up to three bytes, as the profile lays the desk out.
    void decode(const uint8_t* m, uint8_t len) {
        if (len < 3) return;
        if (profile == kProfileApc40) decodeApc(m);
        else decodeMackie(m);
    }

private:
    static constexpr const char* kSources[kSourceCount] = {"browser", "USB", "network"};
    /// Whether the desk is on this device's own USB port.
    bool onUsb() const { return source == kSourceUsb; }
    /// Whether the desk is an RTP-MIDI session.
    bool onNetwork() const { return source == kSourceNetwork; }
    /// Whether the desk on the USB port is passed on rather than used here.
    bool sharing() const { return onUsb() && share; }
    /// Whether a network session is open: a desk on the network, or this device's desk shared there.
    bool needsSession() const { return onNetwork() || sharing(); }

    /// On the surface while this device uses the desk; a shared desk shows the state of the device it is shared with.
    void followSurface() {
        auto* c = ControlModule::active();
        if (!c || attached_ == !sharing()) return;
        if (sharing()) c->removeSurface(this);
        else c->addSurface(this);
        attached_ = !sharing();
    }
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
        if (profile != kProfileApc40) return;
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
        if (cc == kApcFaderCC && channel < ControlModule::kFaderCount) writeSurface(SurfaceControl::Fader, channel, value);
        else if (channel == 0 && cc >= kApcKnobCC && cc < kApcKnobCC + ControlModule::kEncoderCount)
            writeSurface(SurfaceControl::Encoder, static_cast<uint8_t>(cc - kApcKnobCC), value);
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
        if (!c) return;
        const bool empty = c->padState(pad) == ControlModule::kPadEmpty;
        const bool applied = !empty && c->pressPad(pad);
        mm::formatTo(lastMessage_, sizeof(lastMessage_), empty ? "pad %u is empty" : applied ? "pad %u" : "pad %u did not apply",
                     static_cast<unsigned>(pad) + 1u);
    }

    /// A Mackie Control message: a fader, a fader touch, a channel button or a knob turn.
    void decodeMackie(const uint8_t* m) {
        const uint8_t status = m[0] & 0xF0;
        if (status == 0xE0) mackieFader(m[0] & 0x0F, static_cast<uint16_t>(m[1] | (m[2] << 7)));
        else if (status == 0x90 || status == 0x80) mackieNote(m[1], status == 0x90 && m[2] > 0);
        else if (status == 0xB0 && m[1] >= kVPotCC && m[1] < kVPotCC + ControlModule::kEncoderCount) mackieKnob(static_cast<uint8_t>(m[1] - kVPotCC), m[2]);
    }

    /// A fader: 14-bit pitch bend, one channel per fader; channel 8 is the master, which the surface has no slot for.
    void mackieFader(uint8_t channel, uint16_t v14) {
        if (channel < ControlModule::kFaderCount) writeSurface(SurfaceControl::Fader, channel, static_cast<uint8_t>(v14 >> 6));
    }

    /// A fader's touch sensor, which holds its motor off, or a channel's SELECT button, momentary, so a press flips its switch.
    void mackieNote(uint8_t note, bool on) {
        if (note >= kTouchNote && note < kTouchNote + ControlModule::kFaderCount) {
            if (auto* c = ControlModule::active()) c->setTouched(this, SurfaceControl::Fader, static_cast<uint8_t>(note - kTouchNote), on);
        } else if (on && note >= kSelectNote && note < kSelectNote + ControlModule::kSwitchCount) {
            toggleSwitch(static_cast<uint8_t>(note - kSelectNote));
        }
    }

    /// A knob turn: bit 6 is the direction, the low six bits how far.
    void mackieKnob(uint8_t knob, uint8_t value) {
        const int steps = value & 0x3F;
        const int8_t delta = static_cast<int8_t>((value & 0x40) ? -steps : steps);
        if (auto* c = ControlModule::active()) c->turn(knob, delta);
        mm::formatTo(lastMessage_, sizeof(lastMessage_), "encoder %u %+d", static_cast<unsigned>(knob) + 1u, delta);
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

    /// Read one hex token at `p` into up to `max` bytes, moving `p` past it; the token's length in bytes, 0 at the end or at junk.
    static size_t hexToken(const char*& p, uint8_t* out, size_t max) {
        while (*p == ' ') p++;
        size_t len = 0;
        while (std::isxdigit(static_cast<unsigned char>(p[0])) && std::isxdigit(static_cast<unsigned char>(p[1]))) {
            const char hex[3] = {p[0], p[1], 0};
            if (len < max) out[len] = static_cast<uint8_t>(std::strtoul(hex, nullptr, 16));
            len++;
            p += 2;
        }
        return len;
    }

    /// Read "hex hex...", each token one message; a malformed token ends the batch.
    void decodeBatch(const char* s) {
        const char* p = s;
        uint8_t m[3] = {};
        for (size_t len; (len = hexToken(p, m, sizeof(m))) > 0;) decode(m, len > 3 ? 3 : static_cast<uint8_t>(len));
    }

    /// Take or give back the USB port and the network port as `source` says, the status saying when this device has no USB host.
    void followSource() {
        greeted_ = false;
        connected_ = false;
        shareShown_ = kSharedUnshown;
        followed_ = source;
        sharedFollowed_ = share;
        if (!onUsb() && usbBegun_) { platform::usbMidiEnd(); usbBegun_ = false; }
        closeSession();   // only a desk on the network, or one shared there, pays for a session
        if (onUsb()) {
            if (!usbBegun_) usbBegun_ = platform::usbMidiBegin();
            if (platform::hasUsbMidiHost) setStatus("USB: waiting for a desk");
            else setStatus("this device has no USB host", Severity::Warning);
            if (sharing()) openSession();
        } else if (onNetwork()) {
            openSession();
        } else {
            clearStatus();   // the browser reports the desk, not this device
        }
    }

    /// Open the network session on `port`, the status saying when the port is taken.
    void openSession() {
        session_.reset(new (std::nothrow) RtpMidiSession());
        if (session_ && session_->open(port, ControlModule::deviceName())) {
            if (onNetwork()) parseHost();   // a shared desk waits to be invited
            advertise();
            if (onNetwork()) showNetworkWait();
            return;
        }
        session_.reset();
        mm::formatTo(statusBuf_, sizeof(statusBuf_), port >= 65535 ? "port %u: data needs the one above" : "port %u busy",
                     static_cast<unsigned>(port));
        setStatus(statusBuf_, Severity::Error);
    }

    /// Close the session and stop announcing it.
    void closeSession() {
        if (advertised_) platform::mdnsWithdraw("_apple-midi", "_udp");
        advertised_ = false;
        session_.reset();
    }

    // Bonjour's name for an RTP-MIDI session, the record a Mac's Audio MIDI Setup and another device's lookup find.
    /// Announce the session while it waits to be invited: a shared desk, or a desk on the network with no host to invite.
    void advertise() {
        const bool want = session_ && (sharing() || !hostName_[0]);
        if (want) platform::mdnsAdvertise("_apple-midi", "_udp", port);
        else if (advertised_) platform::mdnsWithdraw("_apple-midi", "_udp");
        advertised_ = want;
    }

    /// Read `host`: an address or a name, with an optional `:port`; an empty one waits to be invited.
    void parseHost() {
        char text[sizeof(host_)];
        std::memcpy(text, host_, sizeof(text));
        hostPort_ = rtpmidi::kDefaultPort;
        if (char* colon = std::strchr(text, ':')) {
            *colon = 0;
            const long p = std::strtol(colon + 1, nullptr, 10);
            if (p > 0 && p < 65535) hostPort_ = static_cast<uint16_t>(p);
        }
        std::memcpy(hostName_, text, sizeof(hostName_));
        hostIsName_ = hostName_[0] && !parseDottedQuad(hostName_, hostIp_);
        if (hostIsName_) HostResolver::ensureStarted();
        if (!hostName_[0] && session_) session_->stopInviting();
        connected_ = false;   // the status says the new target on the next tick
    }

    /// Invite the host once its address is known, a name being looked up in the background.
    void inviteHost() {
        if (!hostName_[0] || !session_) return;
        if (hostIsName_) {
            const HostResolver::State st = HostResolver::lookup(hostName_, hostIp_);
            if (st != HostResolver::State::Resolved && st != HostResolver::State::Stale) return;
        }
        session_->invite(hostIp_, hostPort_);
    }

    /// What the status says while no desk is connected over the network.
    void showNetworkWait() {
        if (!session_) return;
        if (!hostName_[0]) mm::formatTo(statusBuf_, sizeof(statusBuf_), "network: waiting to be invited on %u", static_cast<unsigned>(port));
        else if (!session_->refused()) mm::formatTo(statusBuf_, sizeof(statusBuf_), "network: inviting %s", hostName_);
        else mm::formatTo(statusBuf_, sizeof(statusBuf_), "network: %s refused%s%s", hostName_, session_->refusal()[0] ? ": " : "", session_->refusal());
        setStatus(statusBuf_);
    }

    /// Whether the desk is there to talk to.
    bool deskConnected() const {
        return onUsb() ? usbBegun_ && platform::usbMidiConnected() : session_ && session_->connected();
    }

    /// The desk on the USB port or the network: decode what it sent, greet it once it appears, and send each slot of its state that changed.
    void serveDesk() {
        pumpLink();
        const bool connected = deskConnected();
        showLink(connected);
        if (!connected) {
            greeted_ = false;
            return;
        }
        if (onUsb()) readUsb();
        showLastMessage();
        if (!greeted_) greetDesk();
        sendChangedSlots();
        sendLcd();   // again when a full queue refused it
    }

    /// Keep the port going: a USB start that was refused is asked again, and the network session reads, answers and invites.
    void pumpLink() {
        if (onUsb()) {
            if (!usbBegun_) usbBegun_ = platform::usbMidiBegin();   // the port may still be on its way back from an earlier owner
            return;
        }
        if (!session_) return;
        inviteHost();
        session_->service([this](const uint8_t* m, size_t len) { if (len <= 3) decode(m, static_cast<uint8_t>(len)); });
    }

    /// Say where the desk stands when that changes: connected, waiting, or its host refusing.
    void showLink(bool connected) {
        const bool refused = session_ && session_->refused();
        if (connected == connected_ && refused == refusedShown_) return;
        connected_ = connected;
        refusedShown_ = refused;
        if (onUsb()) setStatus(connected ? "USB: desk connected" : "USB: waiting for a desk");
        else if (!connected) showNetworkWait();
        else {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "network: %s", session_->peerName()[0] ? session_->peerName() : "desk connected");
            setStatus(statusBuf_);
        }
    }

    /// The greeting first, so the desk is in the mode its lights are written for, then every slot again.
    void greetDesk() {
        const char* p = hello_;
        uint8_t msg[24];
        for (size_t len; (len = hexToken(p, msg, sizeof(msg))) > 0;) sendDesk(msg, std::min(len, sizeof(msg)));
        std::memset(shown_, 0, sizeof(shown_));
        greeted_ = true;
        lcdSent_ = false;
        sendLcd();
    }

    /// Write the display line to a Mackie desk's LCD: centered on its top row of 56 characters, the bottom row cleared.
    void sendLcd() {
        if (lcdSent_ || profile != kProfileMackie || sharing()) return;
        static constexpr uint8_t kHeader[] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00};   // Mackie Control, LCD from position 0
        uint8_t msg[sizeof(kHeader) + 2 * kLcdRow + 1];
        std::memcpy(msg, kHeader, sizeof(kHeader));
        uint8_t* row = msg + sizeof(kHeader);
        std::memset(row, ' ', 2 * kLcdRow);
        const size_t len = std::min(std::strlen(lcdText_), static_cast<size_t>(kLcdRow));
        const size_t at = (kLcdRow - len) / 2;
        for (size_t i = 0; i < len; i++) {
            const uint8_t c = static_cast<uint8_t>(lcdText_[i]);
            row[at + i] = c >= 0x20 && c < 0x7F ? c : ' ';   // the LCD is 7-bit ASCII
        }
        msg[sizeof(msg) - 1] = 0xF7;
        lcdSent_ = sendDesk(msg, sizeof(msg));
        if (session_) session_->flush();
    }

    /// The shared desk: a session is accepted only while it is plugged in, and messages pass both ways unchanged, except a SysEx the desk sends.
    void serveShare() {
        if (!usbBegun_) usbBegun_ = platform::usbMidiBegin();   // the port may still be on its way back from an earlier owner
        const bool desk = usbBegun_ && platform::usbMidiConnected();
        if (!session_) return;
        session_->setAccepting(desk, "no desk");
        session_->service([](const uint8_t* m, size_t len) { toUsb(m, len); });
        if (desk) shareUsb();
        session_->flush();
        showShare(desk);
    }

    /// One message from the network to the desk; one that does not fit its queue is dropped, as a cable at its limit does.
    static bool toUsb(const uint8_t* msg, size_t len) {
        uint8_t packets[kLcdPackets][4];
        const size_t n = usbmidi::encode(msg, len, packets, kLcdPackets);
        return n == 0 || platform::usbMidiWrite(packets, n);
    }

    /// What the desk sent since the last tick, to the network.
    void shareUsb() {
        readUsbMessages([this](const uint8_t* m, uint8_t len) { session_->send(m, len); });
    }

    /// Call `f(msg, len)` for each message the desk on the USB port sent since the last tick.
    template <class F>
    static void readUsbMessages(F&& f) {
        uint8_t packets[16][4];
        for (int round = 0; round < kUsbReadRounds; round++) {
            const size_t n = platform::usbMidiRead(packets, 16);
            for (size_t i = 0; i < n; i++) {
                uint8_t m[3];
                if (const uint8_t len = usbmidi::decode(packets[i], m)) f(m, len);
            }
            if (n < 16) break;
        }
    }

    /// Say whether the desk and the session are there, when either changes.
    void showShare(bool desk) {
        const Shared state = shareState(desk);
        if (state == shareShown_) return;
        shareShown_ = state;
        if (state == kSharedPortTaken) { setStatus("another service has the USB port", Severity::Warning); return; }
        if (state == kSharedNoDesk) { setStatus(platform::hasUsbMidiHost ? "USB: waiting for a desk" : "this device has no USB host"); return; }
        if (state == kSharedWaiting) mm::formatTo(statusBuf_, sizeof(statusBuf_), "sharing on %u, not connected", static_cast<unsigned>(port));
        else mm::formatTo(statusBuf_, sizeof(statusBuf_), "shared with %s", session_->peerName()[0] ? session_->peerName() : "a host");
        setStatus(statusBuf_);
    }

    /// Decode what the desk on the USB port sent since the last tick.
    void readUsb() {
        readUsbMessages([this](const uint8_t* m, uint8_t len) { decode(m, len); });
    }

    /// Send the desk every slot of its state it was not sent yet.
    void sendChangedSlots() {
        for (uint8_t slot = 0; slot < kDeskSlots && desk_[0]; slot++) {
            const char* hex = desk_ + slot * kSlotChars;
            if (std::memcmp(hex, shown_ + slot * 6, 6) == 0) continue;
            const char* p = hex;
            uint8_t m[3];
            // The placeholder of a slot nothing was written to is not a message, so it is only remembered.
            if (std::memcmp(hex, "000000", 6) != 0 && hexToken(p, m, sizeof(m)) == 3 && !sendDesk(m, 3)) break;   // full: the rest go next tick
            std::memcpy(shown_ + slot * 6, hex, 6);
        }
        if (session_) session_->flush();
    }

    /// Show what the last decoded message did, once: the status borrows statusBuf_, so it is written only here.
    void showLastMessage() {
        if (!lastMessage_[0]) return;
        std::memcpy(statusBuf_, lastMessage_, sizeof(statusBuf_));
        lastMessage_[0] = 0;
        setStatus(statusBuf_);
    }

    /// Send one MIDI message to the desk, false when its queue is full.
    bool sendDesk(const uint8_t* msg, size_t len) {
        return session_ ? session_->send(msg, len) : toUsb(msg, len);
    }

    static constexpr int kUsbReadRounds = 4;   ///< at most 64 packets a tick, far more than a hand moves
    static constexpr size_t kLcdRow = 56;      ///< characters per row of a Mackie desk's LCD
    static constexpr size_t kLcdPackets = 41;  ///< USB-MIDI packets for the LCD's SysEx, the longest message sent

    /// Set one surface control.
    void writeSurface(SurfaceControl kind, uint8_t index, uint8_t value) {
        if (auto* c = ControlModule::active()) c->setValue(kind, index, value);
        mm::formatTo(lastMessage_, sizeof(lastMessage_), "%s%u %u", kind == SurfaceControl::Fader ? "fader" : "encoder",
                     static_cast<unsigned>(index) + 1u, static_cast<unsigned>(value));
    }

    /// Flip one switch of the surface.
    void toggleSwitch(uint8_t index) {
        auto* c = ControlModule::active();
        if (!c) return;
        const bool on = c->value(SurfaceControl::Switch, index) == 0;
        c->setValue(SurfaceControl::Switch, index, on ? 255 : 0);
        mm::formatTo(lastMessage_, sizeof(lastMessage_), "switch%u %s", static_cast<unsigned>(index) + 1u, on ? "on" : "off");
    }

    char midi_[512] = {};     ///< the newest batch the interface wrote
    char desk_[kDeskSlots * kSlotChars] = {};   ///< the faders, the switches, the encoders, then the preset pads, as the desk should show them
    char hello_[96] = {};     ///< what the browser sends when the desk connects
    char statusBuf_[48] = {}; ///< the status shown: what the last message did, or where the desk stands
    char lastMessage_[48] = {};   ///< what the message being decoded did, shown once the batch is done
    bool attached_ = false;
    bool usbBegun_ = false;      ///< whether this device's USB port is the desk's
    bool greeted_ = false;       ///< whether the desk on the USB port or the network was greeted since it appeared
    bool connected_ = false;     ///< what the status last said about that desk
    char shown_[kDeskSlots * 6] = {};   ///< per slot, the hex that desk was last sent
    char host_[64] = {};         ///< the desk on the network, an address or a name with an optional `:port`, empty to be invited
    char hostName_[64] = {};     ///< `host_` without its port
    uint8_t hostIp_[4] = {};
    uint16_t hostPort_ = rtpmidi::kDefaultPort;
    bool hostIsName_ = false;
    bool refusedShown_ = false;  ///< whether the status last said the host refused
    bool advertised_ = false;    ///< whether the session is announced over Bonjour
    uint8_t followed_ = 0xFF;    ///< the source followSource last set up, so a re-prepare does not set it up again
    bool sharedFollowed_ = false;
    char lcdText_[57] = {};      ///< what a Mackie desk's LCD shows: the Control card's display line
    bool lcdSent_ = false;       ///< whether the desk shows lcdText_ already
    /// Where a shared desk stands, as the status says it.
    enum Shared : uint8_t { kSharedNoDesk, kSharedPortTaken, kSharedWaiting, kSharedWith, kSharedUnshown };
    /// Where a shared desk stands now.
    Shared shareState(bool desk) const {
        if (desk) return session_->connected() ? kSharedWith : kSharedWaiting;
        return !usbBegun_ && platform::usbMidiConnected() ? kSharedPortTaken : kSharedNoDesk;
    }
    Shared shareShown_ = kSharedUnshown;   ///< what the status last said about a shared desk
    std::unique_ptr<RtpMidiSession> session_;   ///< the desk on the network, while `source` says so
};

}  // namespace mm
