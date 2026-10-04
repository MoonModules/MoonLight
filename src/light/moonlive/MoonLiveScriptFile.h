#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/moonlive/script_catalog.h"
#include "core/util/fnv.h"   // the shipped names, for isFactoryScript below
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

/// @defgroup moonlive_script_file MoonLive script files
/// @{
/// Where scripts live and how a file name states its role.
///
/// One language, five extensions: the engine stays role-blind and the extension alone decides which picker offers a file.
///
/// @moreinfo
///
/// ## Two directories
///
/// A user directory the UI writes and the loader prefers, and a factory directory the picker offers and the UI downloads from on first use.
/// The split is the revert mechanism: un-editing is a local delete rather than a download.

namespace mm::moonlive {

// A module stores a name, not a path, so it cannot reach outside this folder.
/// Where a user's scripts live, which the UI writes and the loader prefers.
inline constexpr const char* kScriptDir = "/moonlive";

// The split is the revert mechanism: un-editing is a local delete rather than a download.
/// Where the factory scripts land, which the picker offers and the UI downloads on first use.
inline constexpr const char* kFactoryScriptDir = "/.moonlive";

// The engine stays role-blind, so the extension alone decides which picker offers a file.
/// A script's role, carried in its file name: one language, five extensions.
inline constexpr const char* kEffectExt   = ".mle";
inline constexpr const char* kLayoutExt   = ".mll";
inline constexpr const char* kModifierExt = ".mlm";
inline constexpr const char* kServiceExt  = ".mls";
inline constexpr const char* kPaletteExt  = ".mlp";

// A working example rather than an empty file, which would fail to parse the moment it is made.
/// What a new script starts out as, per role.
inline constexpr const char* kEffectTemplate =
    "class NewEffect {\n"
    "  byte bpm = 60;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 255);\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    fill(scale(beat(bpm, t), 256), 0, 100);\n"
    "  }\n"
    "}\n";

inline constexpr const char* kLayoutTemplate =
    "class NewLayout {\n"
    "  byte cols = 16;\n"
    "  byte rows = 16;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"cols\", cols, 1, 64);\n"
    "    addControl(\"rows\", rows, 1, 64);\n"
    "  }\n"
    "\n"
    "  void placeLights() {\n"
    "    for (y = 0; y < rows; y = y + 1) {\n"
    "      for (x = 0; x < cols; x = x + 1) {\n"
    "        addLight(x, y, 0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

inline constexpr const char* kModifierTemplate =
    "class NewModifier {\n"
    "  void modifyLogical() {\n"
    "    setXYZ(width - 1 - xPos, yPos, zPos);\n"
    "  }\n"
    "}\n";

/// A service template: poll a pin on the 50 Hz tick, and write the control surface on a change.
inline constexpr const char* kServiceTemplate =
    "class NewService {\n"
    "  int pin = 0;\n"
    // 1 is the level an idle pull-up reads, so the first tick sees no change that never happened.
    "  int last = 1;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"pin\", pin, 0, 48);\n"
    "  }\n"
    "\n"
    "  void tick20ms() {\n"
    "    int now = gpioRead(pin);\n"
    "    if (now != last) {\n"
    "      last = now;\n"
    // Inverted, since active-low wiring means a pressed button reads 0.
    "      setControl(\"switch1\", 1 - now);\n"
    "    }\n"
    "  }\n"
    "}\n";

/// The template a new palette script starts from.
inline constexpr const char* kPaletteTemplate =
    "class NewPalette {\n"
    "  byte bpm = 20;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 120); // how fast the colors move\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    for (int i = 0; i < 16; i = i + 1) {\n"
    "      setPalEntryHSV(i, scale(beat(bpm, t), 256) + i * 16, 255, 255);\n"
    "    }\n"
    "  }\n"
    "}\n";

// Beside the directory they name, rather than repeated in each binding.
/// What a `script` control tells the UI: the directory, the extension, and the new-file template.
inline constexpr const char* kEffectPick[3]   = {kScriptDir, kEffectExt,   kEffectTemplate};
inline constexpr const char* kLayoutPick[3]   = {kScriptDir, kLayoutExt,   kLayoutTemplate};
inline constexpr const char* kModifierPick[3] = {kScriptDir, kModifierExt, kModifierTemplate};
inline constexpr const char* kServicePick[3]  = {kScriptDir, kServiceExt,  kServiceTemplate};
inline constexpr const char* kPalettePick[3]  = {kScriptDir, kPaletteExt,  kPaletteTemplate};

// One definition, because two copies drifted and the linker picked whichever it liked.
/// Whether `ext` is one of the script extensions.
inline bool isScriptExt(const char* ext) {
    if (!ext) return false;
    return std::strcmp(ext, kEffectExt) == 0 || std::strcmp(ext, kLayoutExt) == 0 ||
           std::strcmp(ext, kModifierExt) == 0 || std::strcmp(ext, kServiceExt) == 0 ||
           std::strcmp(ext, kPaletteExt) == 0;
}

// The catalog is the only list of shipped names, so a second copy would drift.
/// Whether `name` is one of the scripts the firmware ships.
inline bool isFactoryScript(const char* name) {
    if (!name || !*name) return false;
    const char* ext = std::strrchr(name, '.');
    if (!ext) return false;
    const char* const* cat = nullptr;
    size_t n = 0;
    if (std::strcmp(ext, kEffectExt) == 0)        { cat = kEffectCatalog;   n = kEffectCatalogCount; }
    else if (std::strcmp(ext, kLayoutExt) == 0)   { cat = kLayoutCatalog;   n = kLayoutCatalogCount; }
    else if (std::strcmp(ext, kModifierExt) == 0) { cat = kModifierCatalog; n = kModifierCatalogCount; }
    else if (std::strcmp(ext, kServiceExt) == 0)  { cat = kServiceCatalog;  n = kServiceCatalogCount; }
    else if (std::strcmp(ext, kPaletteExt) == 0)  { cat = kPaletteCatalog;  n = kPaletteCatalogCount; }
    else return false;
    for (size_t i = 0; i < n; i++)
        if (std::strcmp(cat[i], name) == 0) return true;
    return false;
}

// A bound rather than a language limit, so a stray large file cannot exhaust a small device.
/// The largest script the loader reads into RAM at once.
inline constexpr long kScriptFileMax = 16384;

// The bindings size their control buffer from this, so an accepted name is always holdable.
/// Longest script name accepted, and the bound on the path buffer below.
inline constexpr size_t kMaxScriptName = 40;

// A caller needing to know whether this changed keeps 4 bytes rather than a copy of the source.
/// FNV-1a over the script text.
inline uint32_t scriptHash(const char* s, size_t len) { return fnv1a(s, len); }

// One resolver, or a fork compiles from one directory and hashes from the other.
/// Where `name` lives: the user's copy when there is one, else the factory copy.
inline bool resolveScript(const char* name, char* out, size_t outLen) {
    std::snprintf(out, outLen, "%s/%s", kScriptDir, name);
    if (platform::fsSize(out) >= 0) return true;
    char factory[96];
    std::snprintf(factory, sizeof(factory), "%s/%s", kFactoryScriptDir, name);
    if (platform::fsSize(factory) < 0) return false;   // neither: leave `out` as the user path
    std::snprintf(out, outLen, "%s", factory);
    return true;
}

// A sidecar, since a provenance line inside user-facing text would be edited away.
/// Where the lineage hash of a forked script is kept.
inline void scriptLineagePath(const char* name, char* out, size_t outLen) {
    std::snprintf(out, outLen, "%s/.%s.from", kScriptDir, name);
}

/// Record that the user's copy of `name` was forked from text hashing to `hash`.
inline bool noteScriptLineage(const char* name, uint32_t hash) {
    if (!name || !name[0]) return false;
    char path[128];
    scriptLineagePath(name, path, sizeof(path));
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(hash));
    return n > 0 && platform::fsWriteAtomic(path, text, static_cast<size_t>(n));
}

