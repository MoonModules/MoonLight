/// @module JsonSink

/// Pins what happens when a heap-mode sink cannot get the memory it needs.
///
/// Both behaviors here were bugs found on the bench (2026-09-08). A classic ESP32 serving a ~40 KB state document truncated it at a power of two and shipped it as a COMPLETE WebSocket frame: the browser parsed it, threw, and every module card past the cut vanished with nothing logged anywhere. Two causes, one per test below.

#include "doctest.h"
#include "core/util/JsonSink.h"
#include "platform/platform_base.h"   // the target's flags and MM_NONBLOCKING
#include "platform/platform_heap.h"   // setTestAllocRefusedFrom

#include <cstring>
#include <string>

TEST_CASE("a heap sink that runs out of memory says so instead of truncating silently") {
    mm::JsonSink sink;
    sink.append("{\"a\":1");
    CHECK_FALSE(sink.overflowed());          // healthy so far

    // Grow until the allocator refuses, and the append then flags it, since a truncated document looks whole; bounded, so a host whose allocator always agrees finishes with a healthy sink.
    std::string chunk(64 * 1024, 'x');
    for (int i = 0; i < 4096 && !sink.overflowed(); i++) sink.append(chunk.c_str());

    if (sink.overflowed()) {
        CHECK(std::strncmp(sink.data(), "{\"a\":1", 6) == 0);   // what WAS written stays intact
        CHECK(sink.data()[sink.size()] == '\0');                // and is still a valid C string
    } else {
        CHECK(sink.size() > 64 * 1024);   // no refusal on this host: it grew, which is also correct
    }
}

TEST_CASE("a document larger than one doubling still serializes whole") {
    // A doubling holds old and new across the copy, about 3x the size in contiguous memory; the JSON below crosses several doublings and comes out byte for byte complete.
    mm::JsonSink sink;
    std::string expect;
    sink.append("[");
    expect += "[";
    for (int i = 0; i < 4000; i++) {
        char frag[32];
        std::snprintf(frag, sizeof(frag), "%s\"item%04d\"", i ? "," : "", i);
        sink.append(frag);
        expect += frag;
    }
    sink.append("]");
    expect += "]";

    CHECK_FALSE(sink.overflowed());
    REQUIRE(sink.size() == expect.size());
    CHECK(std::memcmp(sink.data(), expect.data(), expect.size()) == 0);
    CHECK(sink.data()[sink.size()] == '\0');   // still a valid C string at the far end
}

TEST_CASE("a growth the allocator refuses leaves the document as it was, flags overflow until clear, and then writes again") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } guard;
    mm::JsonSink sink;
    sink.append("{\"a\":1");
    const size_t before = sink.size();

    mm::platform::setTestAllocRefusedFrom(1024);                  // the first block of 2 KiB is refused, so the next growth fails
    const std::string run(4096, 'x');
    sink.append(run.c_str());
    CHECK(sink.overflowed());
    CHECK(sink.size() == before);                                 // none of the refused run is appended

    mm::platform::setTestAllocRefusedFrom(0);
    sink.append("}");
    CHECK(sink.size() == before);                                 // the flag sticks: a later small append is ignored
    CHECK(sink.overflowed());

    sink.clear();
    CHECK_FALSE(sink.overflowed());
    sink.append("{}");
    CHECK(std::strcmp(sink.data(), "{}") == 0);                   // clear recovers
}

TEST_CASE("a formatted fragment too long for the stack buffer flags overflow when its block is refused") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } guard;
    mm::JsonSink sink;
    sink.append("[");
    mm::platform::setTestAllocRefusedFrom(1);                     // every allocation is refused
    const std::string longArg(4096, 'y');
    sink.appendf("%s", longArg.c_str());
    CHECK(sink.overflowed());
    CHECK(sink.size() == 1);
}

TEST_CASE("a small-buffer sink keeps a short document in the caller's slice and moves a long one into a block whole") {
    char slice[16];
    mm::JsonSink small(slice, sizeof(slice), mm::JsonSink::Spill{});
    small.append("[1,2]");
    CHECK(small.data() == slice);                                  // fits: no allocation
    mm::JsonSink big(slice, sizeof(slice), mm::JsonSink::Spill{});
    big.append("[");
    for (int i = 0; i < 100; i++) big.appendf("%d,", i);
    big.appendf("%s", std::string(300, 'x').c_str());              // a fragment past the stack buffer spills too
    big.append("]");
    CHECK_FALSE(big.overflowed());
    CHECK(big.data() != slice);                                    // moved into its own block
    CHECK(std::strncmp(big.data(), "[0,1,2,", 7) == 0);            // the slice's bytes came along
    CHECK(big.size() == std::strlen(big.data()));
    CHECK(big.data()[big.size() - 1] == ']');
}

TEST_CASE("a small-buffer sink whose block is refused flags overflow") {
    struct AllocGuard { ~AllocGuard() { mm::platform::setTestAllocRefusedFrom(0); } } guard;
    char slice[8];
    mm::JsonSink sink(slice, sizeof(slice), mm::JsonSink::Spill{});
    mm::platform::setTestAllocRefusedFrom(1);                      // every allocation is refused
    sink.append("0123456789");
    CHECK(sink.overflowed());
}
