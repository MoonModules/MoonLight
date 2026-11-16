/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

#include <cstdio>
#include <cstring>
#include <string>

// Module names are identifiers the API and documents address, so a clash takes the next free `-N` suffix, the base shortened to fit.

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

// A name holds 15 characters, so a base whose suffix would not fit gives up its last characters to it, and the type can still be added again.
TEST_CASE("Scheduler::freeName shortens the base so its suffix fits a name") {
    // "GlowParticles" is 13 characters, so "GlowParticles-10" would be 16: the base drops one character for the longer suffix.
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

    char out[mm::MoonModule::kNameLen];
    REQUIRE(s.freeName("GlowParticles", out, sizeof(out)));
    CHECK(std::string(out) == "GlowParticle-10");

    s.deleteTree(parent);
}

// The defect a user met: "NetworkReceive" is 14 characters, so a second receiver had no name that fit and the add was refused.
TEST_CASE("Scheduler::freeName names a second module of a type whose name leaves no room for '-2'") {
    mm::Scheduler s;
    auto* parent = new Stub();
    parent->setName("Layer");
    s.addModule(parent);
    auto* first = new Stub();
    first->setName("NetworkReceive");
    parent->addChild(first);

    char out[mm::MoonModule::kNameLen];
    REQUIRE(s.freeName("NetworkReceive", out, sizeof(out)));
    CHECK(std::string(out) == "NetworkReceiv-2");

    s.deleteTree(parent);
}

// An empty base names nothing; a base longer than a name keeps its first 15 characters, so a type with a long name is added at all.
TEST_CASE("Scheduler::freeName refuses an empty name and shortens one too long for a name") {
    mm::Scheduler s;
    char out[mm::MoonModule::kNameLen];
    CHECK_FALSE(s.freeName("", out, sizeof(out)));
    REQUIRE(s.freeName("ImprovProvisioning", out, sizeof(out)));
    CHECK(std::string(out) == "ImprovProvision");
}

// A name holds bytes, and a character can take four: the cut lands before a character, never inside one, so the name stays valid UTF-8 for the JSON and the URL that carry it.
TEST_CASE("Scheduler::freeName shortens a name between characters, never inside one") {
    mm::Scheduler s;
    char out[mm::MoonModule::kNameLen];
    REQUIRE(s.freeName("Licht\xF0\x9F\x8C\x99\xF0\x9F\x8C\x99\xF0\x9F\x8C\x99", out, sizeof(out)));   // "Licht" and three moons, 17 bytes
    CHECK(std::string(out) == "Licht\xF0\x9F\x8C\x99\xF0\x9F\x8C\x99");   // 13 bytes: the third moon does not fit whole, so it goes whole
}
