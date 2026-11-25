#pragma once

#include "core/module/MoonModule.h"
#include "platform/platform_heap.h"   // alloc, free

#include <cstring>

namespace mm {

using CreateModuleFn = MoonModule*(*)();

/// The registry mapping a type name to the function that builds one, which is what lets a preset name a module this build has never instantiated.
///
/// Storage grows on demand rather than reserving a fixed cap, which once held a kilobyte of internal memory empty.
///
/// @technical
///
/// ## What a registration captures
///
/// A single probe instance yields the type's size, its role, its tags and its dimensionality, so registering is one line at boot; it lives on the heap for the moment it is read, since the boot task's stack is not sized for a module.
///
/// The documentation path rides along as a flash literal with no per-instance cost, and the interface builds a help link from it.
///
/// Dimensionality is captured only when the type declares it, as the effect and modifier bases do: the probe detects the method and reduces its result to a byte, so a light-domain concept stays out of core.
///
/// ## Registering the same name twice is a no-op
///
/// Production registers each type once at boot, but a test fixture registers per construction, which once accumulated a duplicate set per case until the capacity saturated and every later registration failed.
///
/// ## The display name drops the role noun
///
/// A card's label would otherwise repeat what the role chip says, so the suffix naming the role is stripped: an effect, a modifier, a layout, a driver and a service each lose their own.
///
/// A name without the suffix is returned unchanged, and the type name itself stays intact for persistence and lookup.
class ModuleFactory {
public:
    /// Register a type, capturing everything from one probe instance: @xref{what-a-registration-captures|what is taken, and how}.
    template<typename T>
    static bool registerType(const char* typeName, const char* docPath = "") {
        if (!typeName) return false;
        // Idempotent: @xref{registering-the-same-name-twice-is-a-no-op|what a duplicate once cost a test run}.
        for (uint8_t i = 0; i < count_; i++)
            if (std::strcmp(types_[i].name, typeName) == 0) return true;
        if (!grow()) return false;
        // On the heap, as an instance can outgrow the boot stack; raw new, since make_unique inlines each type's constructor here, +13.4 KB on the S3.
        T* probe = new T();
        if (!probe) return false;
        uint8_t dim = 0;
        if constexpr (requires(const T& t) { static_cast<uint8_t>(t.dimensions()); }) {
            dim = static_cast<uint8_t>(probe->dimensions());
        }
        types_[count_++] = {typeName,
                            []() -> MoonModule* { return new T(); },
                            sizeof(T),
                            docPath ? docPath : "",
                            probe->tags() ? probe->tags() : "",
                            probe->acceptsChildRoles() ? probe->acceptsChildRoles() : "",
                            probe->role(),
                            dim,
                            0};
        delete probe;
        return true;
    }

    /// Register an entry built by hand, for a factory that is not a simple construction, with no dimensionality to capture.
    static bool registerType(const char* typeName, CreateModuleFn fn, size_t classSize = 0,
                             ModuleRole role = ModuleRole::Generic, const char* docPath = "",
                             const char* tags = "") {
        if (!typeName || !fn) return false;
        for (uint8_t i = 0; i < count_; i++)
            if (std::strcmp(types_[i].name, typeName) == 0) return true;
        if (!grow()) return false;
        // A hand-built entry cannot probe an instance, so it accepts no children; no such type is a container today.
        types_[count_++] = {typeName, fn, classSize, docPath ? docPath : "", tags ? tags : "", "", role, 0, 0};
        return true;
    }

    /// Build a module by type name, or nothing when no such type is registered.
    static MoonModule* create(const char* typeName) {
        if (!typeName || !types_) return nullptr;
        for (uint8_t i = 0; i < count_; i++) {
            if (std::strcmp(types_[i].name, typeName) == 0) {
                auto* mod = types_[i].create();
                if (mod) {
                    // The registry's own literal rather than the caller's parameter, which may be a stack buffer: the name is stored as a pointer, not copied.
                    mod->setTypeName(types_[i].name);
                    // The label with its role noun stripped: @xref{the-display-name-drops-the-role-noun|why the chip already says it}.
                    mod->setName(displayNameFor(types_[i].name, types_[i].role));
                    if (types_[i].classSize > 0) mod->setClassSize(types_[i].classSize);
                }
                return mod;
            }
        }
        return nullptr;
    }

    /// Strip the role noun from a type name, into a shared buffer the caller copies: @xref{the-display-name-drops-the-role-noun|which suffix each role loses}.
    static const char* displayNameFor(const char* typeName, ModuleRole role) {
        const char* suffix = "";
        switch (role) {
            case ModuleRole::Effect:   suffix = "Effect";   break;
            case ModuleRole::Modifier: suffix = "Modifier"; break;
            case ModuleRole::Layout:   suffix = "Layout";   break;
            case ModuleRole::Driver:   suffix = "Driver";   break;
            case ModuleRole::Generic:  suffix = "Module";   break;
            case ModuleRole::Layer:    return typeName;     // no suffix to strip
            // A service names itself by its subcategory, as the light roles do.
            case ModuleRole::Service:  suffix = "Service";  break;
            case ModuleRole::DeviceGroup: return typeName;   // "DeviceGroup" whole: "Group" alone would name the multicast groups the map draws
        }
        size_t typeLen = std::strlen(typeName);
        size_t suffixLen = std::strlen(suffix);
        if (suffixLen == 0 || typeLen <= suffixLen ||
            std::strcmp(typeName + typeLen - suffixLen, suffix) != 0) {
            return typeName;  // Suffix not present — leave the name untouched.
        }
        // Into a shared buffer reused per call, which the caller copies out immediately.
        static char displayBuf[16];
        size_t copyLen = typeLen - suffixLen;
        if (copyLen >= sizeof(displayBuf)) copyLen = sizeof(displayBuf) - 1;
        std::memcpy(displayBuf, typeName, copyLen);
        displayBuf[copyLen] = 0;
        return displayBuf;
    }

