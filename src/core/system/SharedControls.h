#pragma once

#include "core/module/MoonModule.h"
#include "core/system/ControlModule.h"   // kTargetLen and kMaxNameLen: a parameter is a slot's target, a look a preset's name
#include "core/util/Hlc.h"             // Stamp: when and by whom a shared value was written
#include "core/util/ScratchBuffer.h"   // the values and the looks, grown as they are shared

#include <cstddef>
#include <cstdint>

namespace mm {

namespace osc { class Reader; class Writer; }

/// What a device group's members share: each parameter their Control slots drive, by name with its value and stamp, and the look applied last.
///
/// @technical
///
/// **A value is a last-writer-wins register** (Shapiro et al., 2011), keyed by the group and the parameter's name (`Drivers.brightness`): a received value applies when its stamp is newer than the one held, so two changes at once end on one value on every member and a late repeat cannot undo a newer change.
///
/// **A look is pushed whole** (Deluge, Hui and Culler 2004; NACK repair as NORM's, RFC 5740). The maker multicasts the preset's document in numbered chunks with its name, stamp, size, digest and the fleet time it applies at.
///
/// The Trickle repeats advertise each member's stamp and the chunks it holds; a member short of chunks asks for them, and any member holding them answers at its own offset, quiet when it hears them sent first. Complete and its digest matching, the look applies live at its time.
///
/// One mechanism for any size up to kMaxChunks chunks, no request to the maker, and every member that is up ends with the whole look.
///
/// **The trust boundary.** Membership lets a datagram set controls, so a member applies only what an effects preset may set, the Home Assistant rule: a control of an effect, a modifier or a layer, or the Drivers card's own, of a value type.
///
/// A pin, a network setting, a service or a system module stays as it is, and a message merges at most kPerMessage values. Every value is held all the same, so the members' digests agree.
///
/// A look is checked whole once its chunks arrive: a document naming a module outside that rule, or a driver of the Drivers card, is held and left unapplied, and `note` says so.
///
/// **Groups do not bridge.** A parameter two groups both drive is held per group, and a value one group set stays in that group, so a change crosses from one group to the other only when this device's own slot moves it.
///
/// **Bounded per group, newest kept.** Each group holds at most kMaxPerGroup values, and a full group drops its own oldest-stamped value for a newer one, so the members of one group hold the same newest values whatever other groups each is in; `note` says when a group is full. At most kMaxGroups groups, each with one look; a group that leaves this device takes its values and look with it.
class SharedControls {
public:
    /// The most values one group holds: the 24 slots of one device and those of members that drive other parameters.
    static constexpr uint8_t kMaxPerGroup = 32;
    /// The most groups whose values and look are held.
    static constexpr uint8_t kMaxGroups = 8;
    /// The longest parameter name, "Module.control", a Control slot's target.
    static constexpr uint8_t kParamLen = ControlModule::kTargetLen;
    /// The longest look name, a preset's name.
    static constexpr uint8_t kNameLen = ControlModule::kMaxNameLen;
    /// Values a message carries, about 1 KB.
    static constexpr uint8_t kPerMessage = 16;
    /// The OSC address of the shared values.
    static constexpr const char* kValuesAddress = "/moonlight/devicegroup/control";
    /// The bytes of a look one chunk carries, so a chunk fits one datagram with its header.
    static constexpr uint16_t kChunkBytes = 1024;
    /// The most chunks a look has, which bounds its size: a preset is a few hundred bytes to a few KB.
    static constexpr uint8_t kMaxChunks = 32;
    /// How long a member short of chunks waits before it asks again, plus its offset.
    static constexpr uint32_t kNeedEveryMs = 1000;
    /// The span the members' answers and requests spread over, each device at its own offset in it.
    static constexpr uint32_t kSpreadMs = 200;
    /// Chunks one 20 ms read sends, so a burst does not overrun a member's receive queue.
    static constexpr uint8_t kChunksPerRead = 4;
    /// The OSC address of a look's advertisement: its name, stamp, size, digest and the chunks held.
    static constexpr const char* kLookAddress = "/moonlight/devicegroup/look";
    /// The OSC address of one chunk of a look.
    static constexpr const char* kChunkAddress = "/moonlight/devicegroup/look/chunk";
    /// The OSC address of a request for the chunks a member lacks.
    static constexpr const char* kNeedAddress = "/moonlight/devicegroup/look/need";
    /// How many kinds of shared message there are, and their addresses, in the order merge() takes them.
    static constexpr uint8_t kKinds = 4;
    static constexpr const char* kAddresses[kKinds] = {kValuesAddress, kLookAddress, kChunkAddress, kNeedAddress};

