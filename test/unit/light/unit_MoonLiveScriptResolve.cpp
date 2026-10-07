/// @module MoonLive
/// @also MoonLiveLayout, MoonLiveEffect, MoonLiveModifier

/// Which FILE a script name means.
///
/// A device keeps factory scripts in `/.moonlive`, downloaded by the UI from the shipped catalog, and the user's own in `/moonlive`.
/// A name can therefore exist in one, the other, or both, and which one wins is what makes editing a factory script a fork rather than a change to it.
/// The user's copy shadows the factory one, and deleting that copy restores the original without needing a network.
///
/// These pin the three cases plus the one that used to be a bug waiting to happen: both readers (the compiler and the change-detector) must resolve to the SAME file, or a fork would be compiled from one and hashed from the other and recompile on every prepare sweep forever.

#include "doctest.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

/// Write `text` to `dir/name`, and remember it so the test can take it away again.
void put(const char* dir, const char* name, const char* text) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    platform::fsMkdir(dir);
    REQUIRE(platform::fsWriteAtomic(path, text, std::strlen(text)));
}

void drop(const char* dir, const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    platform::fsRemove(path);
}

/// A script that compiles and is trivially told apart from another by its control name, so a test can prove WHICH file was read rather than merely that something was.
std::string scriptWith(const char* controlName) {
    return std::string("class R { byte v = 1; void defineControls() { addControl(\"") + controlName +
           "\", v, 0, 9); } void tick() { fill(0, 0, 0); } }";
}

/// An ISOLATED filesystem for one test: its own temp root, torn down after.
///
/// The same pattern unit_FileManagerModule uses, and for the reason it records.
/// Without a root of its own a test writes into whatever the process is pointed at, which under a developer's build is the real device directory.
/// These tests create scripts named for what they check, so they were leaving files in the user's own `/moonlive` and reading whatever happened to be there.
struct Rig {
    char root[256];
    Rig() {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_resolve_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        platform::fsSetRoot(root);
        platform::fsMount();
    }
    // Restore the default root so a later test in the same binary starts from the baseline it expects. noexcept and error_code-only. This runs while the stack unwinds from a failed CHECK, and a throw there would terminate the process and lose the failure being reported.
    ~Rig() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        platform::fsSetRoot("");
    }
};

}  // namespace

// The ordinary case for a script nobody has edited: it lives only in the factory directory, and naming it is enough. Without the fallback every downloaded script would report "script not found".
TEST_CASE("a factory script resolves when the user has no copy of it") {
    const char* name = "resolve-factory.mle";
    Rig rig;
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kFactoryScriptDir) + "/" + name);
}

// THE fork rule. The editor only ever saves to the user directory, so a copy there is the user's edit of a factory script, and it has to win or an edit would appear to do nothing.
TEST_CASE("a user's copy shadows the factory script of the same name") {
    const char* name = "resolve-both.mle";
    Rig rig;
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());
    put(moonlive::kScriptDir, name, scriptWith("mine").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);
}

// Un-editing, and the reason the two directories exist at all: deleting the fork restores the factory script with no network, where a single directory would need it downloaded again.
TEST_CASE("deleting a user's copy restores the factory script") {
    const char* name = "resolve-revert.mle";
    Rig rig;
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());
    put(moonlive::kScriptDir, name, scriptWith("mine").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    REQUIRE(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);

    drop(moonlive::kScriptDir, name);

    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kFactoryScriptDir) + "/" + name);
}

// A name in neither directory is not found, and the path it reports is the USER one. A message naming a place a user would not write to sends them looking in the wrong folder.
TEST_CASE("a script in neither directory is not found") {
    const char* name = "resolve-absent.mle";
    Rig rig;

    char path[96];
    CHECK_FALSE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);
}

// The two readers must agree. compileScriptFile reads the text and scriptFileHash answers "has it changed since I compiled it": resolve them differently and a fork compiles from one file while its hash comes from the other, so it looks changed on every prepare sweep and recompiles forever.
TEST_CASE("the compiler and the change-detector read the same file") {
    const char* name = "resolve-agree.mle";
    Rig rig;
    const std::string factory = scriptWith("factory");
    const std::string mine    = scriptWith("mine");
    put(moonlive::kFactoryScriptDir, name, factory.c_str());
    put(moonlive::kScriptDir, name, mine.c_str());

    uint32_t hash = 0;
    REQUIRE(moonlive::scriptFileHash(name, hash));
    // The hash of the USER's text, not the factory one, since that is the file that will compile.
    CHECK(hash == moonlive::scriptHash(mine.c_str(), mine.size()));
    CHECK(hash != moonlive::scriptHash(factory.c_str(), factory.size()));

    // And once the fork is gone, both follow to the factory copy together.
    drop(moonlive::kScriptDir, name);
    REQUIRE(moonlive::scriptFileHash(name, hash));
    CHECK(hash == moonlive::scriptHash(factory.c_str(), factory.size()));
}