// Absent lineage means "cannot say", never "unchanged".
/// The hash a fork was made from, or false when none was recorded.
inline bool scriptLineage(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[128];
    scriptLineagePath(name, path, sizeof(path));
    char text[16] = {};
    const int got = platform::fsRead(path, text, sizeof(text));
    if (got <= 0) return false;
    uint32_t v = 0;
    for (int i = 0; i < got && text[i] >= '0' && text[i] <= '9'; i++)
        v = v * 10u + static_cast<uint32_t>(text[i] - '0');
    out = v;
    return true;
}

// Takes the written path rather than a name, so no caller works out whether a write was a fork.
/// Record lineage for a file as it is written, when it forks a script the firmware also ships.
inline void noteForkedFrom(const char* path) {
    if (!path) return;
    char prefix[64];
    const int plen = std::snprintf(prefix, sizeof(prefix), "%s/", kScriptDir);
    if (plen <= 0 || std::strncmp(path, prefix, static_cast<size_t>(plen)) != 0) return;
    const char* name = path + plen;
    if (!name[0] || std::strchr(name, '/') || name[0] == '.') return;   // nested, or our own sidecar

    // A revert arrives here too, so the lineage drops with the fork it described.
    char user[96];
    std::snprintf(user, sizeof(user), "%s/%s", kScriptDir, name);
    if (platform::fsSize(user) < 0) {
        char side[128];
        scriptLineagePath(name, side, sizeof(side));
        platform::fsRemove(side);
        return;
    }

    // Only on creation: a branch point does not move, and re-stamping would erase what it shows.
    uint32_t already = 0;
    if (scriptLineage(name, already)) return;

    char factory[96];
    std::snprintf(factory, sizeof(factory), "%s/%s", kFactoryScriptDir, name);
    const long size = platform::fsSize(factory);
    if (size <= 0 || size > kScriptFileMax) return;                     // nothing shipped: not a fork
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return;
    const int got = platform::fsRead(factory, text, static_cast<size_t>(size) + 1);
    if (got > 0) noteScriptLineage(name, scriptHash(text, static_cast<size_t>(got)));
    platform::free(text);
}

