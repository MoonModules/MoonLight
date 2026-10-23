/// The MoonLive runtime: compiled script code, its control arena, and the calls into both.
/// The language, the compiler and the instruction encoders live behind MoonLive.h.

#include "core/moonlive/MoonLive.h"
#include <cstdio>
#include "core/moonlive/MoonLiveCompiler.h"
#include "platform/platform.h"

namespace mm::moonlive {


// The arena's address survives a recompile, so a control pointer the binding bound stays valid.
/// Drop the prior compilation's code, keeping the control arena.
void MoonLive::freeCode() {
    if (code_) platform::freeExec(code_, codeCap_);
    code_ = nullptr;
    codeCap_ = 0;
    codeLen_ = 0;
    fn_ = nullptr;
    anim_ = nullptr;
    ctrl_ = nullptr;
    // Only a parse failure has a position, so a later failure would report whatever failed last.
    errorPos_ = 0;
    hasErrorPos_ = false;
    // The entry table describes code that is gone, so entry() would hand out a freed address.
    entryCount_ = 0;
    // The pool stays until the next compile replaces it, so text a control still names is never freed under it.
    controlCount_ = 0;
    stringLen_ = 0;
}

// writeExec hides the ISA quirks, so the engine stays target-agnostic.
/// Copy emitted bytes into a fresh exec block, or nullptr on failure.
void* MoonLive::place(const uint8_t* staged, size_t len) {
    freeCode();   // a recompile is a clean re-emit, and keeps the arena
    if (len == 0) { error_ = "emit failed"; return nullptr; }
    // Only what was emitted, word-rounded, since writeExec stores 32-bit words on IRAM.
    size_t cap = (len + 3) & ~size_t(3);
    void* block = platform::allocExec(cap);
    if (!block) { error_ = "no executable memory"; return nullptr; }
    platform::writeExec(block, staged, len);
    code_ = block;
    codeCap_ = cap;
    codeLen_ = len;
    error_ = "";
    errorPos_ = 0;
    return block;
}

// On the heap and sized per compile: 2 KB of stack here overflowed the classic ESP32's task.
/// The emitted-code staging buffer, freed when it leaves scope.
namespace {
struct Staging {
    /// Allocate a buffer of the given size; null when the allocation failed.
    explicit Staging(size_t bytes) : p(static_cast<uint8_t*>(platform::alloc(bytes))), n(bytes) {}
    /// Release the buffer.
    ~Staging() { platform::free(p); }
    /// Non-copyable: it owns a buffer, and a copy would double-free.
    Staging(const Staging&) = delete;
    /// Non-assignable, for the reason it is non-copyable.
    Staging& operator=(const Staging&) = delete;
    /// Whether the buffer was allocated.
    explicit operator bool() const { return p != nullptr; }
    uint8_t* p;                                    ///< the buffer, or null
    size_t   n;                                    ///< its size in bytes
};
}  // namespace

/// Compile the fixed fill blob that paints one color.
bool MoonLive::compile(uint8_t r, uint8_t g, uint8_t b) {
    // A fixed blob with no source to measure, so codeCapFor(0) gives its 256-byte floor.
    Staging staging(codeCapFor(0));
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitFill(staging.p, staging.n, r, g, b);
    void* block = place(staging.p, len);
    if (!block) return false;
    fn_ = asFunction<FillFn>(block);
    return true;
}

/// Compile a script against a builtin and system-variable vocabulary; false sets `error()`.
bool MoonLive::compile(const char* source, const BuiltinTable& table, const SysVarTable& sysvars) {
    Staging staging(codeCapFor(countTokens(source)));
    if (!staging) { freeCode(); error_ = "no memory to compile"; return false; }
    // A pool sized to this script's literals, the new one beside the old until the compile succeeds, so a control name read meanwhile still points at text.
    const uint32_t want = countStringBytes(source);
    if (want > CompileResult::kMaxStrings) { freeCode(); error_ = "the script's strings pass 16 KB"; return false; }
    char* pool = want ? static_cast<char*>(platform::alloc(want)) : nullptr;
    if (want && !pool) { freeCode(); error_ = "no memory for the script's strings"; return false; }
    CompileResult cr = compileSource(source, table, sysvars, staging.p, staging.n,
                                     nullptr, nullptr, pool, static_cast<uint16_t>(want));
    // errorCol is one-based and every consumer counts from zero, so it converts at this boundary.
    if (!cr.ok) {
        platform::free(pool);
        freeCode();
        // One file-static line, since a per-result buffer dangled and a per-engine one repeats.
        if (cr.error == kSpillRefused) {
            // 160: a 63-byte literal plus six three-digit fields, which 112 could clip.
            static char detail[160];
            const SpillDetail& d = spillDetail();
            std::snprintf(detail, sizeof(detail), "%s (guard %u, avail %u, temps %u, vregs %u, slots %u, spilled %u)",
                          kSpillRefused, d.guard, d.avail, d.temps, d.vregs, d.slots, d.spilled);
            error_ = detail;
        } else {
            error_ = cr.error;
        }
        errorPos_ = cr.errorCol > 0 ? static_cast<uint16_t>(cr.errorCol - 1) : 0;
        hasErrorPos_ = true;
        return false;
    }
    // Before publishing, since ensureArena reads the previous count to know which slots are new.
    if (!ensureArena(cr.members, cr.memberCount)) { platform::free(pool); freeCode(); error_ = "no control memory"; return false; }
    // Publish the control set only once place() succeeds, never for code that is not running.
    void* block = place(staging.p, cr.len);
    if (!block) { platform::free(pool); return false; }            // controlCount_/controls_ unchanged
    retirePool();
    strings_ = pool;
    stringCap_ = static_cast<uint16_t>(want);
    // Names included, since a result's `name` points into source the caller frees on return.
    entryCount_ = cr.entryCount < kMaxEntryPoints ? cr.entryCount : kMaxEntryPoints;
    for (uint8_t i = 0; i < entryCount_; i++) {
        const uint8_t n = cr.entries[i].nameLen < kMaxEntryName ? cr.entries[i].nameLen : kMaxEntryName;
        for (uint8_t j = 0; j < n; j++) entryNames_[i][j] = cr.entries[i].name[j];
        entryNames_[i][n] = '\0';
        // The declared return type travels along, or runValue refuses to answer for any entry.
        entries_[i] = {entryNames_[i], n, cr.entries[i].offset, cr.entries[i].ret};
    }
    stringLen_ = cr.stringLen;
    ctrl_ = asFunction<CtrlFn>(block);
    return true;
}

// Allocated once at full capacity, so every bound control pointer stays fixed for the engine.
/// Ensure the control arena exists and seed new slots; false when the allocation failed.
bool MoonLive::ensureArena(const DeclaredControl* decls, uint8_t count) {
    if (!ctrlArena_) {
        ctrlArena_ = static_cast<uint8_t*>(platform::alloc(kArenaBytes));
        if (!ctrlArena_) return false;
        for (uint8_t i = 0; i < kArenaBytes; i++) ctrlArena_[i] = 0;
    }
    // Same means offset and name both, since a member inserted at the top shifts every later one.
    uint64_t seeding = 0;
    uint8_t  kept = 0;                 // rows written this pass; the table is per MEMBER
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t off = decls[i].offset;
        // Bounded by the script's region, which the mask and the name table cover.
        if (off >= kCtrlBytes) continue;
        // The name is an unterminated span, so strcmp would read into the rest of the script.
        const uint8_t n = decls[i].nameLen < kSeedNameLen - 1 ? decls[i].nameLen
                                                             : uint8_t(kSeedNameLen - 1);
        // The offset alone is not identity, since an insertion shifts every later member down.
        const SeededMember* prev = nullptr;
        if ((seeded_ >> off) & 1ull)
            for (uint8_t k = 0; k < seededCount_; k++)
                if (seededName_[k].offset == off) { prev = &seededName_[k]; break; }
        // Shape too, since a widened scalar keeps its name and offset but not its old bytes.
        const bool same = prev && std::strncmp(prev->name, decls[i].name, n) == 0 &&
                          prev->name[n] == '\0' &&
                          prev->type == decls[i].type && prev->count == decls[i].count;
        if (!same) {
            // The whole extent at its width, since the low byte alone left a wider member stale.
            const uint8_t w = decls[i].count > 1 ? ctrlWidth(decls[i].type)
                                                 : ctrlSlotBytes(decls[i].type);
            const uint32_t v = static_cast<uint32_t>(decls[i].def);
            for (uint16_t e = 0; e < decls[i].count; e++) {
                const uint16_t at = uint16_t(off + e * w);
                if (at + w > kCtrlBytes) break;                 // the parser bounds it already
                for (uint8_t b = 0; b < w; b++)
                    ctrlArena_[at + b] = static_cast<uint8_t>((v >> (8 * b)) & 0xff);
            }
        }
        if (kept < kMaxCtrls) {
            seededName_[kept].offset = off;
            seededName_[kept].type   = decls[i].type;
            seededName_[kept].count  = static_cast<uint8_t>(decls[i].count);
            for (uint8_t c = 0; c < n; c++) seededName_[kept].name[c] = decls[i].name[c];
            seededName_[kept].name[n] = '\0';
            kept++;
        }
        seeding |= 1ull << off;
    }
    seeded_ = seeding;   // a member the new script dropped is unseeded: its byte reseeds if it returns
    seededCount_ = kept;
    return true;
}

