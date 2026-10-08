#pragma once

#include <cstdint>
#include <cstdio>   // snprintf, for describe()
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveCompiler.h"   // CompileResult (carries the declared controls)
#include <cstring>   // std::strcmp: entry lookup by name

namespace mm::moonlive {

/// Compiling a script to native code and running it over a host-supplied buffer.
///
/// @moreinfo
///
/// A compile turns a program into native code and places it in executable memory, and a run calls it.
/// The path is emit, allocate, call, write, and it is the same on Xtensa, RISC-V and the host.
///
/// The engine is domain-neutral by construction: it includes the compiler and platform seams and never a light type.
/// The binding is what wraps it as a MoonModule.
class MoonLive {
public:
    /// An engine with nothing compiled, until a compile call fills it.
    MoonLive() = default;
    /// Release the exec block and the control arena.
    ~MoonLive() { free(); }

    // Owns a heap-backed exec block, so copying would duplicate ownership and double-free.
    /// Never copied: each scripted module holds its own engine.
    MoonLive(const MoonLive&) = delete;
    MoonLive& operator=(const MoonLive&) = delete;

    // A failure leaves the engine not ok() with an error(), so the caller degrades.
    /// Compile a fixed-color program direct from the emitter.
    bool compile(uint8_t r, uint8_t g, uint8_t b);

    // A parse or codegen error leaves error() pointing at the diagnostic the editor shows.
    /// Compile source text, resolving calls against the host's registered builtins.
    bool compile(const char* source, const BuiltinTable& table, const SysVarTable& sysvars);

    /// Compile the animated routine, whose color derives from the per-frame `t`.
    bool compileAnimated();

    /// Whether a program is compiled and callable.
    bool ok() const { return fn_ != nullptr || anim_ != nullptr || ctrl_ != nullptr; }

    // Every function sits in the one emitted block, so any number costs one allocation.
    /// The compiled function named `name`, or null when the script defined none.
    CtrlFn entry(const char* name) const {
        const uint8_t* p = entryCode(name);
        return p ? reinterpret_cast<CtrlFn>(reinterpret_cast<uintptr_t>(p)) : nullptr;
    }

    /// What a named entry point declared it returns, or Void when the script has no such function.
    RetType retTypeOf(const char* name) const {
        if (!name) return RetType::Void;
        for (uint8_t i = 0; i < entryCount_; i++)
            if (std::strcmp(entryNames_[i], name) == 0) return entries_[i].ret;
        return RetType::Void;
    }

    // Emitted code has no C++ type and is called through two, so each caller reads the address.
    /// The address of a named entry point, with no signature attached.
    const uint8_t* entryCode(const char* name) const {
        if (!code_ || !name) return nullptr;
        for (uint8_t i = 0; i < entryCount_; i++) {
            if (std::strcmp(entryNames_[i], name) != 0) continue;
            if (entries_[i].offset >= codeLen_) return nullptr;   // a corrupt map is not callable
            return static_cast<const uint8_t*>(code_) + entries_[i].offset;
        }
        return nullptr;
    }

    /// The name of the function at index `i`, or null past the end.
    const char* entryName(uint8_t i) const { return i < entryCount_ ? entryNames_[i] : nullptr; }
    /// How many functions the script defined.
    uint8_t entryCount() const { return entryCount_; }
    /// The diagnostic from the last compile, or an empty string.
    const char* error() const { return error_; }

    // The editor turns it into a line to mark, which the parser already knows.
    /// Where the last compile failed, as a character offset into the source.
    uint16_t errorPos() const { return errorPos_; }
    // Zero is a valid offset, so the value cannot also stand for "no position".
    /// Whether `errorPos` means anything, which only a parse failure gives.
    bool     hasErrorPos() const { return hasErrorPos_; }

    // The hot path: a buffer too small to hold RGB is left untouched rather than overrun.
    /// Run the entry point called `name`, or the whole program when `name` is null.
    void run(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t,
             const char* name = nullptr) const {
        if (!buf || nLights == 0 || cpl < 3) return;
        // The emitted code dereferences the arena, so a null faults with no C++ frame to blame.
        if (ctrl_ && ctrlArena_) {
            // A script that hit the limit stopped calling, so a leaked level would shrink the budget.
            ctrlArena_[kDepthSlot] = 0;
            // A named entry when asked, otherwise the block start a single-function script wants.
            CtrlFn f = name ? entry(name) : ctrl_;
            if (f) f(buf, nLights, cpl, t, ctrlArena_);
        }
        else if (fn_) fn_(buf, nLights, cpl);                 // hand-encoded fixed fill
        else if (anim_) anim_(buf, nLights, cpl, t);          // hand-encoded animated fill
    }