// False without lineage, since claiming an update on a guess would send someone to discard work.
/// Whether the shipped copy has changed since the user forked it.
inline bool scriptFactoryMovedOn(const char* name) {
    uint32_t from = 0;
    if (!scriptLineage(name, from)) return false;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kFactoryScriptDir, name);
    const long size = platform::fsSize(path);
    if (size <= 0 || size > kScriptFileMax) return false;
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return false;
    const int got = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    const bool moved = got > 0 && scriptHash(text, static_cast<size_t>(got)) != from;
    platform::free(text);
    return moved;
}

inline bool scriptShadowsFactory(const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptDir, name);
    if (platform::fsSize(path) < 0) return false;
    std::snprintf(path, sizeof(path), "%s/%s", kFactoryScriptDir, name);
    return platform::fsSize(path) >= 0;
}

// One read, skipping the parse, the codegen and the exec-block allocation after it.
/// The hash of `name`'s current text, without compiling it.
inline bool scriptFileHash(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[96];
    if (!resolveScript(name, path, sizeof(path))) return false;
    const long size = platform::fsSize(path);
    if (size <= 0 || size > kScriptFileMax) return false;

    // One read: a chunked walk opens and closes per call, which boot-looped a P4.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return false;                          // no memory is "cannot answer", not "unchanged"
    const int got = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    const bool ok = got > 0;
    if (ok) out = scriptHash(text, static_cast<size_t>(got));
    platform::free(text);
    return ok;
}

/// As compileScriptFile, and additionally reports the source's hash.
inline bool compileScriptFile(MoonLive& engine, const char* name,
                              const BuiltinTable& builtins, const SysVarTable& sysvars,
                              const char*& err, uint32_t* hashOut = nullptr) {
    // freeCode first, since a rejected script must not keep executing and the arena must survive.
    engine.freeCode();

    // The write endpoint makes no parent directories, so naming a script is what creates this.
    platform::fsMkdir(kScriptDir);

    if (!name || !name[0]) { err = "no script: set the script name"; return false; }

    // A basename only, rejected rather than sanitized: one needing a rewrite was mistyped.
    for (const char* c = name; *c; c++)
        if (*c == '/' || *c == '\\') { err = "script name is a file in the script folder, not a path"; return false; }
    if (std::strcmp(name, "..") == 0 || std::strncmp(name, "../", 3) == 0) {
        err = "script name is a file in the script folder, not a path"; return false;
    }
    // Any script extension, since the loader is role-blind exactly as the engine is.
    const size_t len = std::strlen(name);
    const char* tail = len >= 4 ? name + len - 4 : "";
    if (len < 5 || len > kMaxScriptName || !isScriptExt(tail)) {
        err = "script name must end in .mle, .mll, .mlm, .mls or .mlp"; return false;
    }

    // The user's copy wins, which is what makes editing a factory script a fork.
    char path[96];
    resolveScript(name, path, sizeof(path));

    const long size = platform::fsSize(path);
    if (size < 0)               { err = "script not found"; return false; }
    if (size == 0)              { err = "script is empty";  return false; }
    if (size > kScriptFileMax)  { err = "script too large"; return false; }

    // +1 for the NUL the lexer reads as End, which fsRead writes on success.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) { err = "no memory for the script"; return false; }

    const int read = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    if (read <= 0) { platform::free(text); err = "script could not be read"; return false; }

    if (hashOut) *hashOut = scriptHash(text, static_cast<size_t>(read));
    const bool ok = engine.compile(text, builtins, sysvars);
    if (!ok) err = engine.error();
    // Freed on both paths, since a failed compile is when a device can least afford to leak.
    platform::free(text);
    return ok;
}

/// @}

}  // namespace mm::moonlive
