/// @module Buffer

#include "doctest.h"
#include "light/layers/Buffer.h"

// allocate(N,3) reserves count×channels bytes; count/channelsPerLight/bytes/data/span all reflect that.
TEST_CASE("Buffer allocate and accessors") {
    mm::Buffer buf;
    CHECK(buf.allocate(256, 3));
    CHECK(buf.count() == 256);
    CHECK(buf.channelsPerLight() == 3);
    CHECK(buf.bytes() == 768);
    CHECK(buf.data() != nullptr);
    CHECK(buf.span().size() == 768);
}

// clear() zeroes every byte in the allocated range.
TEST_CASE("Buffer clear zeros data") {
    mm::Buffer buf;
    REQUIRE(buf.allocate(10, 3));
    // Write some data
    buf.data()[0] = 0xFF;
    buf.data()[15] = 0xAB;
    buf.clear();
    for (size_t i = 0; i < buf.bytes(); i++) {
        CHECK(buf.data()[i] == 0);
    }
}

// Move-constructing transfers the data pointer and resets the source (no double free, no copy).
TEST_CASE("Buffer move constructor") {
    mm::Buffer a;
    a.allocate(100, 3);
    auto* ptr = a.data();

    mm::Buffer b(std::move(a));
    CHECK(b.data() == ptr);
    CHECK(b.count() == 100);
    CHECK(b.channelsPerLight() == 3);
    // Asserting the moved-from state IS the test: Buffer's move must leave the source empty, not merely valid-but-unspecified. NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(a.data() == nullptr);
    CHECK(a.count() == 0);
}

// Move-assigning transfers ownership the same way the move constructor does.
TEST_CASE("Buffer move assignment") {
    mm::Buffer a;
    a.allocate(100, 3);
    auto* ptr = a.data();

    mm::Buffer b;
    b = std::move(a);
    CHECK(b.data() == ptr);
    CHECK(b.count() == 100);
    // Asserting the moved-from state IS the test: Buffer's move must leave the source empty, not merely valid-but-unspecified. NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(a.data() == nullptr);
}

// Calling free() twice is harmless; pointer and count remain zeroed.
TEST_CASE("Buffer double free is safe") {
    mm::Buffer buf;
    buf.allocate(10, 3);
    buf.free();
    buf.free(); // should not crash
    CHECK(buf.data() == nullptr);
    CHECK(buf.count() == 0);
}

// allocate() refuses zero-count or zero-channels (returns false, no allocation, buffer left empty so a caller that ignores the bool doesn't get a partial state).
TEST_CASE("Buffer allocate with zero returns false") {
    mm::Buffer buf;
    CHECK_FALSE(buf.allocate(0, 3));
    CHECK(buf.data() == nullptr);
    CHECK(buf.count() == 0);
    CHECK(buf.channelsPerLight() == 0);

    CHECK_FALSE(buf.allocate(10, 0));
    CHECK(buf.data() == nullptr);
    CHECK(buf.count() == 0);
    CHECK(buf.channelsPerLight() == 0);
}