/// Compile the fixed animated fill blob.
bool MoonLive::compileAnimated() {
    Staging staging(codeCapFor(0));   // a fixed blob, so there is no source to measure
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitAnimatedFill(staging.p, staging.n);
    void* block = place(staging.p, len);
    if (!block) return false;
    anim_ = asFunction<AnimFn>(block);
    return true;
}

MoonLive::~MoonLive() {
    free();
    platform::free(retired_);   // nothing names its text once the engine goes
}

/// The current pool retires and the one retired before it goes: the module's controls name text in the current one until it rebuilds them.
void MoonLive::retirePool() {
    platform::free(retired_);
    retired_ = strings_;
    retiredCap_ = stringCap_;
    strings_ = nullptr;
    stringCap_ = 0;
}

/// Release everything: the exec block, the fn pointers and the control arena.
void MoonLive::free() {
    freeCode();                       // exec block + fn pointers
    retirePool();                     // the pool goes with the program, once the module's controls stop naming it
    if (ctrlArena_) platform::free(ctrlArena_);   // full release also releases the control arena
    ctrlArena_ = nullptr;
    controlCount_ = 0;
    // The seeded count goes with its arena, or the next compile skips every initializer.
    seeded_ = 0;
    seededCount_ = 0;
}

}  // namespace mm::moonlive
