#pragma once

#include "core/util/ActiveInstance.h"   // the boot-registry seat, so a surface can find this module
#include "core/util/ControlSurface.h"
#include "core/module/MoonModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/JsonSink.h"
#include "core/module/Scheduler.h"
#include "core/util/JsonUtil.h"
#include "core/util/InputMapping.h"   // runInputAction: an encoder detent is a delta like any other
#include "platform/platform.h"

#include <cstdarg>   // setStatusf
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace mm {

/// Puts the device into a named state, and is where anything wanting to do that will live.
///
/// Its first capability is presets: a preset is a file, saving writes one, selecting reads it.
/// Top-level by necessity, since a preset reaches across the containers it captures from.
/// Not to be confused with the light presets module, a library of fixture wirings.
/// That is a profile, where this is a device state.
///
/// @moreinfo
///
/// ## What a preset captures
///
/// One top-level subtree, recorded in the file, so applying one is never a surprise.
/// That choice decides portability, a look carrying nothing about the hardware.
/// So a look applies on any board, where a driver preset carries pins and is specific.
///
/// ## Why files
///
/// One file per preset, with free-form names.
/// Deleting one is deleting a file, and backing them up is copying a folder.
/// Numbered slots would have bought a fixed grid at the cost of both.
/// The bytes inside are what the persistence engine writes, so restore reuses that engine.
class ControlModule : public MoonModule, public ListSource {
public:
    /// Where the preset files live.
    static constexpr const char* kPresetDir = "/.config/presets";
    /// The surface is a fixed grid, so a pad has a POSITION rather than a place in a list:
    static constexpr uint8_t kGridCols = 8;
    /// How many rows the pad grid has.
    static constexpr uint8_t kGridRows = 8;
    /// How many presets the grid holds, which is every cell.
    static constexpr uint8_t kMaxPresets = kGridCols * kGridRows;
    /// The longest preset name, which becomes a file name.
    static constexpr uint8_t kMaxNameLen = 32;
    /// The top-level subtrees a preset can carry.
    static constexpr const char* kCapturable[] = {"Layouts", "Effects", "Drivers", "Services"};
    /// What each capturable subtree covers, named after the CONTAINER rather than after a module.
    static constexpr const char* kCaptureRole[] = {"layout", "effects", "driver", "service"};
    static constexpr uint8_t kCaptureCount = sizeof(kCapturable) / sizeof(kCapturable[0]);
    static_assert(sizeof(kCapturable) / sizeof(kCapturable[0]) ==
                  sizeof(kCaptureRole) / sizeof(kCaptureRole[0]),
                  "kCapturable and kCaptureRole are index-aligned");
    /// Index of "Effects" within kCapturable, the role a pure look occupies.
    static constexpr uint8_t kEffectsRole = 1;
    static_assert(kCapturable[kEffectsRole][0] == 'E' && kCapturable[kEffectsRole][1] == 'f' &&
                  kCapturable[kEffectsRole][6] == 's', "kEffectsRole must index Effects");

    /// How many faders the bank shows.
    static constexpr uint8_t kFaderCount = 8;
    /// A row of rotary encoders above the pads, mirroring where both the X-Touch and the QCon put.
    static constexpr uint8_t kEncoderCount = 8;

    /// The switch row.
    static constexpr uint8_t kSwitchCount = 8;

    /// The boot ControlModule (exactly one exists).
    static ControlModule* active() { return ActiveInstance<ControlModule>::active(); }

    // --- Control surfaces -------------------------------------------------------------------  A.

    /// Attach a surface.
    void addSurface(ControlSurface* s) {
        if (!s) return;
        for (uint8_t i = 0; i < surfaceCount_; i++)
            if (surfaces_[i] == s) return;
        if (surfaceCount_ >= kMaxSurfaces) return;
        surfaces_[surfaceCount_++] = s;
        // Read the targets BEFORE seeding:
        followTargets();
        resendTo(s);
    }

    /// Push EVERY value to one surface, whatever the mirror last sent.
    void resendTo(ControlSurface* s) {
        if (!s) return;
        for (uint8_t i = 0; i < kSwitchCount; i++)
            s->sendValue(SurfaceControl::Switch, i, switches_[i] ? 255 : 0);
        for (uint8_t i = 0; i < kFaderCount; i++)   s->sendValue(SurfaceControl::Fader, i, faders_[i]);
        for (uint8_t i = 0; i < kEncoderCount; i++) s->sendValue(SurfaceControl::Encoder, i, encoders_[i]);
    }

    /// Detach.
    void removeSurface(ControlSurface* s) {
        for (uint8_t i = 0; i < surfaceCount_; i++) {
            if (surfaces_[i] != s) continue;
            surfaces_[i] = surfaces_[--surfaceCount_];
            surfaces_[surfaceCount_] = nullptr;
            return;
        }
    }

    /// A TURN, not a position.
    void applyEncoderDelta(uint8_t index, int8_t delta) {
        if (index >= kEncoderCount) return;
        // THE hardware boundary, and the only place a delta exists.
        const int v = static_cast<int>(encoders_[index]) + delta;
        encoders_[index] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        // driveEncoder clamps to the target's range and stores what it wrote, so a knob turned past.
        driveEncoder(index);
    }

    /// A hand is on this control.
    void setTouched(SurfaceControl kind, uint8_t index, bool held) {
        uint32_t* mask = touchMask(kind);
        if (!mask || index >= 32) return;
        if (held) *mask |= (1u << index);
        else      *mask &= ~(1u << index);
    }

    /// Push changed values to every attached surface.
    void mirrorToSurfaces() {
        // FOLLOW first, and unconditionally:
        followTargets();
        if (surfaceCount_ == 0) return;
        for (uint8_t i = 0; i < kSwitchCount; i++)
            mirrorOne(SurfaceControl::Switch, i, switches_[i] ? 255 : 0, sentSwitches_[i]);
        for (uint8_t i = 0; i < kEncoderCount; i++)
            mirrorOne(SurfaceControl::Encoder, i, encoders_[i], sentEncoders_[i]);
        for (uint8_t i = 0; i < kFaderCount; i++)
            mirrorOne(SurfaceControl::Fader, i, faders_[i], sentFaders_[i]);
    }

    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        mirrorToSurfaces();
        // The strip falls back to the device's name once what it was showing has gone stale.
        settleStrip();
    }

    /// Declare the strip, the switches, the encoders, the pads, the faders and the save form.
    void defineControls() override {
        // The display strip, ABOVE everything:
        controls_.addReadOnly("display", display_, sizeof(display_));
        controls_.setDisplayStrip(controls_.count() - 1);

        // The switch row sits at the TOP, above the encoders, matching the surfaces this mirrors (a.
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            controls_.addControl(kSwitchNames[i], switches_[i]);
            controls_.setSwitchRow(controls_.count() - 1, true, switchTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        // Encoders next:
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            controls_.addControl(kEncoderNames[i], encoders_[i]);
            controls_.setEncoder(controls_.count() - 1, true, encoderTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        // The ASSIGNMENTS, one hidden text control per surface control.
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            std::snprintf(targetNames_[i], kTargetNameLen, "%sTarget", kSwitchNames[i]);
            controls_.addText(targetNames_[i], switchTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            const uint8_t n = kSwitchCount + i;
            std::snprintf(targetNames_[n], kTargetNameLen, "%sTarget", kEncoderNames[i]);
            controls_.addText(targetNames_[n], encoderTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        for (uint8_t i = 0; i < kFaderCount; i++) {
            const uint8_t n = kSwitchCount + kEncoderCount + i;
            std::snprintf(targetNames_[n], kTargetNameLen, "%sTarget", kFaderNames[i]);
            controls_.addText(targetNames_[n], faderTargets_[i], kTargetLen);
            controls_.setHidden(controls_.count() - 1, true);
        }
        controls_.addList("presets", *this);
        // The fader bank.
        for (uint8_t i = 0; i < kFaderCount; i++) {
            controls_.addControl(kFaderNames[i], faders_[i]);
            controls_.setFader(controls_.count() - 1, true, surfaceTarget(i));
            controls_.setLive(controls_.count() - 1);
        }
        // The save form.
        controls_.addText("name", name_, sizeof(name_), validPresetName);
        controls_.setHidden(controls_.count() - 1, true);
        // The pad a save is aimed at:
        controls_.addControl("slot", saveSlot_, 0, kMaxPresets - 1);
        controls_.setHidden(controls_.count() - 1, true);
        // One flag per capturable subtree rather than a single multi-select:
        controls_.addSelect("captures", captureRole_, kCapturable, kCaptureCount);
        controls_.setHidden(controls_.count() - 1, true);
        controls_.addButton("save");
        controls_.setHidden(controls_.count() - 1, true);
        MoonModule::defineControls();
    }

    /// Take the surface seat, ensure the folder, and scan what is already in it.
    void setup() override {
        // Take the seat before anything looks for us:
        seat_.claim();
        platform::fsMkdir(kPresetDir);
        // A restored `slot` is meaningless:
        saveSlot_ = kNoSlot;
        rescan();
        MoonModule::setup();
    }

    /// `save` writes the current state; a fader drives whatever it targets; the rest is an assignment.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "save") == 0) { savePreset(); return; }
        // An ASSIGNMENT changed:
        if (std::strstr(controlName, "Target") != nullptr) {
            rebuildControls();
            followTargets();
            return;
        }
        for (uint8_t i = 0; i < kFaderCount; i++) {
            if (std::strcmp(controlName, kFaderNames[i]) != 0) continue;
            // NOT marked as already-sent here.
            driveFader(i);
            return;
        }
        for (uint8_t i = 0; i < kEncoderCount; i++) {
            if (std::strcmp(controlName, kEncoderNames[i]) != 0) continue;
            // A transport writes a POSITION, exactly as it does for a fader, and the MOVEMENT is what.
            driveEncoder(i);
            return;
        }
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            if (std::strcmp(controlName, kSwitchNames[i]) != 0) continue;
            // Not marked as already-sent, for the reason the fader branch above gives.
            driveSwitch(i);
            return;
        }
    }

    // ---- ListSource: one row per preset file ----

    /// How many presets the folder holds.
    uint8_t listRowCount() const override { return presetCount_; }

    /// Append one preset's row: its name, what it carries, and which roles it holds now.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        // The name is written through writeJsonString, not raw:
        sink.appendf("{\"id\":%lu,\"slot\":%u,\"name\":",
                     static_cast<unsigned long>(p.id), static_cast<unsigned>(p.slot));
        sink.writeJsonString(p.name);
        sink.append(",\"captures\":");
        sink.writeJsonString(p.captures);
        // The roles this preset covers, as role NAMES:
        sink.append(",\"roles\":[");
        bool firstRole = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!listHas(p.captures, kCapturable[i])) continue;
            sink.appendf("%s\"%s\"", firstRole ? "" : ",", kCaptureRole[i]);
            firstRole = false;
        }
        sink.append("]");
        // Which pad is lit, and for WHICH roles.
        sink.append(",\"activeRoles\":[");
        bool firstActive = true;
        for (uint8_t i = 0; i < kCaptureCount; i++) {
            if (!p.name[0] || std::strcmp(p.name, current_[i]) != 0) continue;
            sink.appendf("%s\"%s\"", firstActive ? "" : ",", kCaptureRole[i]);
            firstActive = false;
        }
        sink.append("]");
        if (!firstActive) sink.append(",\"active\":true");
        sink.append("}");
    }

    /// The expanded row:
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        if (row >= presetCount_) return;
        const Preset& p = presets_[row];
        sink.append("{\"fields\":[{\"name\":\"name\",\"type\":\"text\",\"value\":");
        sink.writeJsonString(p.name);
        sink.append("},{\"name\":\"captures\",\"type\":\"text\",\"readonly\":true,\"value\":");
        sink.writeJsonString(p.captures[0] ? p.captures : "(unknown)");
        // refetch: applying a preset rewrites the module tree, so the whole card set is stale.
        sink.append("},{\"name\":\"apply\",\"type\":\"button\",\"label\":\"apply\","
                    "\"refetch\":true}]}");
    }

    // ---- Presets as an external surface (Home Assistant, and any future consumer).

    /// The one role this preset carries, or kCaptureCount if the file names none or several.
    uint8_t roleOf(uint8_t row) const {
        if (row >= presetCount_) return kCaptureCount;
        uint8_t found = kCaptureCount, n = 0;
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (listHas(presets_[row].captures, kCapturable[i])) { found = i; n++; }
        return n == 1 ? found : kCaptureCount;
    }

    /// Whether this preset is a pure look, which with one role each means its role is Effects.
    bool isLookOnly(uint8_t row) const { return roleOf(row) == kEffectsRole; }

    /// The preset's name, or null for an out-of-range row.
    const char* presetName(uint8_t row) const {
        return row < presetCount_ ? presets_[row].name : nullptr;
    }

    /// How many presets are on the device.
    uint8_t presetCount() const { return presetCount_; }

    /// Monotonic revision of the preset SET, bumped by every save, delete, rename and rescan.
    uint32_t presetsRevision() const { return presetsRevision_; }

    /// Apply a LOOK by name.
    bool applyLookByName(const char* name) {
        if (!name || !name[0]) return false;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (std::strcmp(presets_[i].name, name) != 0) continue;
            if (!isLookOnly(i)) return false;
            return applyPreset(presets_[i].name);
        }
        return false;
    }

    /// The look applied most recently, or "" when none is.
    const char* currentLook() const { return current_[kEffectsRole]; }

    /// Editable, since a pad is renamed and deleted from the surface.
    bool isEditableList() const override { return true; }

    /// The preset FOLDER is the state; rescan() rebuilds these rows at setup.
    bool persistsList() const override { return false; }

    /// Presets are triggered far more than they are edited, so the rows render as a grid of pads:
    bool listAsPads() const override { return true; }

    /// The surface's shape.
    uint8_t listGridCols() const override { return kGridCols; }
    /// How many rows the surface renders.
    uint8_t listGridRows() const override { return kGridRows; }

    /// Deleting a row deletes the file.
    bool deleteListRow(uint32_t id) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            char path[128];
            pathFor(presets_[i].name, path, sizeof(path));
            // Remember the name before the row goes:
            char goneName[kMaxNameLen];
            std::snprintf(goneName, sizeof(goneName), "%s", presets_[i].name);
            const bool ok = platform::fsRemove(path);
            if (ok) clearCurrentIfNamed(goneName);
            if (ok) setSurfaceStatusf("deleted %s", presets_[i].name);
            else    setStatusf(Severity::Error, "could not delete %s", presets_[i].name);
            rescan();
            return ok;
        }
        return false;
    }

    /// A fader drives its target through Scheduler::setControl, the same domain-neutral primitive.
    const char* surfaceTarget(uint8_t index) const {
        if (index >= kFaderCount || !faderTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return faderTargets_[index];
    }

    /// What a switch drives, as "Module.control", or null when it drives nothing yet.
    const char* switchTarget(uint8_t index) const {
        if (index >= kSwitchCount || !switchTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return switchTargets_[index];
    }

    /// What an encoder drives, as "Module.control", or null when it drives nothing yet.
    const char* encoderTarget(uint8_t index) const {
        if (index >= kEncoderCount || !encoderTargets_[index][0]) return nullptr;   // unassigned drives nothing
        return encoderTargets_[index];
    }

    /// Drives whatever `switchTarget` declares.
    void driveSwitch(uint8_t index) {
        const char* target = switchTarget(index);
        if (!target || index >= kSwitchCount) return;
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", switches_[index] ? "true" : "false");
        sched->setControl(module, dot + 1, body);
        // A bool has no option names, so the strip says on or off rather than 1 or 0:
        writeStrip("%s %s", dot + 1, switches_[index] ? "on" : "off");
    }

    /// Read every bound control back, so a surface FOLLOWS what it drives.
    void followTargets() {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        for (uint8_t i = 0; i < kFaderCount; i++) pullTarget(SurfaceControl::Fader, i, faders_[i]);
        // Encoders follow too.
        for (uint8_t i = 0; i < kEncoderCount; i++) pullTarget(SurfaceControl::Encoder, i, encoders_[i]);
        for (uint8_t i = 0; i < kSwitchCount; i++) {
            uint8_t v = switches_[i] ? 255 : 0;
            if (pullTarget(SurfaceControl::Switch, i, v)) switches_[i] = v != 0;
        }
    }

    /// A target control's descriptor, for its type and bounds.
    static const ControlDescriptor* findControl(const char* moduleName, const char* controlName) {
        auto* sched = Scheduler::instance();
        MoonModule* m = sched ? sched->firstByName(moduleName) : nullptr;
        if (!m) return nullptr;
        const ControlList& cs = m->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, controlName) == 0) return &cs[i];
        return nullptr;
    }

    /// One control's read-back.
    bool pullTarget(SurfaceControl kind, uint8_t index, uint8_t& value) {
        const char* target = kind == SurfaceControl::Switch  ? switchTarget(index)
                           : kind == SurfaceControl::Encoder ? encoderTarget(index)
                                                             : surfaceTarget(index);
        if (!target) return false;                    // unassigned: nothing to follow
        const char* dot = std::strchr(target, '.');
        if (!dot) return false;
        auto* sched = Scheduler::instance();
        if (!sched) return false;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        uint8_t live = 0;
        if (!sched->getControl(module, dot + 1, live)) return false;
        if (live == value) return false;
        value = live;
        return true;
    }

    /// Write a surface control's value onto whatever it targets.
    void driveSurface(SurfaceControl kind, uint8_t index) {
        const char* target = kind == SurfaceControl::Encoder ? encoderTarget(index)
                                                             : surfaceTarget(index);
        if (!target) return;                          // unassigned
        const char* dot = std::strchr(target, '.');
        if (!dot) return;
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char module[24];
        const size_t n = std::min(static_cast<size_t>(dot - target), sizeof(module) - 1);
        std::memcpy(module, target, n);
        module[n] = '\0';
        uint8_t value = kind == SurfaceControl::Encoder ? encoders_[index] : faders_[index];
        // Clamped to the TARGET's range before writing:
        if (const ControlDescriptor* tc = findControl(module, dot + 1)) {
            const int hi = (tc->type == ControlType::Select || tc->type == ControlType::Palette)
                               ? static_cast<int>(tc->max) - 1 : static_cast<int>(tc->max);
            if (value > hi) value = static_cast<uint8_t>(hi < 0 ? 0 : hi);
            if (value < tc->min) value = tc->min;
        }
        // And the control itself holds what it just wrote, so the next read agrees with the target.
        if (kind == SurfaceControl::Encoder) encoders_[index] = value; else faders_[index] = value;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(module, dot + 1, body);
        showOnStrip(module, dot + 1, value);
        followTargets();   // siblings on the same target update now, not at the next 1 Hz sample
    }

    /// Write one fader's value onto whatever it targets.
    void driveFader(uint8_t index)   { driveSurface(SurfaceControl::Fader, index); }
    /// Write one encoder's value onto whatever it targets.
    void driveEncoder(uint8_t index) { driveSurface(SurfaceControl::Encoder, index); }

    /// Put text on the display strip, and start its five-second life.
    void writeStrip(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(display_, sizeof(display_), fmt, ap);
        va_end(ap);
        stripWrittenMs_ = platform::millis();
        stripActive_ = true;
    }

    /// Settle the strip once nothing has happened for a while:
    void settleStrip() {
        if (!stripActive_) return;
        // ELAPSED time, never a comparison of two absolute stamps:
        const uint32_t age = platform::millis() - stripWrittenMs_;
        if (age < kStripHoldMs) return;                        // still showing the change
        const char* name = deviceName();
        // Two holds after the change: the product, then the device. Beyond that, nothing to do.
        if (age < kStripHoldMs * 2)
            std::snprintf(display_, sizeof(display_), "MoonLight");
        else if (name && name[0])
            std::snprintf(display_, sizeof(display_), "%s", name);
        else
            std::snprintf(display_, sizeof(display_), "MoonLight");
    }

    /// The device's name, read through the control system rather than by reaching into SystemModule:
    static const char* deviceName() {
        const ControlDescriptor* c = findControl("System", "deviceName");
        return (c && c->ptr && c->type == ControlType::Text) ? static_cast<const char*>(c->ptr)
                                                             : nullptr;
    }

    /// How long the strip holds what it was told, before falling back to the device's name.
    static constexpr uint32_t kStripHoldMs = 5000;

    /// Write the last thing that happened onto the display strip.
    static bool paletteNameAt(uintptr_t optionsFn, uint8_t index, char* out, size_t outLen) {
        JsonSink sink(out, outLen);
        sink.requestName(index);
        reinterpret_cast<PaletteOptionsFn>(optionsFn)(sink);
        return out[0] != 0 && !sink.overflowed();
    }

    /// Write the last change onto the strip, an option's name rather than its index.
    void showOnStrip(const char* module, const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        MoonModule* target = sched ? sched->firstByName(module) : nullptr;
        if (!target) return;
        const ControlList& cs = target->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, control) != 0) continue;
            // A Select carries its options in the descriptor:
            const bool isSelect = cs[i].type == ControlType::Select && cs[i].aux;
            char paletteName[24] = {};
            // "control value", with the name first:
            if (isSelect && value < cs[i].max) {
                const auto* opts = reinterpret_cast<const char* const*>(cs[i].aux);
                writeStrip("%s %s", control, opts[value]);
            } else if (cs[i].type == ControlType::Palette && cs[i].aux && value < cs[i].max
                       && paletteNameAt(cs[i].aux, value, paletteName, sizeof(paletteName))) {
                // The NAME, not "37":
                writeStrip("%s %s", control, paletteName);
            } else {
                writeStrip("%s %u", control, static_cast<unsigned>(value));
            }
            return;
        }
    }

    /// Reorder:
    bool moveListRow(uint32_t id, uint8_t to) override {
        if (to >= kMaxPresets) return false;
        Preset* moving = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++) if (presets_[i].id == id) { moving = &presets_[i]; break; }
        if (!moving) return false;
        if (moving->slot == to) return true;

        Preset* occupant = nullptr;
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].slot == to && presets_[i].id != id) { occupant = &presets_[i]; break; }

        const uint8_t from = moving->slot;
        moving->slot = to;
        if (occupant) occupant->slot = from;    // swap rather than overwrite
        // Only the one or two presets whose slot changed get rewritten:
        const bool movedOk = writeSlot(*moving);
        const bool occupantOk = !occupant || writeSlot(*occupant);
        if (!movedOk || !occupantOk) {
            moving->slot = from;
            if (occupant) occupant->slot = to;
            if (movedOk) writeSlot(*moving);            // undo the half that did land
            setStatusf(Severity::Error, "could not save the new pad order");
            sortBySlot();
            return false;
        }
        presetsRevision_++;   // the surface changed: consumers caching the list must re-read
        sortBySlot();
        return true;
    }

    /// The row's editable fields carry the two actions a preset row needs.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].id != id) continue;
            // `activate` is the pad click and `apply` the row button, one action from two views.
            if (std::strcmp(field, "activate") == 0 || std::strcmp(field, "apply") == 0)
                return applyPreset(presets_[i].name);
            if (std::strcmp(field, "name") == 0) {
                char newName[kMaxNameLen] = {};
                mm::json::parseString(valueJson, "value", newName, sizeof(newName));
                return renamePreset(presets_[i].name, newName);
            }
            return false;
        }
        return false;
    }