    /// Its buffers belong to `owner`, Discovery, which frees them on release.
    explicit SharedControls(MoonModule& owner) : values_(owner), looks_(owner) {}

    /// The key a group's values are held under: its id hashed.
    static uint32_t keyOf(const char* groupId);

    /// Record a change made here to `param` in group `group`, stamped `s`; false when no room holds it.
    bool setLocal(uint32_t group, const char* param, int32_t value, const Stamp& s);
    /// Whether any group holds `value` for `param`: a value a group set here, which is not sent back.
    bool holds(const char* param, int32_t value) const;
    /// Write group `groupId`'s values from `from` into one message, `from` moved past them; only `param` when it is given.
    size_t writeValues(uint8_t* out, size_t cap, const char* groupId, uint16_t& from, const char* param = nullptr) const;
    /// Merge a message of kind `kind` (an index into kAddresses), its group id read. False when this device holds newer, which Trickle answers.
    bool merge(uint8_t kind, osc::Reader& r, uint32_t group, Hlc* clock);
    /// Merge a values message, its group id read: each newer value held and applied by name. False when the sender holds other values, which Trickle answers.
    bool mergeValues(osc::Reader& r, uint32_t group, Hlc* clock);
    /// An order-independent digest of group `group`'s values: equal on two devices exactly when they hold the same ones.
    uint32_t digest(uint32_t group) const;
    /// How many values are held, across the groups.
    uint16_t valueCount() const { return valueCount_; }

    /// Record look `name` applied here, its document `doc` (heap, taken over), stamped `s`, applying on the members at fleet time `atUs` (0: on arrival); its chunks go out on the next reads.
    void lookLocal(uint32_t group, const char* name, char* doc, const Stamp& s, int64_t atUs);
    /// Write group `groupId`'s look advertisement into one message; 0 when it has none.
    size_t writeLook(uint8_t* out, size_t cap, const char* groupId) const;
    /// Merge an advertisement, its group id read: a newer look is taken and its chunks asked for. False when this device holds a newer one, which Trickle answers.
    bool mergeLook(osc::Reader& r, uint32_t group, Hlc* clock);
    /// Merge one chunk, its group id read: a newer look starts with it, a chunk of the held one fills its place.
    void mergeChunk(osc::Reader& r, uint32_t group, Hlc* clock);
    /// Merge a request for chunks, its group id read: the ones held go out at this device's offset.
    void mergeNeed(osc::Reader& r, uint32_t group);
    /// The next message group `groupId` has due at `nowMs`, a chunk to send or a request for missing ones; 0 when none.
    size_t writeDue(uint8_t* out, size_t cap, const char* groupId, uint32_t nowMs);
    /// Every 20 ms: apply each complete look whose time has come, at fleet time `fleetNowUs`, holding one at most `maxHoldUs` ahead while `synced`.
    void tickLooks(int64_t fleetNowUs, bool synced, int64_t maxHoldUs);

    /// The control `param` names when a group may set it (the trust boundary above), its module written to `module` and its name to `control`; null otherwise.
    static const ControlDescriptor* shareable(const char* param, char (&module)[24], const char*& control);

    /// What the group's status adds: the values past the bound, or a look refused; null when all is well.
    const char* note() const MM_NONBLOCKING { return full_ ? "shared controls full" : (lookNote_[0] ? lookNote_ : nullptr); }

    /// Drop every value and look, as the buffers go on release.
    void clear();

