#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveIr.h"      // DeclaredControl, kMaxCtrls (surfaced on CompileResult)
#include "core/moonlive/moonlive_emit.h"   // RegBudget, the test-only budget override

/// @defgroup MoonLiveCompiler The MoonLive front-end
/// @{
/// Source text to tokens to AST to IR, and then to native code through the per-ISA assembler.
///
/// @moreinfo
///
/// The grammar is a statement calling a host-registered function with expression arguments, so any argument may be a literal or a nested call.
///
/// The compiler is neutral by construction: it knows the language and resolves call names against the injected table.
/// Owning no function names and no domain semantics, it leaves the backend lowering a generic call or a generic inline op.

namespace mm::moonlive {

inline constexpr const char* kCodegenFailed = "codegen failed (unsupported on this target, or too large)";
inline constexpr const char* kSpillRefused  = "codegen failed: too many live values for this chip's registers";


// A symbol table: a binding asks for an entry by name, so a script's role follows from what it defined.
/// What a function hands back, where `void` is the default an acting entry point declares.
enum class RetType : uint8_t { Void, Int, Str, Struct };

struct EntryPoint {
    const char* name = nullptr;    ///< into the source, or the engine's own copy after compile
    /// How many characters of `name` are used.
    uint8_t     nameLen = 0;
    uint16_t    offset = 0;        ///< byte offset of its first instruction within the block
    // A host reads a value only from a function saying it has one.
    /// What this function returns, as the script declared it.
    RetType     ret = RetType::Void;
};

/// How many named functions one script may define.
inline constexpr uint8_t kMaxEntryPoints = 8;

/// Longest function name the engine keeps.
inline constexpr uint8_t kMaxEntryName = 23;

// Bounded rather than pointing into source, which is freed as soon as the compile returns.
/// Longest class name kept, which a diagnostic quotes.
inline constexpr size_t kMaxClassName = 31;

struct CompileResult {
    /// Whether the compile succeeded.
    bool        ok = false;
    /// A static diagnostic when the compile failed.
    const char* error = "";
    /// The 1-based column the diagnostic points at, or 0.
    uint16_t    errorCol = 0;
    /// How many bytes of machine code were emitted.
    size_t      len = 0;
    // Every member the class declared, each seeded with its initializer even when the UI hides it.
    DeclaredControl members[kMaxCtrls];
    /// How many members the class declared.
    uint8_t         memberCount = 0;
    // Interned into a pool the engine owns, because the source buffer is freed after a compile.
    /// Bytes of string-literal storage the engine holds.
    static constexpr uint16_t kStringPool = 128;
    // How many of those bytes this program interned, so a binding can report the headroom.
    /// How much of the string pool is used.
    uint16_t        stringLen = 0;
    // What diagnostics report, so renaming a file does not change what a user is told.
    char            className[kMaxClassName + 1] = "";
    // The functions this script defined, in source order.
    EntryPoint      entries[kMaxEntryPoints];
    /// How many named functions the script defined.
    uint8_t         entryCount = 0;
};

// Pure. `sysvars` are read-only names, `squeeze` overrides the register budget for a test.
/// Compile `source` to machine code in `out`, resolving calls against `table`.
using LowerFn = size_t (*)(IrProgram&, uint8_t*, size_t, const RegBudget*);

// `strings` is the caller's because the emitted code carries pointers into it.
CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze = nullptr, LowerFn lower = nullptr,
                            char* strings = nullptr, uint16_t stringCap = 0);

// One function, so a caller cannot measure a script differently from the compiler.
/// Tokens in `source`, which is what both right-sized buffers derive from.
uint32_t countTokens(const char* source);

/// @}
}  // namespace mm::moonlive