private:
    struct Preset {
        uint32_t id = 0;
        char name[kMaxNameLen] = {};
        char captures[64] = {};   // what the file says it carries, shown per row
        uint8_t slot = 0;         // position on the grid (0..kMaxPresets-1), persisted in the file
        bool hasSlot = false;     // false for a file with no stored slot: assignFreeSlots places it
    };

    /// Re-read the folder.
    void rescan() {
        // Every change to the preset SET funnels through here (save, delete, rename, boot), so this.
        presetsRevision_++;
        presetCount_ = 0;
        platform::fsList(kPresetDir, &onEntry, this);
        for (uint8_t i = 0; i < presetCount_; i++) readCaptures(presets_[i]);
        assignFreeSlots();
        sortBySlot();
    }

    /// A preset with no stored slot (saved before slots existed, or copied in by hand) takes the.
    void assignFreeSlots() {
        bool taken[kMaxPresets] = {};
        for (uint8_t i = 0; i < presetCount_; i++)
            if (presets_[i].hasSlot) taken[presets_[i].slot] = true;
        for (uint8_t i = 0; i < presetCount_; i++) {
            if (presets_[i].hasSlot) continue;
            for (uint8_t s = 0; s < kMaxPresets; s++)
                if (!taken[s]) { presets_[i].slot = s; taken[s] = true; break; }
        }
    }

    void sortBySlot() {
        for (uint8_t i = 1; i < presetCount_; i++) {
            Preset key = presets_[i];
            int j = i - 1;
            while (j >= 0 && presets_[j].slot > key.slot) { presets_[j + 1] = presets_[j]; j--; }
            presets_[j + 1] = key;
        }
    }

    static void onEntry(const char* name, bool isDir, uint32_t /*size*/, void* user) {
        auto* self = static_cast<ControlModule*>(user);
        if (isDir || self->presetCount_ >= kMaxPresets) return;
        const size_t len = std::strlen(name);
        if (len < 6 || std::strcmp(name + len - 5, ".json") != 0) return;   // only our files
        // Skip a name too long to hold, rather than truncating it:
        const size_t stem = len - 5;
        if (stem >= sizeof(Preset::name)) return;
        Preset& p = self->presets_[self->presetCount_];
        std::memcpy(p.name, name, stem);
        p.name[stem] = '\0';
        p.id = ++self->nextId_;
        self->presetCount_++;
    }

    /// Drop any active-role claim held by `name`, called when its file goes away.
    void clearCurrentIfNamed(const char* name) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], name) == 0) current_[i][0] = '\0';
    }

    /// Move an active-role claim to a preset's new name, so a rename does not silently unlight it.
    void renameCurrent(const char* from, const char* to) {
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (std::strcmp(current_[i], from) == 0)
                std::snprintf(current_[i], sizeof(current_[i]), "%s", to);
    }

    /// A preset name becomes a FILE name, so it must not be able to steer the path.
    static bool validPresetName(const char* value) {
        if (!value) return false;
        const size_t n = std::strlen(value);
        if (n == 0 || n >= kMaxNameLen) return false;
        for (size_t i = 0; i < n; i++) {
            const unsigned char b = static_cast<unsigned char>(value[i]);
            if (b < 0x20 || b > 0x7E) return false;          // printable ASCII only
            if (b == '/' || b == '\\' || b == '.') return false;   // no separator, no dot-segment
        }
        return true;
    }

    /// Read just the `captures` header so a row can say what it carries without loading the body.
    void readCaptures(Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        char head[192] = {};
        const int n = platform::fsReadAt(path, 0, head, sizeof(head) - 1);
        if (n <= 0) return;
        head[n] = '\0';
        mm::json::parseString(head, "captures", p.captures, sizeof(p.captures));
        p.hasSlot = mm::json::hasKey(head, "slot");
        const int slot = mm::json::parseInt(head, "slot");
        p.slot = (p.hasSlot && slot >= 0 && slot < kMaxPresets) ? static_cast<uint8_t>(slot) : 0;
    }

    /// Persist the pad order by stamping each file with its position.
    bool writeSlot(const Preset& p) {
        char path[128];
        pathFor(p.name, path, sizeof(path));
        const long size = platform::fsSize(path);
        if (size <= 0) return false;
        char* body = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!body) return false;
        bool ok = false;
        const int n = platform::fsRead(path, body, static_cast<size_t>(size) + 1);
        if (n > 0) {
            body[n] = '\0';
            JsonSink sink;
            // Replace the leading brace, the slot being a header field like the captures one.
            sink.appendf("{\"slot\":%u,", static_cast<unsigned>(p.slot));
            const char* rest = std::strchr(body, '{');
            if (rest) {
                const char* after = rest + 1;
                // Drop any previous slot key so repeated reorders do not accumulate them.
                if (std::strncmp(after, "\"slot\":", 7) == 0) {
                    const char* comma = std::strchr(after, ',');
                    if (comma) after = comma + 1;
                }
                sink.append(after);
                if (!sink.overflowed())
                    ok = platform::fsWriteAtomic(path, sink.data(), sink.size());
            }
        }
        platform::free(body);
        return ok;
    }

    static void pathFor(const char* name, char* out, size_t n) {
        std::snprintf(out, n, "%s/%s.json", kPresetDir, name);
    }

    /// Capture the selected subtrees into one file.
    void savePreset() {
        if (name_[0] == 0) { setStatusf(Severity::Warning, "name the preset first"); return; }
        // Flush first:
        FilesystemModule::flushPending();

        auto* fs = FilesystemModule::instance();
        auto* sched = Scheduler::instance();
        if (!fs || !sched) { setStatusf(Severity::Error, "not ready"); return; }

        JsonSink sink;
        // A save aimed at a pad carries its slot in the file, exactly as writeSlots writes it, so.
        if (captureRole_ >= kCaptureCount) { setStatusf(Severity::Warning, "choose what to capture"); return; }
        // A pad can hold one preset:
        if (saveSlot_ < kMaxPresets) {
            for (uint8_t i = 0; i < presetCount_; i++) {
                if (presets_[i].slot != saveSlot_) continue;
                if (std::strcmp(presets_[i].name, name_) != 0) {
                    setStatusf(Severity::Warning, "pad %u is taken by %s",
                               static_cast<unsigned>(saveSlot_ + 1), presets_[i].name);
                    return;
                }
                break;
            }
        }
        const char* type = kCapturable[captureRole_];
        sink.append("{");
        if (saveSlot_ < kMaxPresets)   // anything else (incl. kNoSlot) means "no pad was chosen"
            sink.appendf("\"slot\":%u,", static_cast<unsigned>(saveSlot_));
        sink.appendf("\"captures\":\"%s\"", type);

        // Each captured subtree is written under a type-name prefix into one flat object.
        MoonModule* m = findTopLevel(sched, type);
        if (!m) { setStatusf(Severity::Error, "%s is not on this device", type); return; }
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "%s.", type);
        sink.append(",");
        if (!fs->saveSubtreeTo(m, sink, prefix)) { setStatusf(Severity::Error, "out of memory saving"); return; }
        sink.append("}");
        if (sink.overflowed()) { setStatusf(Severity::Error, "out of memory saving"); return; }

        char path[128];
        pathFor(name_, path, sizeof(path));
        const bool ok = platform::fsWriteAtomic(path, sink.data(), sink.size());
        setStatusf(ok ? Severity::Status : Severity::Error,
                   ok ? "saved %s" : "could not save %s", name_);
        rescan();   // the file carries its slot, so this places it on the clicked pad
    }

    /// Put the device into a preset's state.
    bool applyPreset(const char* presetName) {
        auto* fs = FilesystemModule::instance();
        auto* sched = Scheduler::instance();
        if (!fs || !sched) return false;

        char path[128];
        pathFor(presetName, path, sizeof(path));
        const long size = platform::fsSize(path);
        if (size <= 0) { setStatusf(Severity::Error, "%s is missing", presetName); return false; }
        char* body = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!body) { setStatusf(Severity::Error, "out of memory applying"); return false; }
        const int n = platform::fsRead(path, body, static_cast<size_t>(size) + 1);
        if (n <= 0) { platform::free(body); setStatusf(Severity::Error, "could not read %s", presetName); return false; }
        body[n] = '\0';

        char captures[64] = {};
        mm::json::parseString(body, "captures", captures, sizeof(captures));

        // Apply every captured subtree, THEN prepare once.
        uint8_t role = kCaptureCount, roleCount = 0;
        for (uint8_t i = 0; i < kCaptureCount; i++)
            if (listHas(captures, kCapturable[i])) { role = i; roleCount++; }
        if (roleCount != 1) {
            platform::free(body);
            setStatusf(Severity::Warning, roleCount ? "%s carries several roles, re-save it"
                                                    : "%s carries nothing this build knows", presetName);
            return false;
        }
        const char* type = kCapturable[role];
        MoonModule* m = findTopLevel(sched, type);
        if (!m) {
            platform::free(body);
            setStatusf(Severity::Warning, "%s needs %s, which this device does not have", presetName, type);
            return false;
        }
        char prefix[24];
        std::snprintf(prefix, sizeof(prefix), "%s.", type);
        const bool applied = fs->applySubtree(m, body, prefix);
        platform::free(body);
        if (!applied) { setStatusf(Severity::Error, "could not apply %s", presetName); return false; }

        sched->prepareTree();
        // The preset now holds its role; the other three keep whoever held them, so a layout preset.
        std::snprintf(current_[role], sizeof(current_[role]), "%s", presetName);
        setSurfaceStatusf("applied %s", presetName);
        return true;
    }

    bool renamePreset(const char* from, const char* to) {
        if (!validPresetName(to)) return false;   // the new name becomes a file name (see validPresetName)
        if (std::strcmp(from, to) == 0) return true;   // renaming to itself is a no-op, not a failure
        char src[128], dst[128];
        pathFor(from, src, sizeof(src));
        pathFor(to, dst, sizeof(dst));
        // Refuse a rename onto an existing preset:
        if (platform::fsSize(dst) >= 0) {
            setStatusf(Severity::Warning, "%s already exists", to);
            return false;
        }
        const long size = platform::fsSize(src);
        if (size <= 0) return false;
        char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
        if (!buf) return false;
        const int n = platform::fsRead(src, buf, static_cast<size_t>(size) + 1);
        bool ok = false;
        if (n > 0 && platform::fsWriteAtomic(dst, buf, static_cast<size_t>(n))) {
            // The destination now exists; the rename is only complete once the source is gone.
            if (platform::fsRemove(src)) {
                renameCurrent(from, to);   // the active look follows its new name
                ok = true;
            } else {
                platform::fsRemove(dst);   // roll back, so a half-rename never ships
            }
        }
        platform::free(buf);
        rescan();
        return ok;
    }

    /// Is `type` in the comma-separated `captures` header? Whole-token match, so "Layer" never.
    static bool listHas(const char* list, const char* type) {
        const size_t tlen = std::strlen(type);
        for (const char* p = list; *p;) {
            const char* end = std::strchr(p, ',');
            const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
            if (len == tlen && std::strncmp(p, type, tlen) == 0) return true;
            if (!end) break;
            p = end + 1;
        }
        return false;
    }

    static MoonModule* findTopLevel(Scheduler* s, const char* typeName) {
        for (uint8_t i = 0; i < s->moduleCount(); i++) {
            MoonModule* m = s->module(i);
            if (m && std::strcmp(m->typeName(), typeName) == 0) return m;
        }
        return nullptr;
    }

    /// Report through the base's status, the way every module does, so the UI shows it in the.
    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, ap);
        va_end(ap);
        setStatus(statusBuf_, sev);
    }

    /// Report a SURFACE action:
    void setSurfaceStatusf(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusBuf_, sizeof(statusBuf_), fmt, ap);
        va_end(ap);
        // The STRIP only, not the status row:
        writeStrip("%s", statusBuf_);
    }

    /// Fader names are their position, so a surface binds to "fader1" rather than to a label a.
    static constexpr const char* kFaderNames[kFaderCount] =
        {"fader1", "fader2", "fader3", "fader4", "fader5", "fader6", "fader7", "fader8"};
    static constexpr const char* kEncoderNames[kEncoderCount] =
        {"encoder1", "encoder2", "encoder3", "encoder4", "encoder5", "encoder6", "encoder7", "encoder8"};
    ActiveInstance<ControlModule> seat_{*this};

    /// Attached surfaces. A small fixed array: a rig has a desk and a phone, not thirty.
    static constexpr uint8_t kMaxSurfaces = 4;
    ControlSurface* surfaces_[kMaxSurfaces] = {};
    uint8_t surfaceCount_ = 0;
    /// The last value KNOWN to a surface, so only changes go out.
    uint8_t sentSwitches_[kSwitchCount] = {};
    uint8_t sentFaders_[kFaderCount] = {};
    /// One bit per control, per bank: a hand is on it. See setTouched.
    uint32_t touchedSwitches_ = 0, touchedEncoders_ = 0, touchedFaders_ = 0;

    uint32_t* touchMask(SurfaceControl kind) {
        switch (kind) {
            case SurfaceControl::Switch:  return &touchedSwitches_;
            case SurfaceControl::Encoder: return &touchedEncoders_;
            case SurfaceControl::Fader:   return &touchedFaders_;
            default: return nullptr;   // a pad has no travel to fight over
        }
    }

    /// Push one control if it changed and no hand is on it.
    void mirrorOne(SurfaceControl kind, uint8_t index, uint8_t value, uint8_t& sent) {
        if (value == sent) return;
        const uint32_t* mask = touchMask(kind);
        if (mask && index < 32 && (*mask & (1u << index))) return;
        for (uint8_t s = 0; s < surfaceCount_; s++) surfaces_[s]->sendValue(kind, index, value);
        sent = value;
    }

    static constexpr const char* kSwitchNames[kSwitchCount] =
        {"switch1", "switch2", "switch3", "switch4", "switch5", "switch6", "switch7", "switch8"};
    char     display_[32] = "MoonLight";   ///< the strip: what the surface last touched, in words
    /// WHEN the strip was last written.
    uint32_t stripWrittenMs_ = 0;
    /// Whether a settle sequence is running.
    bool     stripActive_ = true;
    uint8_t faders_[kFaderCount] = {};
    uint8_t encoders_[kEncoderCount] = {};
    /// What each attached surface was last SENT, so a value it already has is not echoed back.
    uint8_t sentEncoders_[kEncoderCount] = {};

    /// What each surface control drives, as "Module.control", empty when unassigned.
    static constexpr uint8_t kTargetLen = 40;
    /// The control NAMES for those assignments ("fader1Target"), built once and borrowed by the.
    static constexpr uint8_t kTargetNameLen = 20;
    char targetNames_[kSwitchCount + kEncoderCount + kFaderCount][kTargetNameLen] = {};
    char faderTargets_[kFaderCount][kTargetLen]     = {"Drivers.brightness"};
    char switchTargets_[kSwitchCount][kTargetLen]   = {"Drivers.on"};
    char encoderTargets_[kEncoderCount][kTargetLen] = {"Drivers.palette"};
    /// bool, not uint8:
    bool switches_[kSwitchCount] = {};
    /// Which pad the next save fills, set by the surface popup.
    static constexpr uint8_t kNoSlot = 0xFF;
    uint8_t saveSlot_ = kNoSlot;

    Preset presets_[kMaxPresets];
    uint8_t presetCount_ = 0;
    uint32_t presetsRevision_ = 0;   ///< see presetsRevision(): drives HA's preset re-fetch
    uint32_t nextId_ = 0;
    char name_[kMaxNameLen] = {};
    /// Which ONE subtree the next save captures, as an index into kCapturable.
    uint8_t captureRole_ = kEffectsRole;   // a look, by default
    /// Which preset currently holds each capturable role, index-aligned with kCapturable.
    char current_[kCaptureCount][kMaxNameLen] = {};
    /// Backing store for the status text: setStatus borrows the pointer, so it must outlive the call.
    char statusBuf_[64] = {};
};

}  // namespace mm
