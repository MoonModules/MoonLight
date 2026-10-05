#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"     // the singleton seat drivers resolve the library through
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"           // restoreList: recursive reader for the persisted array
#include "light/drivers/ChannelRole.h"
#include "light/drivers/Correction.h"  // the role-array rebuild and the derived offsets

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

namespace mm {

/// The reusable fixture-profile library, a Drivers submodule owning NAMED channel-role wirings, each editable in its own row and referenced by many drivers. A driver stores a profile's STABLE id and resolves it here into its own Correction, so a wiring is built once and reordering other profiles disturbs no reference.
///
/// A curated set of real fixtures is seeded read-only on first boot. A user adds custom named wirings alongside them. The render loop never reads this module.
///
/// @moreinfo
///
/// ## What a profile is
///
/// A channel-role layout: role `r` at channel `i` says channel `i` of a light carries role `r`.
/// The color roles cover the strip orders, and the fixture roles cover pan, tilt and the rest.
///
/// ## Storage is uncapped
///
/// A profile is exactly as wide as its fixture. Role bytes live in one dynamic pool, each profile a slice, so a moving head declares as many channels as it has. The pool is touched on the cold path only, leaving it free to reallocate.
///
/// ## The editable-list primitive
///
/// The first consumer of `EditableListSource`. The whole add, delete, reorder and edit surface is reused rather than rebuilt. The per-row fields are the name, the channel count, and one role picker per channel.
///
/// @card fixtureprofiles.png
class FixtureProfilesModule : public MoonModule, public ListSource {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Generic; }

    // A deleted library could never be re-added, and every driver resolves its profile through it.
    /// Not user-editable: every driver resolves its profile through this one boot-wired library.
    bool userEditable() const override { return false; }

    /// How many profile rows a device can hold.
    static constexpr uint8_t kMaxProfiles = 48;   // the 20 built-ins plus room for a rig's own wirings

    /// The boot library, which a driver resolves its profile reference through.
    static FixtureProfilesModule* active() { return ActiveInstance<FixtureProfilesModule>::active(); }

    /// The first profile's id: a safe default for a fresh driver or a dangling reference.
    uint32_t defaultId() const { return count_ ? profiles_[0].id : 0; }

    /// How many profiles the library holds, for a driver building its profile selector.
    uint8_t profileCount() const { return count_; }
    /// The name of profile row `i`, for a driver building its selector.
    const char* nameAt(uint8_t i) const { return i < count_ ? profiles_[i].name : ""; }
    /// The stable id of profile row `i`, which is what a driver stores.
    uint32_t idAt(uint8_t i) const { return i < count_ ? profiles_[i].id : 0; }
    /// The row currently holding `id`, for rendering a selector at its referenced profile.
    uint8_t indexOfId(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (profiles_[i].id == id) return i;
        return 0;   // a dangling id renders at the first profile; rebuildCorrection falls back to it too
    }

    // Any channel apply() synthesizes from RGB counts, since one control governs them all.
    /// Whether the profile carries a channel the white mode would synthesize.
    bool profileHasSynthChannel(uint32_t id) const {
        const Profile* p = find(id);
        if (!p) return false;
        const uint8_t* r = roleAt(*p);
        for (uint8_t c = 0; c < p->channelCount; c++) {
            switch (static_cast<ChannelRole>(r[c])) {
                case ChannelRole::White:
                case ChannelRole::WarmWhite:
                case ChannelRole::WhiteFine:
                case ChannelRole::WarmWhiteFine:
                case ChannelRole::Yellow:
                case ChannelRole::UV:
                    return true;
                default: break;
            }
        }
        return false;
    }

    /// Resolve a profile id into a driver's flat Correction; false leaves `out` untouched.
    bool deriveCorrection(uint32_t id, uint8_t brightness, Correction& out) const {
        const Profile* p = find(id);
        if (!p) return false;
        // The option indices are aligned with the enum, so a role byte IS a ChannelRole value.
        out.rebuild(brightness, reinterpret_cast<const ChannelRole*>(roleAt(*p)), p->channelCount);
        return true;
    }

    // Claimed at CONSTRUCTION: a driver resolves the library while building its own controls.
    /// Claim the singleton seat at construction, before any driver builds its selector.
    FixtureProfilesModule() { seat_.claim(); }
    /// Free the role pool, which lives as long as the library rather than between prepare and release.
    ~FixtureProfilesModule() override { platform::free(pool_); }

