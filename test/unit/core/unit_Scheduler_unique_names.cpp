/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

// Pins Scheduler::freeName and firstByName. The HTTP API and a state document use module names as identifiers, and firstByName returns the first DFS match, so a second module with the same name could never be addressed.
//
// A name that clashes takes the next free `-N` suffix, which is how a stored config file's module is placed when its name is already used elsewhere in the tree.

namespace {

struct Stub : public mm::MoonModule {
    // No role needed for these tests; default Generic is fine.
};

} // namespace

// A name no module holds is returned unchanged.
TEST_CASE("Scheduler::freeName returns an unused name as it is") {
    mm::Scheduler s;
    auto* a = new Stub();
    a->setName("OnlyOne");
    s.addModule(a);

    char out[mm::MoonModule::kNameLen];
    CHECK(s.freeName("Fresh", out, sizeof(out)));
    CHECK(std::strcmp(out, "Fresh") == 0);

    s.deleteTree(a);
}

// A name a module holds gets "-2"; the holder keeps its name.
TEST_CASE("Scheduler::freeName suffixes a name that is taken") {
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Effects");
    s.addModule(parent);

    auto* first = new Stub();
    first->setName("Layer");
    parent->addChild(first);

    char out[mm::MoonModule::kNameLen];
    CHECK(s.freeName("Layer", out, sizeof(out)));
    CHECK(std::strcmp(out, "Layer-2") == 0);  // '-' separator (URL-safe)
    CHECK(std::strcmp(first->name(), "Layer") == 0);

    s.deleteTree(parent);
}

// Suffix counting increments past existing "-2" / "-3" suffixes ("Layer", "Layer-2" taken gives "Layer-3").
TEST_CASE("Scheduler::freeName keeps counting past 'Foo-2'") {
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Effects");
    s.addModule(parent);

    auto* a = new Stub(); a->setName("Layer");   parent->addChild(a);
    auto* b = new Stub(); b->setName("Layer-2"); parent->addChild(b);

    char out[mm::MoonModule::kNameLen];
    CHECK(s.freeName("Layer", out, sizeof(out)));
    CHECK(std::strcmp(out, "Layer-3") == 0);

    s.deleteTree(parent);
}

// firstByName(name) returns the first match in DFS order, or nullptr if no module carries that name.
TEST_CASE("Scheduler::firstByName returns the first match in tree-walk order") {
    mm::Scheduler s;
    auto* p = new Stub(); p->setName("Effects");      s.addModule(p);
    auto* a = new Stub(); a->setName("Layer");       p->addChild(a);
    auto* b = new Stub(); b->setName("Other");       p->addChild(b);

    CHECK(s.firstByName("Effects") == p);
    CHECK(s.firstByName("Layer")  == a);
    CHECK(s.firstByName("Other")  == b);
    CHECK(s.firstByName("Missing") == nullptr);

    s.deleteTree(p);
}

// If the disambiguating suffix would overflow the 16-byte name buffer, freeName refuses rather than truncate.
TEST_CASE("Scheduler::freeName refuses a suffix that does not fit a name") {
    // MoonModule::name_ is 16 bytes (15 chars + NUL). A 13-char base name like "GlowParticles" reaches the buffer ceiling at suffix "10", "GlowParticles-10" is 16 chars, doesn't fit. freeName must report false rather than silently produce a different result than the caller asked for.
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Effects");
    s.addModule(parent);

    // One base "GlowParticles" plus eight "GlowParticles-2".."GlowParticles-9". The next free name needs "GlowParticles-10" which doesn't fit.
    auto* first = new Stub();
    first->setName("GlowParticles");
    parent->addChild(first);

    for (int n = 2; n <= 9; n++) {
        auto* m = new Stub();
        char nm[16];
        std::snprintf(nm, sizeof(nm), "GlowParticles-%d", n);
        m->setName(nm);
        parent->addChild(m);
    }

    char out[mm::MoonModule::kNameLen] = "untouched";
    CHECK_FALSE(s.freeName("GlowParticles", out, sizeof(out)));
    CHECK(std::strcmp(out, "untouched") == 0);

    s.deleteTree(parent);
}

// A name that cannot be a module name at all is refused.
TEST_CASE("Scheduler::freeName refuses an empty name and one too long for the buffer") {
    mm::Scheduler s;
    char out[mm::MoonModule::kNameLen];
    CHECK_FALSE(s.freeName("", out, sizeof(out)));
    CHECK_FALSE(s.freeName("ThisNameIsFarTooLongForTheBuffer", out, sizeof(out)));
}
