/// @module JsonSink

/// Pins JsonSink::detach(), the move-out that hands the built heap buffer to a caller who keeps it (the resumable WS state-frame send). The ownership contract: after detach the sink's destructor frees nothing (no double-free), the returned block is NUL-terminated and owned by the caller (free with platform::free), and detach is a no-op in socket/fixed mode. Run under ASAN, a leak or double-free here fails the build.

#include "doctest.h"
#include "core/util/JsonSink.h"
#include "platform/platform_heap.h"   // free

#include <cstring>

TEST_CASE("JsonSink::detach hands over the heap buffer; sink frees nothing after") {
    char* buf = nullptr;
    size_t len = 0;
    {
        mm::JsonSink sink;                 // buffer mode
        sink.append("{\"k\":");
        sink.appendf("%d}", 42);
        len = sink.size();
        buf = sink.detach();               // take ownership before the sink destructs
        CHECK(sink.size() == 0);           // sink emptied — its dtor now frees nothing (no double-free)
        CHECK(sink.detach() == nullptr);   // a second detach hands over nothing
    }                                      // sink destructed here: must NOT free buf (ASAN would catch)
    REQUIRE(buf != nullptr);
    CHECK(std::strcmp(buf, "{\"k\":42}") == 0);   // NUL-terminated, complete
    CHECK(len == 8);
    mm::platform::free(buf);               // caller owns it now — free with the same allocator
}

TEST_CASE("JsonSink::detach is a no-op in fixed-buffer mode") {
    char fixed[32];
    mm::JsonSink sink(fixed, sizeof(fixed));
    sink.append("hello");
    CHECK(sink.detach() == nullptr);       // fixed mode owns no heap — nothing to hand over
    CHECK(std::strcmp(fixed, "hello") == 0);   // the caller's buffer is untouched by the failed detach
}

TEST_CASE("JsonSink::detach on an empty buffer-mode sink returns null, not a dangling block") {
    mm::JsonSink sink;                     // nothing appended → no heap allocated yet
    CHECK(sink.detach() == nullptr);
}