    /// Seed the curated built-ins the list does not hold yet.
    void setup() override {
        MoonModule::setup();
        seedBuiltins();
        refreshStatus();
    }

    /// Re-claim the singleton seat, which is a no-op while we already hold it.
    void prepare() override { seat_.claim(); }   // idempotent: no-op while we already hold the seat
    /// Vacate the singleton seat, then release the base.
    void release() override { seat_.vacate(); MoonModule::release(); }

    /// Bind the profiles list, which the editable-list primitive renders.
    void defineControls() override {
        MoonModule::defineControls();
        // The persisted rows INCLUDE the built-ins, so restoring replaces rather than duplicates.
        seedBuiltins();
        controls_.addList("profiles", *this);   // this module is the (editable) ListSource
    }

    // --- ListSource (editable) ---------------------------------------------------------
    /// How many profile rows the list holds.
    uint8_t listRowCount() const override { return count_; }

    // This row IS the persisted form, so it must carry the profile's full wiring.
    /// Write one row, which is also the persisted form, so it carries the full wiring.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        const Profile& p = profiles_[row];
        const uint8_t* roles = roleAt(p);
        // Escaped, not raw: a quote in a name would produce JSON that wipes every custom profile.
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
        const Profile& p = profiles_[row];
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

    /// Add a profile, returning its new stable id.
    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxProfiles) return false;
        Profile& p = profiles_[count_];
        p = Profile{};
        p.id = nextId_++;
        p.channelCount = 3;
        // Modulo 10^5 so the name provably fits: it is a placeholder the user renames anyway.
        std::snprintf(p.name, sizeof(p.name), "profile %u",
                      static_cast<unsigned>(p.id % 100000u));
        count_++;
        rebuildPool();                       // give the new profile its slice (defaults R,G,B)
        uint8_t* r = roleAtMut(p);
        r[0] = static_cast<uint8_t>(ChannelRole::Red);
        r[1] = static_cast<uint8_t>(ChannelRole::Green);
        r[2] = static_cast<uint8_t>(ChannelRole::Blue);
        outId = p.id;
        refreshStatus();
        return true;
    }

    /// Delete a profile, refusing a locked built-in.
    bool deleteListRow(uint32_t id) override {
        int i = indexOf(id);
        if (i < 0 || profiles_[i].locked) return false;   // a seeded built-in is protected
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < count_; j++) profiles_[j] = profiles_[j + 1];
        count_--;
        rebuildPool();
        refreshStatus();
        return true;
    }

    /// Move a custom profile, which may not cross into the locked built-in block.
    bool moveListRow(uint32_t id, uint8_t to) override {
        int i = indexOf(id);
        if (i < 0) return false;
        // The built-ins are a FIXED block at the top, so customs reorder only among themselves.
        if (profiles_[i].locked) return false;
        const uint8_t firstCustom = lockedCount();
        if (to < firstCustom) to = firstCustom;      // clamp a custom above the built-ins back down
        if (to >= count_) to = static_cast<uint8_t>(count_ - 1);
        Profile moved = profiles_[i];
        if (to > i) for (int j = i; j < to; j++) profiles_[j] = profiles_[j + 1];
        else        for (int j = i; j > to; j--) profiles_[j] = profiles_[j - 1];
        profiles_[to] = moved;
        rebuildPool();                       // pool order follows profile order
        return true;
    }

    /// Edit one field of a profile: its name, its channel count, or one channel's role.
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        int i = indexOf(id);
        if (i < 0 || profiles_[i].locked) return false;   // built-ins are read-only
        Profile& p = profiles_[i];
        if (std::strcmp(field, "name") == 0) {
            // A driver saves its profile by name, so two profiles of one name would let a reboot pick the wrong one.
            char name[sizeof(p.name)] = {};
            mm::json::parseString(valueJson, "value", name, sizeof(name));
            if (!name[0] || nameTaken(name, p.id)) return false;
            std::memcpy(p.name, name, sizeof(p.name));
            return true;
        }
        if (std::strcmp(field, "channels") == 0) {
            int v = mm::json::parseInt(valueJson, "value");
            if (v < 1 || v > 255) return false;
            setChannelCount(p, static_cast<uint8_t>(v));
            return true;
        }
        return setChannelRole(p, field, valueJson);
    }

    /// Restore the profiles and the role pool, so custom wirings survive a reboot.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        count_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && count_ < kMaxProfiles; r++) restoreRow(doc, mm::json::element(doc, arr, r), profiles_[count_++]);
        rebuildPool();
        // Degrade to an empty list rather than writing roles through a null pool base.
        if (!pool_ && count_ > 0) { count_ = 0; return true; }
        // Second pass: fill each profile's roles now that the pool is sized.
        for (int r = 0; r < n && r < count_; r++) restoreRoles(doc, mm::json::element(doc, arr, r), profiles_[r]);
        seedBuiltins();   // a built-in this firmware added since the list was saved
        return true;
    }

