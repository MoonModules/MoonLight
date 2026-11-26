/// What the system has deliberately allocated, so a change in memory cost is visible on a laptop.
///
/// The figure exists for its DELTA: every buffer taken on purpose goes through platform::alloc, so adding or removing a module moves it by exactly that module's cost. The process's own RSS cannot answer that question, because the allocator, the JIT and the HTTP buffers move it too.

#include "doctest.h"

#include "platform/platform_heap.h"   // alloc, allocatedBytes, free

#include <cstdint>
#include <vector>

TEST_CASE("an allocation is visible in the reported total, and freeing it gives the bytes back") {
    const size_t before = mm::platform::allocatedBytes();
    void* p = mm::platform::alloc(4096);
    REQUIRE(p != nullptr);
    // Exactly what was ASKED for, not what the allocator rounded it to: a caller comparing two builds needs the difference to be the number it wrote, not a bucket size.
    CHECK(mm::platform::allocatedBytes() == before + 4096);
    mm::platform::free(p);
    CHECK(mm::platform::allocatedBytes() == before);
}

TEST_CASE("the peak survives the free that follows it") {
    void* p = mm::platform::alloc(64 * 1024);
    REQUIRE(p != nullptr);
    const size_t peak = mm::platform::allocatedPeak();
    // At least what is live right now, and never less: the peak is a high-water mark over the whole process, so it may already sit above this allocation from an earlier one.
    CHECK(peak >= mm::platform::allocatedBytes());
    mm::platform::free(p);
    // Still there: a high-water mark answers "how close did this come to the limit", which a current-total cannot, since the worst moment is usually over by the time anyone looks.
    CHECK(mm::platform::allocatedPeak() == peak);
}

TEST_CASE("the live-block count separates one big buffer from many small ones") {
    // Two shapes that cost the same bytes and mean different things: a module that took one buffer, and something allocating per frame.
    const uint32_t before = mm::platform::allocatedCount();
    std::vector<void*> many;
    for (int i = 0; i < 50; i++) many.push_back(mm::platform::alloc(64));
    CHECK(mm::platform::allocatedCount() == before + 50);
    for (void* p : many) mm::platform::free(p);
    // BACK DOWN AFTER FREEING, which is what makes it a live count rather than a tally of allocations ever made. The first version only incremented, so it climbed forever and read as a leak on any device left running: 35 live blocks reported as 10834.
    CHECK(mm::platform::allocatedCount() == before);
}

TEST_CASE("a freed null is not counted") {
    const size_t before = mm::platform::allocatedBytes();
    mm::platform::free(nullptr);
    CHECK(mm::platform::allocatedBytes() == before);
}
