#pragma once

#include "core/module/MoonModule.h"
#include "core/moonlive/MoonLiveBuiltins_common.h"   // runDefineControls
#include "core/moonlive/MoonLiveScriptFile.h"

#include <cstring>

namespace mm::moonlive {

/// One scripted module's script: the file it names, the compiled program, and the content hash.
///
/// The rule it exists to state once: if the file changed, recompile. That is all `sync()` is.
///
/// @moreinfo
///
/// ## Held by value
///
/// The three scripted modules derive from three sibling bases under MoonModule.
/// A shared base would need virtual inheritance and change the layout of every module.
///
/// ## One answer to one question
///
/// The three bindings each grew their own bookkeeping around that rule and drifted apart.
/// An effect had no content hash, so editing a script changed nothing until it was renamed.
/// A layout cleared its hash only on a name change, and only a modifier re-read the file.
class MoonLiveScript {
public:
    // Sized from the worst line the code can produce, which the status-fits test recomputes.
    /// The longest status this module reports, with its offset suffix.
    static constexpr size_t kMaxStatus = 112;
    /// What the status says when the user's copy is hiding a shipped one.
    static constexpr const char* kShadowMark = "edited copy";
    /// Said instead when the shipped copy has moved on since the fork.
    static constexpr const char* kStaleMark = "edited copy, shipped one updated";

    /// Let a binding attach its own sinks around the script's defineControls(), such as a pool it sizes.
    void setDefineHook(DefineHook fn, void* ctx) { defineHook_ = fn; defineCtx_ = ctx; }

    // True means a new program was installed, which a modifier turns into a Layer rebuild.
    /// Re-read the file and recompile when its content hash moved.
    bool sync(const SysVarTable& sysvars, MoonModule& owner, const BuiltinTable& builtins) {
        // Cheapest question first, since this runs on every prepare sweep and must not compile.
        uint32_t fileHash = 0;
        const bool readable = scriptFileHash(name_, fileHash);
        if (readable && engine_.ok() && haveCompiled_ && fileHash == compiledHash_) return false;

        // Without this latch the retries starve the task until the watchdog resets the device.
        if (compileFailed_ && std::strcmp(failedScript_, name_) == 0 &&
            (!readable ? !failedReadable_ : (failedReadable_ && fileHash == failedHash_)))
            return false;

        resetPrintBudget();
        const char* err = nullptr;
        uint32_t hash = 0;
        if (compileScriptFile(engine_, name_, builtins, sysvars, err, &hash)) {
            // By running defineControls(), the way a compiled module does.
            runDefineControls(engine_, defineHook_, defineCtx_);
            // What the script says it is, read once per compile rather than per frame.
            readIdentity();
            // How big the program is and which budget it is closest to, which the card cannot say.
            engine_.describe(statusBuf_, sizeof(statusBuf_));
            // Named on success as much as on failure, since a push upstream changes nothing here.
            if (shadowMark()) {
                const size_t n = std::strlen(statusBuf_);
                std::snprintf(statusBuf_ + n, sizeof(statusBuf_) - n, ", %s", shadowMark());
            }
            owner.setStatus(statusBuf_, MoonModule::Severity::Status);
            compileFailed_ = false;
        } else {
            // One string, since status is the channel a module reports through.
            if (engine_.hasErrorPos()) {
                // Suffix first, so a long message loses its tail rather than the offset.
                char at[12];
                std::snprintf(at, sizeof(at), " @%u", static_cast<unsigned>(engine_.errorPos()));
                const size_t room = sizeof(statusBuf_) - std::strlen(at) - 1;
                const char* mark = shadowMark();
                int n = std::snprintf(statusBuf_, room + 1, "%s%s%s",
                                      mark ? mark : "", mark ? ": " : "",
                                      err ? err : "compile failed");
                if (n < 0) n = 0;
                if (static_cast<size_t>(n) > room) n = static_cast<int>(room);
                std::snprintf(statusBuf_ + n, sizeof(statusBuf_) - static_cast<size_t>(n), "%s", at);
                owner.setStatus(statusBuf_, MoonModule::Severity::Error);
            } else {
                owner.setStatus(err, MoonModule::Severity::Error);
            }
            // Forget what the last script said: its strings are already overwritten in the pool.
            dim_ = Dim::D2;
            tags_ = nullptr;
            compileFailed_ = true;
            failedHash_ = fileHash;
            failedReadable_ = readable;   // distinguishes "this text is broken" from "no file"
            std::snprintf(failedScript_, sizeof(failedScript_), "%s", name_);
        }
        // A separate flag rather than hash != 0, since 0 is a legitimate hash a script may have.
        compiledHash_ = hash;
        haveCompiled_ = engine_.ok();   // a FAILED compile has no program, whatever the file hashed to
        // Added rather than assigned, since a binding may own buffers that report themselves.
        const size_t nowBytes = engine_.heapBytes();
        owner.setDynamicBytes(owner.dynamicBytes() - reportedBytes_ + nowBytes);
        reportedBytes_ = nowBytes;
        return true;
    }