    // `fallback` rather than 0, since "did not say" and "said 0" are different answers.
    /// Run the entry point called `name` and return its answer, or `fallback` when it has none.
    uintptr_t runValue(const char* name, RetType want, uintptr_t fallback = 0,
                       uint8_t* buf = nullptr, uint32_t nLights = 0, uint8_t cpl = 0,
                       uint32_t t = 0) const {
        if (!name || !ctrl_ || !ctrlArena_) return fallback;
        // The declared type decides whether there is a value, since a void call reads the register.
        if (retTypeOf(name) != want) return fallback;
        // Through the code address, since casting between return types is what the warning catches.
        const uint8_t* code = entryCode(name);
        if (!code) return fallback;
        ValueFn f = reinterpret_cast<ValueFn>(reinterpret_cast<uintptr_t>(code));
        ctrlArena_[kDepthSlot] = 0;      // same fresh-depth contract as run()
        return f(buf, nLights, cpl, t, ctrlArena_);
    }

    // One home for the byte order, which the engine, the seeding pass and the binding share.
    /// Read a member's 4-byte slot, little-endian.
    int32_t readSlot(uint8_t offset) const {
        if (!ctrlArena_ || offset + 4 > kArenaBytes) return 0;
        return int32_t(uint32_t(ctrlArena_[offset]) | (uint32_t(ctrlArena_[offset + 1]) << 8) |
                       (uint32_t(ctrlArena_[offset + 2]) << 16) |
                       (uint32_t(ctrlArena_[offset + 3]) << 24));
    }
    /// Write a member's 4-byte slot, little-endian.
    void writeSlot(uint8_t offset, int32_t v) {
        if (!ctrlArena_ || offset + 4 > kArenaBytes) return;
        const uint32_t u = uint32_t(v);
        ctrlArena_[offset]     = uint8_t(u & 0xff);
        ctrlArena_[offset + 1] = uint8_t((u >> 8) & 0xff);
        ctrlArena_[offset + 2] = uint8_t((u >> 16) & 0xff);
        ctrlArena_[offset + 3] = uint8_t((u >> 24) & 0xff);
    }

    // `name` must outlive the engine, pointing into the string pool the compiler interned it into.
    /// Append a control the running `defineControls()` declared; with `options`, "a|b|c", it is a dropdown of those names.
    void addDeclaredControl(const char* name, uint8_t offset, int32_t lo, int32_t hi,
                            CtrlType type = CtrlType::Int, const char* options = nullptr) {
        if (controlCount_ >= kMaxCtrls || !name || offset >= kArenaBytes) return;
        // A scalar owns a whole 4-byte slot, so the engine refuses one running past the arena.
        if (offset + ctrlSlotBytes(type) > kArenaBytes) return;
        // Two controls on one member would write the same byte from two cards; refused before a dropdown's names take pool space.
        for (uint8_t i = 0; i < controlCount_; i++)
            if (controls_[i].offset == offset) return;
        const char* const* optionList = nullptr;
        if (options) {
            const uint8_t first = selectCount_;
            if (!splitOptions(options)) return;   // no room for its names: no dropdown rather than a cut-off one
            optionList = &selectPtrs_[first];
            lo = 0;
            hi = selectCount_ - first - 1;
        }
        if (lo > hi) return;

        // Clamped in the arena as well as the record, since the native code reads the arena byte.
        int32_t def = lo;
        if (ctrlArena_) def = readSlot(offset);
        if (def < lo) def = lo;
        else if (def > hi) def = hi;
        if (ctrlArena_) writeSlot(offset, def);
        controls_[controlCount_] = {name, lo, hi, def, 0, type, offset, 1, optionList};
        // Measured here rather than passed, and the bound is tested before the byte is read.
        uint8_t n = 0;
        while (n < kMaxControlName - 1 && name[n]) n++;
        controls_[controlCount_].nameLen = n;
        controlCount_++;
    }

    /// Forget the controls a previous `defineControls()` declared, so re-running it rebuilds.
    void clearDeclaredControls() { controlCount_ = 0; selectLen_ = 0; selectCount_ = 0; }

    /// Does the script define this entry point? A binding asks before reporting "no tick() to run".
    bool hasEntry(const char* name) const { return entry(name) != nullptr; }

    /// Release the exec block and the control arena.
    void free();

    // A bound control pointer stays valid and keeps the live value the user set.
    /// Drop the compiled code, keeping the control arena.
    void freeCode();

    // What a script author asks, where `heapBytes` answers what the module takes from the heap.
    /// How many bytes of machine code the current program is.
    size_t codeLen() const { return codeLen_; }
    /// The allocated exec-block size, which is the heap held.
    size_t codeCap() const { return codeCap_; }

    // The string pool is an inline member, so it is already counted in the module's own sizeof.
    /// Every heap byte this engine holds: the exec block plus the control arena.
    size_t heapBytes() const { return codeCap_ + (ctrlArena_ ? kArenaBytes : 0); }