    /// The name a new module of this type takes, or nothing when no such type is registered.
    static const char* defaultNameOf(const char* typeName) {
        for (uint8_t i = 0; typeName && i < count_; i++)
            if (std::strcmp(types_[i].name, typeName) == 0) return displayNameFor(types_[i].name, types_[i].role);
        return nullptr;
    }

    static uint8_t typeCount() { return count_; }   ///< how many types are registered
    /// The heap the table of types holds, which no module owns.
    static HeapBytes tableBytes() { return heapOf(types_, sizeof(TypeEntry) * capacity_); }
    /// Give back the room the table grew past its entries, once every type is registered: growing doubles, so a table of 159 types holds room for 255.
    static void shrinkToFit() {
        if (count_ == 0 || count_ == capacity_) return;
        auto* fit = static_cast<TypeEntry*>(platform::alloc(sizeof(TypeEntry) * count_));
        if (!fit) return;   // the roomier table keeps working
        for (uint8_t i = 0; i < count_; i++) fit[i] = types_[i];
        platform::free(types_);
        types_ = fit;
        capacity_ = count_;
    }
    /// The registered name at an index, or nothing when it is out of range.
    static const char* typeName(uint8_t i) { return (types_ && i < count_) ? types_[i].name : nullptr; }
    /// The role a type declares, generic when it declares none.
    static ModuleRole typeRole(uint8_t i) { return (types_ && i < count_) ? types_[i].role : ModuleRole::Generic; }
    /// The documentation page a type names, which the interface links to.
    static const char* typeDocPath(uint8_t i) { return (types_ && i < count_) ? types_[i].docPath : ""; }
    /// The tags a type declares, which the interface renders as its emoji.
    static const char* typeTags(uint8_t i) { return (types_ && i < count_) ? types_[i].tags : ""; }
    /// How many dimensions the type declares, or none when it declares no such method.
    static uint8_t typeDim(uint8_t i) { return (types_ && i < count_) ? types_[i].dim : 0; }
    /// Which child roles this type accepts, empty for none, which is how the interface knows what may be added under it.
    static const char* typeAcceptsChildRoles(uint8_t i) { return (types_ && i < count_) ? types_[i].acceptsChildRoles : ""; }
    /// The reader's mode a type's cards need, as a control's `minMode` (Control.h): user for every type until a build says otherwise.
    static uint8_t typeMinMode(uint8_t i) { return (types_ && i < count_) ? types_[i].minMode : 0; }
    /// Raise a type's cards to `mode`, for what a build carries as a preview of hardware it lacks; false for a type it has not registered.
    static bool setTypeMinMode(const char* typeName, uint8_t mode) {
        for (uint8_t i = 0; i < count_; i++)
            if (std::strcmp(types_[i].name, typeName) == 0) { types_[i].minMode = mode; return true; }
        return false;
    }

    /// Name a type this build leaves out on purpose behind a capability gate (`hasSdCard` on a `-lite` firmware), so a state document naming it skips that member, where a type no build knows refuses the document.
    static void registerCompiledOut(const char* typeName) {
        if (isCompiledOut(typeName)) return;
        for (const char*& slot : compiledOut_)
            if (!slot) { slot = typeName; return; }   // the first free slot; a full list drops it
    }
    /// Whether this build left `typeName` out on purpose.
    static bool isCompiledOut(const char* typeName) {
        for (const char* name : compiledOut_)
            if (name && std::strcmp(name, typeName) == 0) return true;
        return false;
    }

private:
    static inline const char* compiledOut_[8] = {};   // the capability gates a build can close, one type each, filled from the front

    struct TypeEntry {
        const char* name;
        CreateModuleFn create;
        size_t classSize;
        const char* docPath;
        const char* tags;
        const char* acceptsChildRoles;  // comma-separated roles this type accepts as children; "" = none
        ModuleRole role;                // the three single bytes together, so they share one word
        uint8_t dim;                    // 0 = N/A; 1/2/3 for types whose probe.dimensions() returns a Dim
        uint8_t minMode;                // the reader's mode its cards need, a control's kModeUser to kModeDeveloper
    };

    static inline TypeEntry* types_ = nullptr;
    static inline uint8_t count_ = 0;
    static inline uint8_t capacity_ = 0;

    /// Make room for one more entry, doubling capacity; false when the allocation fails and the registration is refused.
    static bool grow() {
        if (count_ < capacity_) return true;
        uint8_t newCap = capacity_ == 0 ? 4 : (capacity_ < 128 ? capacity_ * 2 : 255);
        if (newCap == capacity_) return false;  // saturated at uint8_t ceiling
        auto* newArr = static_cast<TypeEntry*>(platform::alloc(sizeof(TypeEntry) * newCap));
        if (!newArr) return false;
        for (uint8_t i = 0; i < count_; i++) newArr[i] = types_[i];
        if (types_) platform::free(types_);
        types_ = newArr;
        capacity_ = newCap;
        return true;
    }
};

} // namespace mm