    // A control write lands here directly, which is why sync() re-derives from the file.
    /// The name buffer the `script` control binds to.
    char*  buffer() { return name_; }
    /// How many bytes the name buffer holds.
    size_t bufferSize() const { return sizeof(name_); }
    /// The script file this module runs.
    const char* name() const { return name_; }

    /// Point at a different script. The next sync() compiles it.
    void setName(const char* n) {
        if (!n) return;
        std::snprintf(name_, sizeof(name_), "%s", n);
        invalidate();
    }

    // D2 is the fallback, so a script that stays silent behaves exactly as it always did.
    /// The dimensionality the script declared, or D2 when it declared none.
    Dim dimensions() const { return dim_; }

    // Points into the engine's string pool, so it is valid until the next compile replaces it.
    /// The emoji the script declared, or null when it declared none.
    const char* tags() const { return tags_; }

    // Subtracting rather than zeroing, so a binding may own other memory the owner still counts.
    /// Hand back what this script reported, called after the engine is freed.
    void releaseReporting(MoonModule& owner) {
        const size_t held = owner.dynamicBytes();
        owner.setDynamicBytes(held > reportedBytes_ ? held - reportedBytes_ : 0);
        reportedBytes_ = 0;
    }

    /// Forget what is compiled, so the next sync rebuilds for a module coming back from disabled.
    void invalidate() {
        compiledHash_ = 0;
        haveCompiled_ = false;
        compileFailed_ = false;
        failedHash_ = 0;
        failedReadable_ = false;
        failedScript_[0] = '\0';
    }

    // One home, since all three bindings publish identically.
    /// Publish every control the script declared, bound by reference to its live arena slot.
    void publishDeclaredControls(ControlList& controls) {
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;   // engine not compiled yet: controls appear after prepare
            // A dropdown reads the low byte too, its value being an index below its option count.
            if (decls[i].options) {
                controls.addSelect(decls[i].name, *slot, decls[i].options, static_cast<uint8_t>(decls[i].max + 1));
                controls.setDefault(controls.count() - 1, static_cast<int32_t>(decls[i].def));
                continue;
            }
            // byte and bool read the low byte, which holds because a store masks the upper three.
            switch (decls[i].type) {
                case moonlive::CtrlType::Bool:
                    // Normalized before the byte is read as a bool, which a store leaves free.
                    *slot = (*slot != 0) ? 1 : 0;
                    controls.addControl(decls[i].name, *reinterpret_cast<bool*>(slot));
                    break;
                case moonlive::CtrlType::Byte:
                    controls.addControl(decls[i].name, *slot,
                                      static_cast<uint8_t>(decls[i].min),
                                      static_cast<uint8_t>(decls[i].max));
                    break;
                default:   // Int; Fixed and Str never reach here (the compiler refuses to bind one)
                    controls.addControl(decls[i].name, *reinterpret_cast<int32_t*>(slot),
                                      decls[i].min, decls[i].max);
                    break;
            }
            // The member's initializer is the only default there is.
            controls.setDefault(controls.count() - 1, static_cast<int32_t>(decls[i].def));
        }
    }

    /// The engine holding the compiled program.
    MoonLive&       engine()       { return engine_; }
    /// The engine holding the compiled program, for a reader.
    const MoonLive& engine() const { return engine_; }
    /// Whether a program is compiled and ready to run.
    bool ok() const { return engine_.ok(); }

private:
    // A script declaring neither keeps the binding's defaults, as every older script does.
    /// Read what the script says it is, by running the function it wrote.
    void readIdentity() {
        dim_ = Dim::D2;
        tags_ = nullptr;
        // runValue answers only for a matching declared type, so a void one gets the fallback.
        const uintptr_t d = engine_.runValue("dimensions", moonlive::RetType::Int, 2);
        if (d >= 1 && d <= 3) dim_ = static_cast<Dim>(d);
        const uintptr_t s = engine_.runValue("tags", moonlive::RetType::Str, 0);
        if (s) tags_ = reinterpret_cast<const char*>(s);
    }

    MoonLive engine_;
    /// What the compiled script declared, cached so no call lands on the tick path.
    Dim         dim_  = Dim::D2;
    const char* tags_ = nullptr;
    // Asked once per status line, so the success and failure branches cannot drift apart.
    /// Which shadow note the status should carry, or null when this is the only copy.
    const char* shadowMark() const {
        if (!moonlive::scriptShadowsFactory(name_)) return nullptr;
        return moonlive::scriptFactoryMovedOn(name_) ? kStaleMark : kShadowMark;
    }

    char     statusBuf_[kMaxStatus] = {};
    // Empty on a fresh card, reporting "no script" until one is named.
    char     name_[kMaxScriptName + 1] = "";
    uint32_t compiledHash_ = 0;
    bool     haveCompiled_ = false;   // 0 is a valid hash, so "is anything compiled" is its own flag
    // Name and content both, so a script fixed in place under the same name is retried.
    bool     compileFailed_ = false;
    bool     failedReadable_ = false;   // was there a file at all when it failed?
    uint32_t failedHash_ = 0;
    char     failedScript_[kMaxScriptName + 1] = "";

    size_t     reportedBytes_ = 0;   // what this script last added to the owner's total
    DefineHook  defineHook_ = nullptr;
    void*       defineCtx_  = nullptr;
};

}  // namespace mm::moonlive
