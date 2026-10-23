#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveIr.h"      // DeclaredControl, kMaxCtrls (surfaced on CompileResult)
#include "core/moonlive/moonlive_emit.h"   // RegBudget, the test-only budget override

/// Source text to tokens to AST to IR, and then to native code through the per-ISA assembler.
///
/// @technical
///
/// The grammar is a statement calling a host-registered function with expression arguments, so any argument may be a literal or a nested call.
///
/// The compiler is neutral by construction: it knows the language and resolves call names against the injected table. Owning no function names and no domain semantics, it leaves the backend lowering a generic call or a generic inline op.

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
    /// What this function returns, as the script declared it; a host reads a value only from a function saying it has one.
    RetType     ret = RetType::Void;
};

/// How many named functions one script may define.
inline constexpr uint8_t kMaxEntryPoints = 8;

/// Longest function name the engine keeps.
inline constexpr uint8_t kMaxEntryName = 23;

/// Longest class name kept, which a diagnostic quotes; bounded rather than pointing into source, which is freed as soon as the compile returns.
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
    /// Every member the class declared, each seeded with its initializer even when the UI hides it.
    DeclaredControl members[kMaxCtrls];
    /// How many members the class declared.
    uint8_t         memberCount = 0;
    /// The most string-literal text one script holds, interned into a pool the engine sizes to the script because the source buffer is freed after a compile.
    static constexpr uint16_t kMaxStrings = 16384;
    /// How much of the string pool is used, so a binding can report the headroom.
    uint16_t        stringLen = 0;
    /// The class name diagnostics report, so renaming a file does not change what a user is told.
    char            className[kMaxClassName + 1] = "";
    /// The functions this script defined, in source order.
    EntryPoint      entries[kMaxEntryPoints];
    /// How many named functions the script defined.
    uint8_t         entryCount = 0;
};

/// A backend's lowering of an IR program into machine code in a buffer, within an optional register budget; it returns the byte count, 0 when the program cannot be encoded.
using LowerFn = size_t (*)(IrProgram&, uint8_t*, size_t, const RegBudget*);

/// Compile `source` to machine code in `out`, resolving calls against `table`. Pure: `sysvars` are read-only names, `squeeze` overrides the register budget for a test, and `strings` is the caller's because the emitted code points into it.
CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze = nullptr, LowerFn lower = nullptr,
                            char* strings = nullptr, uint16_t stringCap = 0);

/// Tokens in `source`, which both right-sized buffers derive from; one function, so a caller cannot measure a script differently from the compiler.
uint32_t countTokens(const char* source);

/// Bytes the string pool needs for `source`: every literal and its terminator, as the compiler interns them.
uint32_t countStringBytes(const char* source);

}  // namespace mm::moonlive