    /// Drop the values and look of each group `held(key)` answers false for: a group that left this device.
    template <class Held>
    void retain(Held&& held) {
        uint16_t w = 0;
        for (uint16_t i = 0; i < valueCount_; i++) if (held(values_[i].group)) values_[w++] = values_[i];
        valueCount_ = w;
        uint8_t r = 0;
        for (uint8_t i = 0; i < lookCount_; i++) {
            if (held(looks_[i].group)) looks_[r++] = looks_[i];
            else freeText(looks_[i]);
        }
        lookCount_ = r;
        refreshFull();
    }

private:
    /// One shared value.
    struct Value {
        uint32_t group = 0;
        char     param[kParamLen] = {};
        int32_t  value = 0;
        Stamp    stamp{};
    };
    /// One group's last look, whole or filling in.
    struct Look {
        uint32_t group = 0;
        char     name[kNameLen] = {};
        Stamp    stamp{};
        uint32_t size = 0;       ///< the document's bytes
        uint32_t digest = 0;     ///< FNV-1a of the document, checked once it is whole
        uint32_t have = 0;       ///< the chunks held, a bit each
        uint32_t send = 0;       ///< the chunks to send, a bit each
        uint32_t sendDueMs = 0;  ///< platform::millis() at which they go
        uint32_t needDueMs = 0;  ///< platform::millis() at which a member short of chunks asks; 0 while it has them all
        int64_t  atUs = 0;       ///< the fleet time it applies at; 0 on completion
        char*    text = nullptr; ///< the document, size bytes and a terminator, on the heap
        bool     applied = false;
    };

    Value* find(uint32_t group, const char* param);
    /// Group `group`'s value with the oldest stamp, and how many values it holds; null when none.
    Value* oldest(uint32_t group, uint16_t& count);
    /// How many groups hold values.
    uint8_t groupCount() const;
    /// Whether a group holds its bound of values, which `note` reports.
    void refreshFull();
    /// A place for `param`: a new one while room allows, else the oldest-stamped value's when `s` is newer; null otherwise.
    Value* add(uint32_t group, const char* param, const Stamp& s);
    /// Group `group`'s look, a new place for it when `make` and room allows; null otherwise.
    Look* lookOf(uint32_t group, bool make);
    /// A look's header as a message carries it.
    struct LookHead { const char* name = ""; Stamp stamp{}; uint32_t size = 0, digest = 0; };
    /// Write a look's header: the group's id, its name, stamp, size and digest.
    static void writeHead(osc::Writer& w, const char* groupId, const Look& l);
    /// Read a look's header; false when malformed or past kMaxChunks chunks.
    static bool readHead(osc::Reader& r, LookHead& h);
    /// One chunk's part of a message: the fleet time it applies at, its index and its bytes in the datagram.
    struct Chunk { int64_t atUs = 0; uint32_t index = 0; const uint8_t* data = nullptr; size_t n = 0; };
    /// Read a chunk message's header and chunk; false unless the chunk is one of the look's, whole.
    static bool readChunk(osc::Reader& r, LookHead& h, Chunk& c);
    /// Start `l` afresh as the look `h` names, its chunks to come; false, the slot left empty, when memory refuses its document.
    bool restart(Look& l, const LookHead& h);
    /// Whether a chunk of the look `h` names belongs in `l`: the held look's, or a newer one's, which `l` restarts as.
    bool accepts(Look& l, const LookHead& h);
    /// Take chunk `index` of `l`, `n` bytes; once whole, check its digest.
    void fill(Look& l, uint32_t index, const uint8_t* data, size_t n);
    /// Whether `l` is whole and its time has come at fleet time `fleetNowUs`.
    static bool due(const Look& l, int64_t fleetNowUs, bool synced, int64_t maxHoldUs);
    static void freeText(Look& l);
    /// How many chunks a look of `size` bytes has, and the bits of all of them.
    static uint8_t chunksOf(uint32_t size) { return static_cast<uint8_t>((size + kChunkBytes - 1) / kChunkBytes); }
    static uint32_t allOf(uint32_t size) { const uint8_t n = chunksOf(size); return n >= 32 ? ~0u : (1u << n) - 1u; }
    /// Merge one received value: held when newer, applied by name.
    void mergeOne(uint32_t group, const char* param, int32_t value, const Stamp& s);
    /// Set `param` ("Module.control") to `value` through the control path, as a click does.
    static void apply(const char* param, int32_t value);
    /// This device's offset in kSpreadMs, from its MAC: different per member, the same each time.
    static uint32_t spreadMs();

    ScratchBuffer<Value> values_;
    ScratchBuffer<Look> looks_;
    uint16_t valueCount_ = 0;
    uint8_t lookCount_ = 0;
    bool full_ = false;
    char lookNote_[48] = "";
};

}  // namespace mm