// The fourth question: is a user copy HIDING a shipped one?
// From outside the two cases are identical, and a stale user copy has twice been chased as a compiler bug (a control that never appeared; then an old-syntax copy failing at offsets that matched nothing in the file just written).
// The binding puts the answer in its status, and this is the predicate it asks.
TEST_CASE("a user copy is reported as a shadow only when a shipped copy is under it") {
    Rig rig;   // its own root: these write scripts, and without it that is the real device
    const char* name = "unit-shadow.mle";
    drop(moonlive::kScriptDir, name);
    drop(moonlive::kFactoryScriptDir, name);

    CHECK_FALSE(moonlive::scriptShadowsFactory(name));                 // neither

    put(moonlive::kFactoryScriptDir, name, "class A { void tick() {} }\n");
    CHECK_FALSE(moonlive::scriptShadowsFactory(name));                 // shipped only: no shadow

    put(moonlive::kScriptDir, name, "class A { void tick() {} }\n");
    CHECK(moonlive::scriptShadowsFactory(name));                       // both: the user's hides it

    drop(moonlive::kFactoryScriptDir, name);
    CHECK_FALSE(moonlive::scriptShadowsFactory(name));                 // user's own script: not a shadow
    drop(moonlive::kScriptDir, name);
}

// The fifth question, and the one the shadow marker alone cannot answer: has the SHIPPED copy moved on since the user forked it? Without lineage an edit and a stale leftover look identical forever, which is what let 29 pre-`void tick()` copies sit on a bench board failing every compile.
TEST_CASE("a fork knows whether the shipped script has changed under it") {
    Rig rig;   // its own root, for the same reason
    const char* name = "unit-lineage.mle";
    const char* shipped = "class A { void tick() {} }\n";
    drop(moonlive::kScriptDir, name);
    drop(moonlive::kFactoryScriptDir, name);

    put(moonlive::kFactoryScriptDir, name, shipped);
    put(moonlive::kScriptDir, name, "class A { void tick() { fill(1, 2, 3); } }\n");

    // No lineage recorded (an older fork): "cannot say", never "unchanged". Claiming an update on a guess would send someone to discard work for nothing.
    CHECK_FALSE(moonlive::scriptFactoryMovedOn(name));

    REQUIRE(moonlive::noteScriptLineage(name, moonlive::scriptHash(shipped, std::strlen(shipped))));
    CHECK_FALSE(moonlive::scriptFactoryMovedOn(name));          // forked from exactly this text

    put(moonlive::kFactoryScriptDir, name, "class A { void tick() { fill(9, 9, 9); } }\n");
    CHECK(moonlive::scriptFactoryMovedOn(name));                // the library moved under the fork

    char side[128];
    moonlive::scriptLineagePath(name, side, sizeof(side));
    platform::fsRemove(side);
    drop(moonlive::kScriptDir, name);
    drop(moonlive::kFactoryScriptDir, name);
}

// Lineage is bookkeeping the DEVICE does, on the same hook that reacts to any write or delete, so every writer gets it. The editor, a restored backup, a script pushed by a tool.
TEST_CASE("forking a shipped script records what it was forked from, and reverting forgets it") {
    Rig rig;   // its own root, for the same reason
    const char* name = "unit-fork.mle";
    const char* shipped = "class A { void tick() {} }\n";
    char userPath[96], side[128];
    std::snprintf(userPath, sizeof(userPath), "%s/%s", moonlive::kScriptDir, name);
    moonlive::scriptLineagePath(name, side, sizeof(side));
    drop(moonlive::kScriptDir, name);
    drop(moonlive::kFactoryScriptDir, name);
    platform::fsRemove(side);

    // A write with nothing shipped under it is a user's OWN script, not a fork: no lineage.
    put(moonlive::kScriptDir, name, "class A { void tick() {} }\n");
    moonlive::noteForkedFrom(userPath);
    uint32_t from = 0;
    CHECK_FALSE(moonlive::scriptLineage(name, from));

    // Now the same name ships too, and the next write IS a fork.
    put(moonlive::kFactoryScriptDir, name, shipped);
    put(moonlive::kScriptDir, name, "class A { void tick() { fill(1, 2, 3); } }\n");
    moonlive::noteForkedFrom(userPath);
    REQUIRE(moonlive::scriptLineage(name, from));
    CHECK(from == moonlive::scriptHash(shipped, std::strlen(shipped)));

    // EDITING the fork again must not move the branch point. Re-stamping here would mark the fork up to date with a library version the user never saw, erasing the one fact the record exists to carry.
    put(moonlive::kFactoryScriptDir, name, "class A { void tick() { fill(9, 9, 9); } }\n");
    put(moonlive::kScriptDir, name, "class A { void tick() { fill(4, 5, 6); } }\n");
    moonlive::noteForkedFrom(userPath);
    REQUIRE(moonlive::scriptLineage(name, from));
    CHECK(from == moonlive::scriptHash(shipped, std::strlen(shipped)));   // still the ORIGINAL
    CHECK(moonlive::scriptFactoryMovedOn(name));                          // and the library moved

    // Reverting removes the fork. The hook fires again and the lineage goes with it, so a later fork of this name is not compared against a hash from one that no longer exists.
    platform::fsRemove(userPath);
    moonlive::noteForkedFrom(userPath);
    CHECK_FALSE(moonlive::scriptLineage(name, from));

    drop(moonlive::kFactoryScriptDir, name);
}
