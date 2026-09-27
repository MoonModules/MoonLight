#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"     // the singleton seat drivers resolve the library through
#include "core/util/ScratchBuffer.h"      // the dynamic (no-cap) role pool
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"           // restoreList: recursive reader for the persisted array
#include "light/drivers/ChannelRole.h"
#include "light/drivers/Correction.h"  // LightPreset + fillRolesFromPreset + the derived offsets

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace mm {

/// The reusable light-preset library, a Drivers submodule owning NAMED channel-role wirings, each editable in its own row and referenced by many drivers. A driver stores a preset's STABLE id and resolves it here into its own Correction, so a wiring is built once and reordering other presets disturbs no reference.
///
/// A curated set of real fixtures is seeded read-only on first boot. A user adds custom named wirings alongside them. The render loop never reads this module.
///
/// @moreinfo
///
/// ## What a preset is
///
/// A channel-role layout: role `r` at channel `i` says channel `i` of a light carries role `r`.
/// The color roles cover the strip orders, and the fixture roles cover pan, tilt and the rest.
///
/// ## Storage is uncapped
///
/// A preset is exactly as wide as its fixture. Role bytes live in one dynamic pool, each preset a slice, so a moving head declares as many channels as it has. The pool is touched on the cold path only, leaving it free to reallocate.
///
/// ## The editable-list primitive
///
/// The first consumer of `EditableListSource`. The whole add, delete, reorder and edit surface is reused rather than rebuilt. The per-row fields are the name, the channel count, and one role picker per channel.
///
/// @card lightpresets.png
class LightPresetsModule : public MoonModule, public ListSource {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Generic; }

    // A deleted library could never be re-added, and every driver resolves its preset through it.
    /// Not user-editable: every driver resolves its preset through this one boot-wired library.
    bool userEditable() const override { return false; }

    /// How many preset rows a device can hold.
    static constexpr uint8_t kMaxPresets = 32;   // bounded row count; a device won't wire more light types

    /// The boot library, which a driver resolves its preset reference through.
    static LightPresetsModule* active() { return ActiveInstance<LightPresetsModule>::active(); }

    /// The first preset's id: a safe default for a fresh driver or a dangling reference.
    uint32_t defaultId() const { return count_ ? presets_[0].id : 0; }

    /// How many presets the library holds, for a driver building its preset selector.
    uint8_t presetCount() const { return count_; }
    /// The name of preset row `i`, for a driver building its selector.
    const char* nameAt(uint8_t i) const { return i < count_ ? presets_[i].name : ""; }
    /// The stable id of preset row `i`, which is what a driver stores.
    uint32_t idAt(uint8_t i) const { return i < count_ ? presets_[i].id : 0; }
    /// The row currently holding `id`, for rendering a selector at its referenced preset.
    uint8_t indexOfId(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return i;
        return 0;   // a dangling id renders at the first preset; rebuildCorrection falls back to it too
    }

    /// Whether the preset carries `role`, for a caller that needs one emitter rather than any.
    bool presetHasRole(uint32_t id, ChannelRole role) const {
        const Preset* p = find(id);
        if (!p) return false;
        const uint8_t* r = roleAt(*p);
        for (uint8_t c = 0; c < p->channelCount; c++)
            if (static_cast<ChannelRole>(r[c]) == role) return true;
        return false;
    }

    // Any channel apply() synthesises from RGB counts, since one control governs them all.
    /// Whether the preset carries a channel the white mode would synthesise.
    bool presetHasSynthChannel(uint32_t id) const {
        const Preset* p = find(id);
        if (!p) return false;
        const uint8_t* r = roleAt(*p);
        for (uint8_t c = 0; c < p->channelCount; c++) {
            switch (static_cast<ChannelRole>(r[c])) {
                case ChannelRole::White:
                case ChannelRole::WarmWhite:
                case ChannelRole::Yellow:
                case ChannelRole::UV:
                    return true;
                default: break;
            }
        }
        return false;
    }

    /// Resolve a preset id into a driver's flat Correction; false leaves `out` untouched.
    bool deriveCorrection(uint32_t id, uint8_t brightness, Correction& out) const {
        const Preset* p = find(id);
        if (!p) return false;
        // The option indices are aligned with the enum, so a role byte IS a ChannelRole value.
        out.rebuild(brightness, reinterpret_cast<const ChannelRole*>(roleAt(*p)), p->channelCount);
        return true;
    }

    // Claimed at CONSTRUCTION: a driver resolves the library while building its own controls.
    /// Claim the singleton seat at construction, before any driver builds its selector.
    LightPresetsModule() { seat_.claim(); }

    /// Seed the curated built-ins when the set is empty.
    void setup() override {
        MoonModule::setup();
        if (count_ == 0) seedBuiltins();   // belt-and-braces; defineControls already seeds if empty
        refreshStatus();
    }

    /// Re-claim the singleton seat, which is a no-op while we already hold it.
    void prepare() override { seat_.claim(); }   // idempotent: no-op while we already hold the seat
    /// Vacate the singleton seat, then release the base.
    void release() override { seat_.vacate(); MoonModule::release(); }

    /// Bind the presets list, which the editable-list primitive renders.
    void defineControls() override {
        MoonModule::defineControls();
        // The persisted rows INCLUDE the built-ins, so restoring replaces rather than duplicates.
        if (count_ == 0) seedBuiltins();
        controls_.addList("presets", *this);   // this module is the (editable) ListSource
    }

    // --- ListSource (editable) ---------------------------------------------------------
    /// How many preset rows the list holds.
    uint8_t listRowCount() const override { return count_; }

    // This row IS the persisted form, so it must carry the preset's full wiring.
    /// Write one row, which is also the persisted form, so it carries the full wiring.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        const Preset& p = presets_[row];
        const uint8_t* roles = roleAt(p);
        // Escaped, not raw: a quote in a name would produce JSON that wipes every custom preset.
        sink.appendf("{\"id\":%lu,\"name\":", static_cast<unsigned long>(p.id));
        sink.writeJsonString(p.name);
        sink.appendf(",\"channels\":%u,\"roles\":[", static_cast<unsigned>(p.channelCount));
        for (uint8_t c = 0; c < p.channelCount; c++)
            sink.appendf("%s%u", c ? "," : "", static_cast<unsigned>(roles[c]));
        sink.append("]");
        if (p.locked) sink.append(",\"locked\":true");
        sink.append("}");
    }

    // Emitted ONCE per list: inlining would repeat the same array hundreds of times per push.
    /// Emit the shared channel-role option set every row's selectors reference.
    void writeListOptionSets(JsonSink& sink) const override {
        sink.append("\"channelRole\":[");
        for (uint8_t o = 0; o < kChannelRoleCount; o++)
            sink.appendf("%s\"%s\"", o ? "," : "", kChannelRoleOptions[o]);
        sink.append("]");
    }

    /// Write one row's editable fields: the name, the channel count, and a role per channel.
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        const Preset& p = presets_[row];
        const uint8_t* roles = roleAt(p);
        sink.append("{\"fields\":[");
        sink.append("{\"name\":\"name\",\"type\":\"text\",\"value\":");
        sink.writeJsonString(p.name);   // escape the value, same reason as writeListRow
        sink.append("},");
        sink.appendf("{\"name\":\"channels\",\"type\":\"uint8\",\"value\":%u,\"min\":1,\"max\":255}",
                     static_cast<unsigned>(p.channelCount));
        for (uint8_t c = 0; c < p.channelCount; c++) {
            // Reference the shared option set rather than inlining its strings.
            sink.appendf(",{\"name\":\"ch%u\",\"type\":\"select\",\"value\":%u,\"optionsRef\":\"channelRole\"}",
                         static_cast<unsigned>(c), static_cast<unsigned>(roles[c]));
        }
        sink.append("]}");
    }

    /// The list is editable, so the UI offers add, delete and reorder.
    bool isEditableList() const override { return true; }

    /// Add a preset, returning its new stable id.
    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxPresets) return false;
        Preset& p = presets_[count_];
        p = Preset{};
        p.id = nextId_++;
        p.channelCount = 3;
        // Modulo 10^5 so the name provably fits: it is a placeholder the user renames anyway.
        std::snprintf(p.name, sizeof(p.name), "preset %u",
                      static_cast<unsigned>(p.id % 100000u));
        count_++;
        rebuildPool();                       // give the new preset its slice (defaults R,G,B)
        uint8_t* r = roleAtMut(p);
        r[0] = static_cast<uint8_t>(ChannelRole::Red);
        r[1] = static_cast<uint8_t>(ChannelRole::Green);
        r[2] = static_cast<uint8_t>(ChannelRole::Blue);
        outId = p.id;
        refreshStatus();
        return true;
    }

    /// Delete a preset, refusing a locked built-in.
    bool deleteListRow(uint32_t id) override {
        int i = indexOf(id);
        if (i < 0 || presets_[i].locked) return false;   // a seeded built-in is protected
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < count_; j++) presets_[j] = presets_[j + 1];
        count_--;
        rebuildPool();
        refreshStatus();
        return true;
    }

    /// Move a custom preset, which may not cross into the locked built-in block.
    bool moveListRow(uint32_t id, uint8_t to) override {
        int i = indexOf(id);
        if (i < 0) return false;
        // The built-ins are a FIXED block at the top, so customs reorder only among themselves.
        if (presets_[i].locked) return false;
        const uint8_t firstCustom = lockedCount();
        if (to < firstCustom) to = firstCustom;      // clamp a custom above the built-ins back down
        if (to >= count_) to = static_cast<uint8_t>(count_ - 1);
        Preset moved = presets_[i];
        if (to > i) for (int j = i; j < to; j++) presets_[j] = presets_[j + 1];
        else        for (int j = i; j > to; j--) presets_[j] = presets_[j - 1];
        presets_[to] = moved;
        rebuildPool();                       // pool order follows preset order
        return true;
    }

    /// Edit one field of a preset: its name, its channel count, or one channel's role.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        int i = indexOf(id);
        if (i < 0 || presets_[i].locked) return false;   // built-ins are read-only
        Preset& p = presets_[i];
        if (std::strcmp(field, "name") == 0) {
            mm::json::parseString(valueJson, "value", p.name, sizeof(p.name));
            return true;
        }
        if (std::strcmp(field, "channels") == 0) {
            int v = mm::json::parseInt(valueJson, "value");
            if (v < 1 || v > 255) return false;
            setChannelCount(p, static_cast<uint8_t>(v));
            return true;
        }
        if (field[0] == 'c' && field[1] == 'h' && field[2] >= '0' && field[2] <= '9') {
            // strtol, not atoi: a malformed suffix must be rejected, not coerced to channel 0.
            char* end = nullptr;
            errno = 0;
            const long c = std::strtol(field + 2, &end, 10);
            if (errno != 0 || *end != '\0' || c < 0 || c >= p.channelCount) return false;
            int v = mm::json::parseInt(valueJson, "value");
            if (v < 0 || v >= kChannelRoleCount) return false;
            roleAtMut(p)[static_cast<uint8_t>(c)] = static_cast<uint8_t>(v);
            return true;
        }
        return false;
    }

    /// Restore the presets and the role pool, so custom wirings survive a reboot.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        count_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && count_ < kMaxPresets; r++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            Preset& p = presets_[count_];
            p = Preset{};
            p.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
            mm::json::readString(mm::json::member(doc, row, "name"), p.name, sizeof(p.name));
            int ch = mm::json::readInt(mm::json::member(doc, row, "channels"), 3);
            p.channelCount = static_cast<uint8_t>(ch < 1 ? 1 : ch > 255 ? 255 : ch);
            p.locked = mm::json::readBool(mm::json::member(doc, row, "locked"), false);
            if (p.id >= nextId_) nextId_ = p.id + 1;   // never reissue a persisted id
            count_++;
        }
        rebuildPool();
        // Degrade to an empty list rather than writing roles through a null pool base.
        if (!rolePool_.data() && count_ > 0) { count_ = 0; return true; }
        // Second pass: fill each preset's roles now that the pool is sized.
        for (int r = 0, idx = 0; r < n && idx < count_; r++, idx++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            const mm::json::JsonNode* roles = mm::json::member(doc, row, "roles");
            uint8_t* dst = roleAtMut(presets_[idx]);
            const int rn = (roles && roles->type == mm::json::JsonType::Array)
                               ? mm::json::arraySize(doc, roles) : 0;
            // Clamped: a corrupt file can carry a byte the UI mis-renders and the Correction drops.
            for (uint8_t c = 0; c < presets_[idx].channelCount; c++) {
                const long rv = (c < rn) ? mm::json::readInt(mm::json::element(doc, roles, c), 0) : 0;
                dst[c] = (rv < 0 || rv >= kChannelRoleCount) ? 0 : static_cast<uint8_t>(rv);
            }
        }
        return true;
    }

