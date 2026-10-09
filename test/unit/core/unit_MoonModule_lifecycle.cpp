/// Pins the MoonModule base-default propagation for loop / tick20ms / tick1s.
/// The three tick callbacks default to iterating children, gating by `!respectsEnabled() || enabled()`, dispatching the same callback on each child, and accumulating per-child timing.
/// The tests pin the gating and dispatch rules, so a change to MoonModule::tickChildren fails here instead of silently breaking a container subtree.

/// @module MoonModule

#include "doctest.h"
#include "core/module/MoonModule.h"

namespace {

// Test module that counts each tick callback. Default ctor; no extra behavior beyond accounting. Used for both parents and children below.
class Counting : public mm::MoonModule {
public:
    uint32_t loopCalls = 0;
    uint32_t tick20msCalls = 0;
    uint32_t tick1sCalls = 0;

    void tick() MM_NONBLOCKING override {
        loopCalls++;
        mm::MoonModule::tick();   // chain so this counter doubles as a parent too
    }
    void tick20ms() MM_NONBLOCKING override {
        tick20msCalls++;
        mm::MoonModule::tick20ms();
    }
    void tick1s() MM_NONBLOCKING override {
        tick1sCalls++;
        mm::MoonModule::tick1s();
    }
};

// Variant that opts out of the enabled gate, same behavior as NetworkModule / SystemModule / FirmwareUpdateModule today.
class AlwaysOn : public Counting {
public:
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }
};

} // namespace

// A parent's default tick() fans out to every enabled child, no per-container boilerplate needed.
TEST_CASE("tick default propagates to enabled children") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);

    parent.tick();

    CHECK(parent.loopCalls == 1);
    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 1);
}

// Disabled children are skipped during propagation (the universal enable-gate).
TEST_CASE("tick default skips disabled children") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);
    b.setEnabled(false);

    parent.tick();

    CHECK(a.loopCalls == 1);
    CHECK(b.loopCalls == 0);
}

// Modules that override respectsEnabled() to false (NetworkModule, SystemModule, …) tick regardless of their enable bit.
TEST_CASE("tick default still ticks children that opt out of the enabled gate") {
    Counting parent;
    AlwaysOn a;
    parent.addChild(&a);
    a.setEnabled(false);   // would normally skip, but respectsEnabled() == false

    parent.tick();

    CHECK(a.loopCalls == 1);
}

// tick20ms / tick1s use the same gate-and-propagate rule as tick().
TEST_CASE("tick20ms and tick1s follow the same gating + dispatch rules") {
    Counting parent;
    Counting a, b;
    parent.addChild(&a);
    parent.addChild(&b);
    b.setEnabled(false);

    parent.tick20ms();
    parent.tick1s();

    CHECK(a.tick20msCalls == 1);
    CHECK(a.tick1sCalls == 1);
    CHECK(b.tick20msCalls == 0);
    CHECK(b.tick1sCalls == 0);
}

// A leaf module (no children) ticks safely as a no-op with no accumulated timing.
TEST_CASE("leaf module tick default is a safe no-op (childCount_ == 0)") {
    // Direct MoonModule (no override): default loop / tick20ms / tick1s should iterate over zero children and return without side effects.
    mm::MoonModule leaf;
    leaf.tick();
    leaf.tick20ms();
    leaf.tick1s();

    CHECK(leaf.childCount() == 0);
    CHECK(leaf.tickTimeUs() == 0);  // no timing accumulated either
}

// Each child's tickTimeUs() reflects its own accumulated cost (Scheduler reads per-child timing, not the parent's sum).
TEST_CASE("per-child timing accumulates on the child, not the parent") {
    // Inject a known accumulation via addAccumUs(), since two adjacent platform::micros() reads on a fast desktop can round to 0 and make the assertion tautological.
    Counting parent;
    Counting a;
    parent.addChild(&a);

    // Tick once via the parent to exercise the propagation path, then add a deterministic contribution for a known per-frame average.
    parent.tick();
    a.addAccumUs(40);   // 40us across 2 frames = average 20us/frame

    parent.publishTiming(2);

    CHECK(a.loopCalls == 1);
    // 40us across 2 frames bounds the averaged tickTimeUs() at 20 or more; a broken publishTiming would drop to 0.
    CHECK(a.tickTimeUs() >= 20u);
}

// Two crashes or brownouts in a row hold a module that can crash the device or overload its supply: it counts as disabled, and its saved flag stays as set.
TEST_CASE("two crashes in a row hold a module that asks for it and keep its saved flag, one crash does not") {
    struct Risky : Counting { bool heldInSafeMode() const override { return true; } };
    struct Record { ~Record() { mm::platform::setTestBootRecord({}); } } guard;
    {
        mm::platform::setTestBootRecord({0, 1});
        Counting parent;
        Risky risky;
        parent.addChild(&risky);
        CHECK(risky.enabled());
        parent.removeChild(&risky);
    }
    mm::platform::setTestBootRecord({0, 2});
    Counting parent;
    Risky risky;
    Counting plain;
    parent.addChild(&risky);
    parent.addChild(&plain);
    CHECK(mm::MoonModule::safeMode());
    CHECK_FALSE(risky.enabled());
    CHECK(risky.enabledSetting());
    CHECK(risky.held());
    CHECK_FALSE(plain.held());
    parent.tick();
    CHECK(risky.loopCalls == 0);
    CHECK(plain.loopCalls == 1);
    parent.removeChild(&plain);
    parent.removeChild(&risky);
}
