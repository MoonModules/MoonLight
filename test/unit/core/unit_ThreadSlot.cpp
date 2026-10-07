/// @module ThreadSlot

#include "doctest.h"
#include "core/util/ThreadSlot.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace {
struct TestSlot { std::atomic<uintptr_t> owner{0}; int value = 0; };
}  // namespace

TEST_CASE("a thread finds the slot it claimed, and a read alone claims none") {
    TestSlot slots[2]{};
    CHECK(mm::ownedThreadSlot(slots, false) == nullptr);
    TestSlot* mine = mm::ownedThreadSlot(slots, true);
    REQUIRE(mine != nullptr);
    CHECK(mm::ownedThreadSlot(slots, false) == mine);
    CHECK(mm::ownedThreadSlot(slots, true) == mine);   // claiming again is the same slot
    mm::releaseThreadSlot(mine);
    CHECK(mm::ownedThreadSlot(slots, false) == nullptr);
}

TEST_CASE("two threads hold two slots, and a third finds the table full") {
    TestSlot slots[2]{};
    TestSlot* mine = mm::ownedThreadSlot(slots, true);
    TestSlot* theirs = nullptr;
    TestSlot* third = &slots[0];   // overwritten with what the third thread gets
    // The second thread holds its slot while the third claims, since an ended thread's id is reused.
    std::atomic<bool> claimed{false}, done{false};
    std::thread second([&] {
        theirs = mm::ownedThreadSlot(slots, true);
        claimed = true;
        while (!done) std::this_thread::yield();
    });
    while (!claimed) std::this_thread::yield();
    std::thread([&] { third = mm::ownedThreadSlot(slots, true); }).join();
    done = true;
    second.join();
    REQUIRE(mine != nullptr);
    REQUIRE(theirs != nullptr);
    CHECK(mine != theirs);
    CHECK(third == nullptr);
    mm::releaseThreadSlot(theirs);
    std::thread([&] { third = mm::ownedThreadSlot(slots, true); }).join();
    CHECK(third == theirs);    // a released slot is free for the next claimer
}