    // One budget rather than five, since the others follow from code size. Shown past half full.
    /// Write the program's size and the one budget it is closest to exhausting into `out`.
    void describe(char* out, size_t cap) const {
        if (!out || !cap) return;
        if (!codeLen_) { out[0] = '\0'; return; }
        // Each actionable budget as a percentage, so the tightest is comparable across units.
        struct Budget { const char* name; uint32_t used, max; };
        const Budget budgets[] = {
            {"controls", controlCount_, kMaxCtrls},
            {"strings",  stringLen_,    CompileResult::kStringPool},
            {"code",     static_cast<uint32_t>(codeLen_), kCodeCap},
            {"entries",  entryCount_,   kMaxEntryPoints},
        };
        const Budget* worst = &budgets[0];
        for (const Budget& b : budgets)
            if (b.used * uint64_t(worst->max) > worst->used * uint64_t(b.max)) worst = &b;
        if (worst->used * 2 > worst->max)
            std::snprintf(out, cap, "%u B, %s %u/%u", unsigned(codeLen_),
                          worst->name, unsigned(worst->used), unsigned(worst->max));
        else
            std::snprintf(out, cap, "%u B", unsigned(codeLen_));
    }

    /// The controls the last compile declared, which the binding turns into real module controls.
    const DeclaredControl* declaredControls(uint8_t& count) const { count = controlCount_; return controls_; }
    // The arena never moves, so a bound pointer stays valid across every recompile.
    /// The live byte at an arena offset, or null for an offset the arena does not hold.
    uint8_t* controlSlot(uint8_t offset) { return (ctrlArena_ && offset < kArenaBytes) ? &ctrlArena_[offset] : nullptr; }

private:
    // Copied rather than split in place, since the string pool is the program's and a recompile reuses it.
    /// Split a dropdown's "a|b|c" into names this engine keeps; false when they do not fit.
    bool splitOptions(const char* text) {
        const uint8_t len0 = selectLen_, count0 = selectCount_;
        const char* p = text;
        while (true) {
            const char* end = p;
            while (*end && *end != '|') end++;
            const size_t n = static_cast<size_t>(end - p);
            if (n == 0 || selectCount_ >= kSelectOptions || selectLen_ + n + 1 > kSelectText) {
                selectLen_ = len0;   // all or nothing
                selectCount_ = count0;
                return false;
            }
            std::memcpy(selectText_ + selectLen_, p, n);
            selectText_[selectLen_ + n] = '\0';
            selectPtrs_[selectCount_++] = selectText_ + selectLen_;
            selectLen_ = static_cast<uint8_t>(selectLen_ + n + 1);
            if (!*end) return true;
            p = end + 1;
        }
    }
    /// Room for every dropdown one script declares: the names, and a pointer to each.
    static constexpr uint8_t kSelectText = 96, kSelectOptions = 16;
    char        selectText_[kSelectText] = {};
    const char* selectPtrs_[kSelectOptions] = {};
    uint8_t     selectLen_ = 0, selectCount_ = 0;

    // Returns the block, or null on failure with error_ set.
    void* place(const uint8_t* staged, size_t len);

    // Grows without moving, so a bound pointer survives a recompile and keeps its live value.
    bool ensureArena(const DeclaredControl* decls, uint8_t count);
    // Identity is offset plus name, since declaration position shifts when a member is inserted.
    static constexpr uint8_t kSeedNameLen = 12;
    // Type and count ride along, since a member's identity is its whole shape rather than its name.
    struct SeededMember {
        uint8_t  offset = 0;
        CtrlType type   = CtrlType::Int;
        uint8_t  count  = 1;
        char     name[kSeedNameLen] = {};
    };
    // A static_assert ties the two together: a mask that outgrew its budget aliased silently.
    static_assert(kCtrlBytes <= 64, "seeded_ is a 64-bit mask, one bit per script arena byte");
    uint64_t     seeded_ = 0;
    SeededMember seededName_[kMaxCtrls] = {};
    uint8_t      seededCount_ = 0;

    void*   code_ = nullptr;     // allocExec block holding the emitted machine code
    size_t  codeCap_ = 0;        // its capacity (for freeExec)
    size_t  codeLen_ = 0;        // bytes emitted
    FillFn  fn_ = nullptr;       // static fill (3-arg), or nullptr
    AnimFn  anim_ = nullptr;     // animated fill (4-arg, reads t), or nullptr
    CtrlFn  ctrl_ = nullptr;     // front-end-compiled routine (5-arg, reads the controls arena)
    const char* error_ = "";
    uint16_t    errorPos_ = 0;
    bool        hasErrorPos_ = false;

    // Names owned here, since a CompileResult's point into source the caller frees at once.
    EntryPoint entries_[kMaxEntryPoints] = {};
    char       entryNames_[kMaxEntryPoints][kMaxEntryName + 1] = {};
    uint8_t    entryCount_ = 0;

    uint8_t* ctrlArena_ = nullptr;   // live control + system-variable bytes (platform::alloc, kArenaBytes, fixed)
    uint8_t  controlCount_ = 0;      // controls the current program declared
    DeclaredControl controls_[kMaxCtrls] = {};   // the declared-control metadata for the binding
    // Copied, so the engine outlives the source text it was built from.
    char strings_[CompileResult::kStringPool] = {};
    // How much of the pool the current program uses, for the card's headroom readout.
    uint16_t stringLen_ = 0;
};

}  // namespace mm::moonlive

