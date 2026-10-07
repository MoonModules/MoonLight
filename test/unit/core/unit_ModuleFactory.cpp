/// @module ModuleFactory

#include "doctest.h"
#include "core/util/ModuleFactory.h"
#include "core/module/MoonModule.h"

namespace {

// Minimal per-role probes to exercise role discovery via the templated registerType<T>. Real domain types are excluded so this test stays a unit test of the factory itself.
class EffectStub : public mm::MoonModule {
public:
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Effect; }
};
class ModifierStub : public mm::MoonModule {
public:
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Modifier; }
};
class DriverStub : public mm::MoonModule {
public:
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Driver; }
};
class LayoutStub : public mm::MoonModule {
public:
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Layout; }
};
class GenericStub : public mm::MoonModule {
    // No override, inherits ModuleRole::Generic.
};

} // namespace

// NOTE: ModuleFactory is a global singleton (static inline state). The doctest harness runs all tests in one process, so once registered, types persist across tests. To avoid cross-test contamination we use unique-name registrations per test case and check role/count properties rather than asserting an empty initial state.

// registerType<T>(name) instantiates a probe of T to read its role(), then stores name+role+constructor for later create() calls.
TEST_CASE("ModuleFactory: registerType captures role via probe instance") {
    const uint8_t before = mm::ModuleFactory::typeCount();
    REQUIRE(mm::ModuleFactory::registerType<EffectStub>("__test_EffectStub"));
    REQUIRE(mm::ModuleFactory::registerType<ModifierStub>("__test_ModifierStub"));
    REQUIRE(mm::ModuleFactory::registerType<DriverStub>("__test_DriverStub"));
    REQUIRE(mm::ModuleFactory::registerType<LayoutStub>("__test_LayoutStub"));
    REQUIRE(mm::ModuleFactory::registerType<GenericStub>("__test_GenericStub"));
    CHECK(mm::ModuleFactory::typeCount() == before + 5);

    // Walk the registry; for each of the 5 names we just added, verify the role.
    struct Expect { const char* name; mm::ModuleRole role; };
    Expect expected[] = {
        {"__test_EffectStub",   mm::ModuleRole::Effect},
        {"__test_ModifierStub", mm::ModuleRole::Modifier},
        {"__test_DriverStub",   mm::ModuleRole::Driver},
        {"__test_LayoutStub",   mm::ModuleRole::Layout},
        {"__test_GenericStub",  mm::ModuleRole::Generic},
    };
    int matched = 0;
    for (uint8_t i = 0; i < mm::ModuleFactory::typeCount(); i++) {
        const char* n = mm::ModuleFactory::typeName(i);
        if (!n) continue;
        for (const auto& e : expected) {
            if (std::strcmp(n, e.name) == 0) {
                CHECK(mm::ModuleFactory::typeRole(i) == e.role);
                matched++;
            }
        }
    }
    CHECK(matched == 5);
}

// create(name) returns a heap-allocated instance whose role and typeName match what was registered.
TEST_CASE("ModuleFactory: create returns a module of the registered type") {
    REQUIRE(mm::ModuleFactory::registerType<EffectStub>("__test_create_target"));
    auto* mod = mm::ModuleFactory::create("__test_create_target");
    REQUIRE(mod != nullptr);
    CHECK(mod->role() == mm::ModuleRole::Effect);
    CHECK(std::strcmp(mod->typeName(), "__test_create_target") == 0);
    delete mod;
}

// create() returns nullptr for an unknown name or a nullptr name (no crash on bogus input).
TEST_CASE("ModuleFactory: create returns nullptr for unknown type") {
    CHECK(mm::ModuleFactory::create("__no_such_type_registered__") == nullptr);
    CHECK(mm::ModuleFactory::create(nullptr) == nullptr);
}

// typeName/typeRole with an out-of-range index returns nullptr / Generic safely (never UB).
TEST_CASE("ModuleFactory: typeName / typeRole bounds-check") {
    const uint8_t count = mm::ModuleFactory::typeCount();
    // Out-of-range access returns null / Generic, not UB.
    CHECK(mm::ModuleFactory::typeName(count) == nullptr);
    CHECK(mm::ModuleFactory::typeName(255) == nullptr);
    CHECK(mm::ModuleFactory::typeRole(count) == mm::ModuleRole::Generic);
}

// The factory grows its registry capacity dynamically, registering 10+ extra types past the initial size still works and every name stays discoverable.
TEST_CASE("ModuleFactory: dynamic capacity grows past initial size") {
    // Register enough types to force at least one capacity-doubling step beyond the initial 4 → 8 → 16 progression. We register 10 throwaway names with the Generic role to confirm they all land and remain accessible.
    //
    // The factory stores name pointers without copying, so the name buffer MUST have process lifetime, `static` makes the storage outlive the test. Stack-local names would dangle the moment this TEST_CASE returned, and subsequent tests walking typeName(i) would read freed memory.
    static char nameBuf[10][16];
    const uint8_t before = mm::ModuleFactory::typeCount();
    for (int i = 0; i < 10; i++) {
        std::snprintf(nameBuf[i], sizeof(nameBuf[i]), "__test_grow_%d", i);
        REQUIRE(mm::ModuleFactory::registerType<GenericStub>(nameBuf[i]));
    }
    CHECK(mm::ModuleFactory::typeCount() == before + 10);
    // Every new name remains discoverable, and the created probes are owned by us.
    for (int i = 0; i < 10; i++) {
        auto* m = mm::ModuleFactory::create(nameBuf[i]);
        CHECK(m != nullptr);
        delete m;
    }
}

namespace {
// Counts the instances built through operator new, so a test sees where the probe lived.
class HeapCountedStub : public mm::MoonModule {
public:
    static inline int heapBuilt = 0;
    void* operator new(size_t size) noexcept { heapBuilt++; return mm::MoonModule::operator new(size); }
    uint8_t big[8192] = {};   ///< larger than a classic ESP32's boot-task stack can spare
};
}  // namespace

// Regression: the probe was a stack local, so registering a large module boot-looped the P4 at registration.
TEST_CASE("ModuleFactory: the registration probe lives on the heap, however large the module") {
    HeapCountedStub::heapBuilt = 0;
    CHECK(mm::ModuleFactory::registerType<HeapCountedStub>("HeapCountedStub"));
    CHECK(HeapCountedStub::heapBuilt == 1);
}
