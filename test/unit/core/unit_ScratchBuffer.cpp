/// @module ScratchBuffer

#include "doctest.h"
#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "platform/platform.h"   // setTestMaxAllocBlock: refuse a growth

#include <cstdint>

using namespace mm;

namespace {

// A minimal concrete MoonModule to own scratch buffers in these tests. MoonModule is abstract-in-practice only by convention (it has no pure virtuals), so a bare subclass with a public ScratchBuffer member is all the fixture needs.
struct Owner : MoonModule {
    ScratchBuffer<uint8_t> buf{*this};
};

struct Point { int16_t x; int16_t y; };   // a 4-byte element to exercise sizeof(T) math

} // namespace

// resize(N) allocates N elements, count()/bytes() reflect it, data() is non-null, and the owner's dynamicBytes tracks the buffer's byte count.
TEST_CASE("ScratchBuffer resize allocates and reports bytes to its owner") {
    Owner o;
    CHECK(o.dynamicBytes() == 0);
    CHECK_FALSE(static_cast<bool>(o.buf));

    CHECK(o.buf.resize(256));
    CHECK(o.buf.count() == 256);
    CHECK(o.buf.bytes() == 256);
    CHECK(o.buf.data() != nullptr);
    CHECK(static_cast<bool>(o.buf));
    CHECK(o.dynamicBytes() == 256);   // delta accounting: the buffer told its owner
}

// (re)alloc zero-fills the buffer.
TEST_CASE("ScratchBuffer zero-fills on allocation") {
    Owner o;
    o.buf.resize(64);
    for (size_t i = 0; i < o.buf.count(); i++) CHECK(o.buf[i] == 0);
    o.buf[0] = 0xFF;
    o.buf[63] = 0xAB;
    // Growing reallocates and zero-fills the fresh block.
    o.buf.resize(128);
    for (size_t i = 0; i < o.buf.count(); i++) CHECK(o.buf[i] == 0);
}

// resize(0) frees the buffer and returns the owner's dynamicBytes to zero.
TEST_CASE("ScratchBuffer resize(0) frees and clears the owner's byte total") {
    Owner o;
    o.buf.resize(100);
    CHECK(o.dynamicBytes() == 100);
    CHECK(o.buf.resize(0));
    CHECK(o.buf.count() == 0);
    CHECK(o.buf.data() == nullptr);
    CHECK_FALSE(static_cast<bool>(o.buf));
    CHECK(o.dynamicBytes() == 0);
}

// A shrink adjusts the owner's total by the signed delta, not by an absolute set.
TEST_CASE("ScratchBuffer grow then shrink tracks dynamicBytes by delta") {
    Owner o;
    o.buf.resize(200);
    CHECK(o.dynamicBytes() == 200);
    o.buf.resize(50);
    CHECK(o.buf.bytes() == 50);
    CHECK(o.dynamicBytes() == 50);   // 200 → 50 via delta -150, not a fresh absolute
}

// sizeof(T) drives the byte math for non-uint8 element types.
TEST_CASE("ScratchBuffer sizes by element type") {
    struct PtOwner : MoonModule { ScratchBuffer<Point> pts{*this}; } o;
    CHECK(o.pts.resize(10));
    CHECK(o.pts.count() == 10);
    CHECK(o.pts.bytes() == 10 * sizeof(Point));   // 40 for a 4-byte Point
    CHECK(o.dynamicBytes() == 10 * sizeof(Point));
    o.pts[3] = {7, -9};
    CHECK(o.pts[3].x == 7);
    CHECK(o.pts[3].y == -9);
}

// release() on the owner frees every registered buffer (the disable-without-destroy path applyState() takes), the buffer is emptied and the owner's total returns to zero.
TEST_CASE("ScratchBuffer is freed by the owner's release()") {
    Owner o;
    o.buf.resize(300);
    CHECK(o.dynamicBytes() == 300);
    CHECK(static_cast<bool>(o.buf));

    o.release();   // MoonModule::release() walks the registry and resizes each buffer to 0

    CHECK_FALSE(static_cast<bool>(o.buf));
    CHECK(o.buf.count() == 0);
    CHECK(o.dynamicBytes() == 0);

    // Re-acquire after release works (the disable → re-enable cycle).
    CHECK(o.buf.resize(300));
    CHECK(o.dynamicBytes() == 300);
}

// Multiple buffers on one module each report independently; the owner's total is their sum, and release() frees them all (the StarSky/GameOfLife multi-buffer case).
TEST_CASE("ScratchBuffer multiple buffers on one module sum and release together") {
    struct MultiOwner : MoonModule {
        ScratchBuffer<uint8_t> a{*this};
        ScratchBuffer<uint8_t> b{*this};
        ScratchBuffer<uint8_t> c{*this};
    } o;
    o.a.resize(10);
    o.b.resize(20);
    o.c.resize(30);
    CHECK(o.dynamicBytes() == 60);   // 10 + 20 + 30, each buffer's delta summed

    o.release();
    CHECK(o.dynamicBytes() == 0);
    CHECK_FALSE(static_cast<bool>(o.a));
    CHECK_FALSE(static_cast<bool>(o.b));
    CHECK_FALSE(static_cast<bool>(o.c));
}

// A buffer's destructor frees its heap and deregisters from the still-alive owner, no leak, no dangling list node. (ASAN in the test build is the real guard; this pins the accounting.)
TEST_CASE("ScratchBuffer destructor frees and deregisters") {
    Owner o;
    {
        ScratchBuffer<uint8_t> local{o};
        local.resize(40);
        CHECK(o.dynamicBytes() == 40);
    }   // local destructs here: frees + deregisters + subtracts its bytes
    CHECK(o.dynamicBytes() == 0);
    // The owner's own buffer still works after a sibling deregistered.
    CHECK(o.buf.resize(15));
    CHECK(o.dynamicBytes() == 15);
}

// ensure() grows a list in place: what is held stays, the block doubles, and the owner's total follows.
TEST_CASE("ScratchBuffer ensure keeps the held elements and doubles the block") {
    Owner o;
    CHECK(o.buf.ensure(1));
    CHECK(o.buf.count() == 4);         // the smallest block, so the next appends allocate nothing
    o.buf[0] = 7; o.buf[3] = 9;
    CHECK(o.buf.ensure(4));
    CHECK(o.buf.count() == 4);         // already held: no allocation
    CHECK(o.buf.ensure(5));
    CHECK(o.buf.count() == 8);         // doubled
    CHECK(o.buf[0] == 7);
    CHECK(o.buf[3] == 9);
    CHECK(o.buf[4] == 0);              // the new half is zeroed
    CHECK(o.dynamicBytes() == 8);
}

// A growth the reserve refuses leaves the held block as it was, and the exact size is tried before giving up.
TEST_CASE("ScratchBuffer ensure refused by memory keeps the held block, trying the exact size first") {
    struct CapGuard { ~CapGuard() { platform::setTestMaxAllocBlock(0); } } guard;
    Owner o;
    REQUIRE(o.buf.ensure(8));
    o.buf[7] = 5;
    platform::setTestMaxAllocBlock(12);   // 16 (doubled) does not fit, 12 (exact) does
    CHECK(o.buf.ensure(12));
    CHECK(o.buf.count() == 12);
    CHECK(o.buf[7] == 5);
    platform::setTestMaxAllocBlock(1);    // nothing more fits
    CHECK_FALSE(o.buf.ensure(13));
    CHECK(o.buf.count() == 12);
    CHECK(o.buf[7] == 5);
    CHECK(o.dynamicBytes() == 12);
}