private:
    // A fixed small struct: the roles live in the shared pool, packed in profile order.
    struct Profile {
        uint32_t id = 0;
        char     name[16] = {};
        uint8_t  channelCount = 3;
        uint16_t poolOffset = 0;   // start of this profile's roles in pool_ (set by rebuildPool); 48 x 255 fits
        uint16_t poolLen = 0;      // the OLD slice size, distinct from channelCount mid-change
        bool     locked = false;
    };

    static_assert(kMaxProfiles * 255 <= 0xFFFF, "a full library's role pool must fit the 16-bit pool offsets");
    Profile   profiles_[kMaxProfiles] = {};
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    // Configuration, like the rows above, so it survives release(); a ScratchBuffer would be freed there with every profile's wiring.
    uint8_t* pool_ = nullptr;   // Σ channelCount role bytes, packed in profile order
    uint32_t poolBytes_ = 0;    // the pool's size, the bound a re-pack copies within
    char     statusBuf_[24] = {};
    ActiveInstance<FixtureProfilesModule> seat_{*this};

    const Profile* find(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (profiles_[i].id == id) return &profiles_[i];
        return nullptr;
    }
    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (profiles_[i].id == id) return i;
        return -1;
    }
    // The built-ins never move, so the locked count is also the first custom row's index.
    uint8_t lockedCount() const {
        uint8_t n = 0;
        while (n < count_ && profiles_[n].locked) n++;
        return n;
    }
    const uint8_t* roleAt(const Profile& p) const { return pool_ + p.poolOffset; }
    uint8_t*       roleAtMut(const Profile& p)     { return pool_ + p.poolOffset; }

    /// Whether another profile than `self` already has this name.
    bool nameTaken(const char* name, uint32_t self) const {
        for (uint8_t i = 0; i < count_; i++)
            if (profiles_[i].id != self && std::strcmp(profiles_[i].name, name) == 0) return true;
        return false;
    }

    /// Whether `field` names one channel's role picker, `ch` and a digit.
    static bool isChannelField(const char* field) {
        return field[0] == 'c' && field[1] == 'h' && field[2] >= '0' && field[2] <= '9';
    }

    /// Set the role of the channel a `chN` field names; false for any other field or an out-of-range value.
    bool setChannelRole(Profile& p, const char* field, const char* valueJson) {
        if (!isChannelField(field)) return false;
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

    /// Read one persisted row's id, name, channel count and lock, its roles following once the pool is sized.
    void restoreRow(const mm::json::JsonDoc& doc, const mm::json::JsonNode* row, Profile& p) {
        p = Profile{};
        p.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
        mm::json::readString(mm::json::member(doc, row, "name"), p.name, sizeof(p.name));
        const int ch = mm::json::readInt(mm::json::member(doc, row, "channels"), 3);
        p.channelCount = static_cast<uint8_t>(ch < 1 ? 1 : ch > 255 ? 255 : ch);
        p.locked = mm::json::readBool(mm::json::member(doc, row, "locked"), false);
        if (p.id >= nextId_) nextId_ = p.id + 1;   // never reissue a persisted id
    }

    // Clamped: a corrupt file can carry a byte the UI mis-renders and the Correction drops.
    /// Read one persisted row's roles into its slice of the pool, an absent or invalid one as None.
    void restoreRoles(const mm::json::JsonDoc& doc, const mm::json::JsonNode* row, const Profile& p) {
        const mm::json::JsonNode* roles = mm::json::member(doc, row, "roles");
        uint8_t* dst = roleAtMut(p);
        const int rn = (roles && roles->type == mm::json::JsonType::Array) ? mm::json::arraySize(doc, roles) : 0;
        for (uint8_t c = 0; c < p.channelCount; c++) {
            const long rv = (c < rn) ? mm::json::readInt(mm::json::element(doc, roles, c), 0) : 0;
            dst[c] = (rv < 0 || rv >= kChannelRoleCount) ? 0 : static_cast<uint8_t>(rv);
        }
    }

    // A new pool in profile order, each profile's surviving roles copied across, then swapped in.
    void rebuildPool() {
        uint32_t newOff[kMaxProfiles] = {};
        uint32_t total = 0;
        for (uint8_t i = 0; i < count_; i++) { newOff[i] = total; total += profiles_[i].channelCount; }
        uint8_t* fresh = total ? static_cast<uint8_t*>(platform::alloc(total)) : nullptr;
        if (fresh) {
            std::memset(fresh, 0, total);
            copySurvivingRoles(fresh, newOff);
        }
        platform::free(pool_);
        pool_ = fresh;
        poolBytes_ = fresh ? total : 0;
        for (uint8_t i = 0; i < count_; i++) {
            profiles_[i].poolOffset = static_cast<uint16_t>(newOff[i]);
            profiles_[i].poolLen    = profiles_[i].channelCount;   // slice now matches the channel count
        }
    }

    // Tracked explicitly rather than inferred: channelCount may already hold the new value.
    /// Copy each profile's surviving roles from the current pool into `fresh` at its new offset.
    void copySurvivingRoles(uint8_t* fresh, const uint32_t* newOff) const {
        if (!pool_) return;
        for (uint8_t i = 0; i < count_; i++) {
            const uint32_t oldStart = profiles_[i].poolOffset;
            const uint32_t oldLen   = profiles_[i].poolLen;
            const uint32_t newLen   = profiles_[i].channelCount;
            const uint32_t copy     = oldLen < newLen ? oldLen : newLen;   // surviving overlap
            if (copy && oldStart + copy <= poolBytes_) std::memcpy(fresh + newOff[i], pool_ + oldStart, copy);
        }
    }

    // Preserves the existing role picks; new channels default to R, G, B, W then None.
    void setChannelCount(Profile& p, uint8_t n) {
        const uint8_t was = p.channelCount;
        p.channelCount = n;
        rebuildPool();                       // pool now holds n bytes for this profile (old kept, tail zeroed)
        uint8_t* r = roleAtMut(p);
        for (uint8_t c = was; c < n; c++)
            r[c] = c < 4 ? static_cast<uint8_t>(c + 1) : 0;   // 1=R,2=G,3=B,4=W, then None
    }

    // Data, not code, so a profile of any width seeds directly: only real orders are listed.
    /// Add each built-in the list lacks, by name, and order the built-in block as this table does, so a new built-in reaches a saved list too.
    void seedBuiltins() {
        uint8_t shipped = 0;
        const Builtin* table = builtins(shipped);
        for (uint8_t k = 0; k < shipped && count_ < kMaxProfiles; k++)
            if (!holdsBuiltin(table[k].name)) insertBuiltin(table[k]);
        sortBuiltins(table, shipped);
    }

    /// One shipped profile: its name and its role per channel.
    struct Builtin { const char* name; const ChannelRole* roles; uint8_t channelCount; };

    /// The shipped built-ins, in the order the list shows them; `count` receives how many.
    static const Builtin* builtins(uint8_t& count) {
        using R = ChannelRole;
        // All six orders of three colors, as FastLED and WLED offer them; GRB is the WS2812B, WS2813 and WS2815.
        static constexpr R kRGB[]    = {R::Red, R::Green, R::Blue};
        static constexpr R kGRB[]    = {R::Green, R::Red, R::Blue};
        static constexpr R kBGR[]    = {R::Blue, R::Green, R::Red};
        static constexpr R kRBG[]    = {R::Red, R::Blue, R::Green};
        static constexpr R kGBR[]    = {R::Green, R::Blue, R::Red};
        static constexpr R kBRG[]    = {R::Blue, R::Red, R::Green};
        static constexpr R kRGBW[]   = {R::Red, R::Green, R::Blue, R::White};
        static constexpr R kGRBW[]   = {R::Green, R::Red, R::Blue, R::White};
        static constexpr R kWRGB[]   = {R::White, R::Red, R::Green, R::Blue};                     // ws2814
        static constexpr R kGRB6[]   = {R::Green, R::Red, R::Blue, R::None, R::None, R::None};     // curtain
        static constexpr R kRGBWYP[] = {R::Red, R::Green, R::Blue, R::White, R::Yellow, R::UV};    // lightbar
        static constexpr R kRGBCCT[] = {R::Red, R::Green, R::Blue, R::White, R::WarmWhite};        // cold+warm
        static constexpr R kIRGB[]   = {R::Dimmer, R::Red, R::Green, R::Blue};                     // CH1 master intensity
        // 16-bit chips, each color as its high byte, then its low byte: UCS8903, WS2816 and UCS8904.
        static constexpr R kRGB16[]  = {R::Red, R::RedFine, R::Green, R::GreenFine, R::Blue, R::BlueFine};
        static constexpr R kGRB16[]  = {R::Green, R::GreenFine, R::Red, R::RedFine, R::Blue, R::BlueFine};
        static constexpr R kRGBW16[] = {R::Red, R::RedFine, R::Green, R::GreenFine, R::Blue, R::BlueFine, R::White, R::WhiteFine};
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

        static constexpr Builtin kBuiltins[] = {
            {"RGB", kRGB, 3}, {"GRB", kGRB, 3}, {"BGR", kBGR, 3},
            {"RBG", kRBG, 3}, {"GBR", kGBR, 3}, {"BRG", kBRG, 3},
            {"RGBW", kRGBW, 4}, {"GRBW", kGRBW, 4}, {"WRGB", kWRGB, 4},
            {"RGB 16-bit", kRGB16, 6}, {"GRB 16-bit", kGRB16, 6}, {"RGBW 16-bit", kRGBW16, 8},
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
                if (len >= sizeof(Profile::name)) return false;
            }
            return true;
        };
        static_assert(namesFit(kBuiltins, sizeof(kBuiltins) / sizeof(kBuiltins[0])),
                      "a built-in profile name exceeds Profile::name: shorten it");
        count = static_cast<uint8_t>(sizeof(kBuiltins) / sizeof(kBuiltins[0]));
        return kBuiltins;
    }

    /// Whether the list already holds the built-in of this name.
    bool holdsBuiltin(const char* name) const {
        for (uint8_t i = 0; i < count_; i++)
            if (profiles_[i].locked && std::strcmp(profiles_[i].name, name) == 0) return true;
        return false;
    }

    // At the end of the built-in block, which sortBuiltins then puts in order.
    /// Add a built-in the list lacks, above the custom rows.
    void insertBuiltin(const Builtin& b) {
        const uint8_t at = lockedCount();
        for (uint8_t j = count_; j > at; j--) profiles_[j] = profiles_[j - 1];
        Profile& p = profiles_[at];
        p = Profile{};
        p.id = nextId_++;
        p.locked = true;
        std::snprintf(p.name, sizeof(p.name), "%s", b.name);
        p.channelCount = b.channelCount;
        count_++;
        rebuildPool();
        uint8_t* dst = roleAtMut(p);
        for (uint8_t c = 0; c < b.channelCount; c++) dst[c] = static_cast<uint8_t>(b.roles[c]);
    }

    // A user cannot move a built-in, so the block always reads in the shipped order: an insertion sort, stable and in place.
    /// Order the built-in block as `table` does; a built-in an older firmware shipped sorts after the rest.
    void sortBuiltins(const Builtin* table, uint8_t shipped) {
        const auto rank = [&](const Profile& p) {
            for (uint8_t k = 0; k < shipped; k++) if (std::strcmp(p.name, table[k].name) == 0) return k;
            return shipped;
        };
        const uint8_t n = lockedCount();
        for (uint8_t i = 1; i < n; i++) {
            const Profile moved = profiles_[i];
            uint8_t j = i;
            for (; j > 0 && rank(profiles_[j - 1]) > rank(moved); j--) profiles_[j] = profiles_[j - 1];
            profiles_[j] = moved;
        }
        rebuildPool();   // the pool follows the new profile order
    }

    void refreshStatus() {
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u profile%s", count_, count_ == 1 ? "" : "s");
        setStatus(statusBuf_);
    }
};

} // namespace mm
