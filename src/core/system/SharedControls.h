#pragma once

#include "core/module/MoonModule.h"
#include "core/system/ControlModule.h"   // kTargetLen and kMaxNameLen: a parameter is a slot's target, a look a preset's name
#include "core/util/Hlc.h"             // Stamp: when and by whom a shared value was written
#include "core/util/ScratchBuffer.h"   // the values and the recalls, grown as they are shared

#include <cstddef>
#include <cstdint>

namespace mm {

namespace osc { class Reader; }

/// What a device group's members share: each parameter their Control slots drive, by name with its value and stamp, and the look recalled last.
///
/// @technical
///
/// **A value is a last-writer-wins register** (Shapiro et al., 2011), keyed by the group and the parameter's name (`Drivers.brightness`): a received value applies when its stamp is newer than the one held, so two changes at once end on one value on every member and a late repeat cannot undo a newer change.
///
/// **A look travels as its name.** The recall carries the preset's name, its stamp and where its maker serves it; a member fetches the document over HTTP and applies it live, once per stamp, releasing it at once.
///
/// **The trust boundary.** Membership lets a datagram set controls, so a member applies only what an effects preset may set, the Home Assistant rule: a control of an effect, a modifier or a layer, or the Drivers card's own, of a value type.
///
/// A pin, a network setting, a service or a system module stays as it is, and a message merges at most kPerMessage values. Every value is held all the same, so the members' digests agree.
///
/// **Groups do not bridge.** A parameter two groups both drive is held per group, and a value one group set stays in that group, so a change crosses from one group to the other only when this device's own slot moves it.
///
/// **Bounded, newest kept.** kMaxValues values across the groups: a full table drops its oldest-stamped value for a newer one, so every member converges on the same newest values, and `note` says the table is full. kMaxRecalls recalls, one per group.
class SharedControls {
public:
    /// The most shared values held, across every group: the 24 slots of one device and those of members that drive other parameters.
    static constexpr uint8_t kMaxValues = 48;
    /// The longest parameter name, "Module.control", a Control slot's target.
    static constexpr uint8_t kParamLen = ControlModule::kTargetLen;
    /// The longest look name, a preset's name.
    static constexpr uint8_t kNameLen = ControlModule::kMaxNameLen;
    /// The most recalls held, one per group.
    static constexpr uint8_t kMaxRecalls = 8;
    /// Values a message carries, about 1 KB.
    static constexpr uint8_t kPerMessage = 16;
    /// The OSC address of the shared values.
    static constexpr const char* kValuesAddress = "/moonlight/devicegroup/control";
    /// The OSC address of a look's recall.
    static constexpr const char* kLookAddress = "/moonlight/devicegroup/look";

    /// Its buffers belong to `owner`, Discovery, which frees them on release.
    explicit SharedControls(MoonModule& owner) : values_(owner), recalls_(owner) {}

    /// The key a group's values are held under: its id hashed.
    static uint32_t keyOf(const char* groupId);

    /// Record a change made here to `param` in group `group`, stamped `s`; false when no room holds it.
    bool setLocal(uint32_t group, const char* param, int32_t value, const Stamp& s);
    /// Whether any group holds `value` for `param`: a value a group set here, which is not sent back.
    bool holds(const char* param, int32_t value) const;
    /// Write group `groupId`'s values from `from` into one message, `from` moved past them; only `param` when it is given.
    size_t writeValues(uint8_t* out, size_t cap, const char* groupId, uint16_t& from, const char* param = nullptr) const;
    /// Merge a values message, its group id read: each newer value held and applied by name. False when the sender holds other values, which Trickle answers.
    bool mergeValues(osc::Reader& r, uint32_t group, Hlc* clock);
    /// An order-independent digest of group `group`'s values: equal on two devices exactly when they hold the same ones.
    uint32_t digest(uint32_t group) const;
    /// How many values are held, across the groups.
    uint8_t valueCount() const { return valueCount_; }

    /// Record a look applied here, served at `ip`:`port`, stamped `s`.
    void recallLocal(uint32_t group, const char* name, const uint8_t ip[4], uint16_t port, const Stamp& s);
    /// Write group `groupId`'s recall into one message; 0 when it has none.
    size_t writeRecall(uint8_t* out, size_t cap, const char* groupId) const;
    /// Merge a recall, its group id read: a newer one is fetched and applied. False when the sender's is older, which Trickle answers.
    bool mergeRecall(osc::Reader& r, uint32_t group, Hlc* clock);
    /// Every 20 ms: start fetching the newest recall not applied, then apply it, the request on JsonFetch's task. Its answer is taken in by JsonFetch's once-a-second poll, so a look follows within about a second.
    void tickFetch();

    /// The control `param` names when a group may set it (the trust boundary above), its module written to `module` and its name to `control`; null otherwise.
    static const ControlDescriptor* shareable(const char* param, char (&module)[24], const char*& control);

    /// What the group's status adds: the values past the bound, or a look that could not be fetched; null when all is well.
    const char* note() const MM_NONBLOCKING { return full_ ? "shared controls full" : (fetchNote_[0] ? fetchNote_ : nullptr); }

    /// Drop every value and recall, as the buffers go on release.
    void clear();

private:
    /// One shared value.
    struct Value {
        uint32_t group = 0;
        char     param[kParamLen] = {};
        int32_t  value = 0;
        Stamp    stamp{};
    };
    /// One group's last recall.
    struct Recall {
        uint32_t group = 0;
        char     name[kNameLen] = {};
        uint8_t  ip[4] = {};   ///< where its maker serves the preset
        uint16_t port = 0;
        Stamp    stamp{};
        bool     applied = false;   ///< fetched and applied, or made here
    };

    Value* find(uint32_t group, const char* param);
    /// The value with the oldest stamp, or null when none is held.
    Value* oldest();
    /// A place for `param`: a new one while room allows, else the oldest-stamped value's when `s` is newer; null otherwise.
    Value* add(uint32_t group, const char* param, const Stamp& s);
    Recall* recallOf(uint32_t group);
    /// Merge one received value: held when newer, applied by name.
    void mergeOne(uint32_t group, const char* param, int32_t value, const Stamp& s);
    /// Set `param` ("Module.control") to `value` through the control path, as a click does.
    static void apply(const char* param, int32_t value);
    /// The URL a recall's preset is served at.
    static bool urlOf(const Recall& r, char* out, size_t cap);
    void startFetch();
    void advanceFetch();
    void endFetch(const char* failed);

    ScratchBuffer<Value> values_;
    ScratchBuffer<Recall> recalls_;
    uint8_t valueCount_ = 0, recallCount_ = 0;
    bool full_ = false;
    int fetchHandle_ = 0, applyHandle_ = 0;   ///< the document on its way, then its apply
    char fetchName_[kNameLen] = "";           ///< its name when the fetch began, which a newer recall may replace meanwhile
    char fetchNote_[48] = "";
};

}  // namespace mm
