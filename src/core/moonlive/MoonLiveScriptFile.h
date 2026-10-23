#pragma once

#include "core/module/MoonModule.h"   // safeMode: no script runs in it
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveMissing.h"   // what a compile finds missing
#include "core/moonlive/script_catalog.h"
#include "core/util/fnv.h"   // the shipped names, for isFactoryScript below
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

/// Where scripts live and how a file name states its role.
///
/// One language, five extensions: the engine stays role-blind and the extension alone decides which picker offers a file.
///
/// @technical
///
/// ## Two directories
///
/// A user directory the UI writes and the loader prefers, and a factory directory the picker offers and the UI downloads from on first use. The split is the revert mechanism: un-editing is a local delete rather than a download.

namespace mm::moonlive {

/// Where a user's scripts live, which the UI writes and the loader prefers; a module stores a name, not a path, so it cannot reach outside this folder.
inline constexpr const char* kScriptDir = "/moonlive";

/// Where the factory scripts land, which the picker offers and the UI downloads on first use; the split is the revert mechanism: @xref{two-directories}.
inline constexpr const char* kFactoryScriptDir = "/.moonlive";

/// A script's role, carried in its file name: one language, five extensions, and the extension alone decides which picker offers a file.
inline constexpr const char* kEffectExt   = ".mle";
inline constexpr const char* kLayoutExt   = ".mll";
inline constexpr const char* kModifierExt = ".mlm";
inline constexpr const char* kServiceExt  = ".mls";
inline constexpr const char* kPaletteExt  = ".mlp";

/// Whether `ext` is one of the script extensions, defined once for every caller.
inline bool isScriptExt(const char* ext) {
    if (!ext) return false;
    return std::strcmp(ext, kEffectExt) == 0 || std::strcmp(ext, kLayoutExt) == 0 ||
           std::strcmp(ext, kModifierExt) == 0 || std::strcmp(ext, kServiceExt) == 0 ||
           std::strcmp(ext, kPaletteExt) == 0;
}

/// Whether `name` is one of the scripts the firmware ships, read from the catalog, the only list of shipped names.
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

/// The largest script the loader reads into RAM at once: a bound so a stray large file cannot exhaust a small device.
inline constexpr long kScriptFileMax = 16384;

/// Longest script name accepted, and the bound on the path buffer below; the bindings size their control buffer from it.
inline constexpr size_t kMaxScriptName = 40;

/// FNV-1a over the script text, so a caller detecting a change keeps 4 bytes rather than a copy of the source.
inline uint32_t scriptHash(const char* s, size_t len) { return fnv1a(s, len); }

/// Whether `path` is in `dir`, is `dir`, or holds it.
inline bool pathTouchesFolder(const char* path, const char* dir) {
    const size_t p = std::strlen(path), d = std::strlen(dir);
    if (p >= d) return std::strncmp(path, dir, d) == 0 && (path[d] == '/' || path[d] == 0);
    return std::strncmp(dir, path, p) == 0 && (dir[p] == '/' || path[p - 1] == '/');
}

/// Whether a changed `path` may have changed a script: one in, or above, either script folder.
inline bool touchesScripts(const char* path) {
    return path && (pathTouchesFolder(path, kScriptDir) || pathTouchesFolder(path, kFactoryScriptDir));
}

/// Where `name` lives: the user's copy when there is one, else the factory copy; one resolver, so a fork never compiles from one directory and hashes from the other.
inline bool resolveScript(const char* name, char* out, size_t outLen) {
    std::snprintf(out, outLen, "%s/%s", kScriptDir, name);
    if (platform::fsSize(out) >= 0) return true;
    char factory[96];
    std::snprintf(factory, sizeof(factory), "%s/%s", kFactoryScriptDir, name);
    if (platform::fsSize(factory) < 0) return false;   // neither: leave `out` as the user path
    std::snprintf(out, outLen, "%s", factory);
    return true;
}

/// Where the lineage hash of a forked script is kept, in a sidecar since a provenance line inside user-facing text would be edited away.
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

/// The hash a fork was made from, or false when none was recorded, which means "cannot say", never "unchanged".
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

/// Record lineage for a file as it is written, when it forks a script the firmware also ships. It takes the written path so no caller works out whether a write was a fork.
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
    const int64_t size = platform::fsSize(factory);
    if (size <= 0 || size > kScriptFileMax) return;                     // nothing shipped: not a fork
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return;
    const int got = platform::fsRead(factory, text, static_cast<size_t>(size) + 1);
    if (got > 0) noteScriptLineage(name, scriptHash(text, static_cast<size_t>(got)));
    platform::free(text);
}

/// Whether the shipped copy has changed since the user forked it; false without lineage, since claiming an update on a guess would send someone to discard work.
inline bool scriptFactoryMovedOn(const char* name) {
    uint32_t from = 0;
    if (!scriptLineage(name, from)) return false;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kFactoryScriptDir, name);
    const int64_t size = platform::fsSize(path);
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

/// The hash of `name`'s current text, without compiling it: one read, skipping the parse, the codegen and the exec-block allocation.
inline bool scriptFileHash(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[96];
    if (!resolveScript(name, path, sizeof(path))) return false;
    const int64_t size = platform::fsSize(path);
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
    // A script is what a crash loop most often comes from, so safe mode runs none.
    if (MoonModule::safeMode()) { err = "not run in safe mode"; return false; }

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

    const int64_t size = platform::fsSize(path);
    // A shipped script the device lacks is listed for an open page to download, the picker's own path.
    if (size < 0 && isFactoryScript(name)) { noteScriptMissing(name); err = "script missing: an open page downloads it"; return false; }
    if (size < 0)               { err = "script not found"; return false; }
    noteScriptPresent(name);
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

}  // namespace mm::moonlive