private:
    // A fixed small struct: the roles live in the shared pool, packed in preset order.
    struct Preset {
        uint32_t id = 0;
        char     name[16] = {};
        uint8_t  channelCount = 3;
        uint32_t poolOffset = 0;   // start of this preset's roles in rolePool_ (set by rebuildPool)
        uint32_t poolLen = 0;      // the OLD slice size, distinct from channelCount mid-change
        bool     locked = false;
    };

    Preset   presets_[kMaxPresets] = {};
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    ScratchBuffer<uint8_t> rolePool_{*this};   // Σ channelCount role bytes, packed in preset order
    char     statusBuf_[24] = {};
    ActiveInstance<LightPresetsModule> seat_{*this};

    const Preset* find(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return &presets_[i];
        return nullptr;
    }
    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return i;
        return -1;
    }
    // The built-ins never move, so the locked count is also the first custom row's index.
    uint8_t lockedCount() const {
        uint8_t n = 0;
        while (n < count_ && presets_[n].locked) n++;
        return n;
    }
    const uint8_t* roleAt(const Preset& p) const { return rolePool_.data() + p.poolOffset; }
    uint8_t*       roleAtMut(const Preset& p)     { return rolePool_.data() + p.poolOffset; }

    // Preserves the role bytes across the re-pack, since resize reallocates and zero-fills.
    void rebuildPool() {
        const uint32_t oldPoolLen = static_cast<uint32_t>(rolePool_.count());
        uint32_t newOff[kMaxPresets] = {};
        uint32_t total = 0;
        for (uint8_t i = 0; i < count_; i++) { newOff[i] = total; total += presets_[i].channelCount; }

        // Tracked explicitly rather than inferred: channelCount may already hold the new value.
        uint8_t* keep = total ? static_cast<uint8_t*>(platform::alloc(total)) : nullptr;
        if (keep) {
            std::memset(keep, 0, total);
            if (rolePool_.data()) {
                for (uint8_t i = 0; i < count_; i++) {
                    const uint32_t oldStart = presets_[i].poolOffset;
                    const uint32_t oldLen   = presets_[i].poolLen;
                    const uint32_t newLen   = presets_[i].channelCount;
                    const uint32_t copy     = oldLen < newLen ? oldLen : newLen;   // surviving overlap
                    if (copy && oldStart + copy <= oldPoolLen)
                        std::memcpy(keep + newOff[i], rolePool_.data() + oldStart, copy);
                }
            }
        }
        rolePool_.resize(total);
        for (uint8_t i = 0; i < count_; i++) {
            presets_[i].poolOffset = newOff[i];
            presets_[i].poolLen    = presets_[i].channelCount;   // slice now matches the channel count
        }
        if (keep) {
            if (rolePool_.data()) std::memcpy(rolePool_.data(), keep, total);
            platform::free(keep);
        }
    }

    // Preserves the existing role picks; new channels default to R, G, B, W then None.
    void setChannelCount(Preset& p, uint8_t n) {
        const uint8_t was = p.channelCount;
        p.channelCount = n;
        rebuildPool();                       // pool now holds n bytes for this preset (old kept, tail zeroed)
        uint8_t* r = roleAtMut(p);
        for (uint8_t c = was; c < n; c++)
            r[c] = c < 4 ? static_cast<uint8_t>(c + 1) : 0;   // 1=R,2=G,3=B,4=W, then None
    }

    // Data, not code, so a preset of any width seeds directly: only real orders are listed.
    void seedBuiltins() {
        using R = ChannelRole;
        static constexpr R kRGB[]    = {R::Red, R::Green, R::Blue};
        static constexpr R kGRB[]    = {R::Green, R::Red, R::Blue};
        static constexpr R kBGR[]    = {R::Blue, R::Green, R::Red};
        static constexpr R kRGBW[]   = {R::Red, R::Green, R::Blue, R::White};
        static constexpr R kGRBW[]   = {R::Green, R::Red, R::Blue, R::White};
        static constexpr R kWRGB[]   = {R::White, R::Red, R::Green, R::Blue};                     // ws2814
        static constexpr R kGRB6[]   = {R::Green, R::Red, R::Blue, R::None, R::None, R::None};     // curtain
        static constexpr R kRGBWYP[] = {R::Red, R::Green, R::Blue, R::White, R::Yellow, R::UV};    // lightbar
        static constexpr R kRGBCCT[] = {R::Red, R::Green, R::Blue, R::White, R::WarmWhite};        // cold+warm
        static constexpr R kIRGB[]   = {R::Dimmer, R::Red, R::Green, R::Blue};                     // CH1 master intensity
        // Moving heads (MoonLight offset maps → dense arrays). N = None.
        static constexpr R N = R::None;
        // Two Dimmer roles: the derivation keeps the last, so CH4 is the first thing to check.
        static constexpr R kMHBeeEyes15[] = {   // 15ch: Pan,Tilt,-,Dim,-,Gobo,-,Zoom,Dim,-,R,G,B,-,-
            R::Pan, R::Tilt, N, R::Dimmer, N, R::Gobo, N, R::Zoom, R::Dimmer, N, R::Red, R::Green, R::Blue, N, N};
        static constexpr R kMHBeTopper32[] = {  // 32ch: Pan,-,Tilt,-,-,Zoom,Dim,-,-,R,G,B,… (RGBW cells → None)
            R::Pan, N, R::Tilt, N, N, R::Zoom, R::Dimmer, N, N, R::Red, R::Green, R::Blue,
            N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N};
        static constexpr R kMH19x15W24[] = {    // 24ch: Pan,Tilt,-,Dim,R,G,B,W,…,Zoom@17 (RGBW cells → None)
            R::Pan, R::Tilt, N, R::Dimmer, R::Red, R::Green, R::Blue, R::White,
            N, N, N, N, N, N, N, N, N, R::Zoom, N, N, N, N, N, N};
        // Leaving strobe and axis speed unmapped holds them at 0, which a light driver wants.
        static constexpr R kMHMini11[] = {      // 11ch: Pan,-,Tilt,-,-,Dim,-,R,G,B,W
            R::Pan, N, R::Tilt, N, N, R::Dimmer, N, R::Red, R::Green, R::Blue, R::White};

        struct Builtin { const char* name; const R* roles; uint8_t channelCount; };
        static constexpr Builtin kBuiltins[] = {
            {"RGB", kRGB, 3}, {"GRB", kGRB, 3}, {"BGR", kBGR, 3},
            {"RGBW", kRGBW, 4}, {"GRBW", kGRBW, 4}, {"WRGB", kWRGB, 4},
            {"Curtain GRB6", kGRB6, 6}, {"Lightbar RGBWYP", kRGBWYP, 6},
            {"RGBCCT", kRGBCCT, 5}, {"IRGB", kIRGB, 4},
            {"MH BeeEyes 15", kMHBeeEyes15, 15},
            {"MH BeTopper 32", kMHBeTopper32, 32},
            {"MH 19x15W-24", kMH19x15W24, 24},
            {"MH Mini 10W 11", kMHMini11, 11},
        };
        // Fails LOUD at build time: the fix is a shorter name, not a wider standing buffer.
        constexpr auto namesFit = [](const Builtin* t, size_t n) {
            for (size_t i = 0; i < n; i++) {
                size_t len = 0; while (t[i].name[len]) len++;
                if (len >= sizeof(Preset::name)) return false;
            }
            return true;
        };
        static_assert(namesFit(kBuiltins, sizeof(kBuiltins) / sizeof(kBuiltins[0])),
                      "a built-in preset name exceeds Preset::name — shorten it");
        for (const Builtin& b : kBuiltins) {
            if (count_ >= kMaxPresets) break;
            Preset& p = presets_[count_];
            p = Preset{};
            p.id = nextId_++;
            p.locked = true;
            std::snprintf(p.name, sizeof(p.name), "%s", b.name);
            p.channelCount = b.channelCount;
            count_++;
            rebuildPool();
            uint8_t* dst = roleAtMut(p);
            for (uint8_t c = 0; c < b.channelCount; c++) dst[c] = static_cast<uint8_t>(b.roles[c]);
        }
    }

    void refreshStatus() {
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u preset%s", count_, count_ == 1 ? "" : "s");
        setStatus(statusBuf_);
    }
};

} // namespace mm
