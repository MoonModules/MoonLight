/// @module ImprovOpReassembler

/// Unit tests for src/core/ImprovOpReassembler.h, the chunk-reassembly + sequence guard behind the device's APPLY_OP (0xFC) handler ("Improv = REST over serial").
/// The ESP32 handler (platform_esp32_improv.cpp::improvHandleApplyOp) owns the serial I/O and hands each [seq][last][bytes] chunk here; isolating the state machine lets us prove every path, in-order multi-chunk, duplicate, out-of-order, overflow, recovery, without an MCU + serial cable. This is the heart of config-push: a bug here silently misconfigures a freshly-flashed device.

#include "doctest.h"
#include "core/util/ImprovOpReassembler.h"

#include <atomic>
#include <cstring>
#include <string>

using namespace mm;
using R = ImprovOpReassembler::Result;

// Feed a chunk from a string; chunk index + last flag explicit so a test reads like the wire frames it models.
static R feedStr(ImprovOpReassembler& r, uint8_t seq, bool last, const std::string& s) {
    return r.feed(seq, last, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

TEST_CASE("a single-frame op (seq 0, last 1) is Ready with the exact bytes") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    const std::string op = R"({"op":"set","module":"Grid","control":"width","value":8})";
    CHECK(feedStr(r, 0, true, op) == R::Ready);
    CHECK(r.len() == op.size());
    CHECK(std::string(r.out()) == op);   // NUL-terminated, byte-identical
}

TEST_CASE("a multi-chunk op reassembles in order and NUL-terminates") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "{\"op\":\"set\",") == R::Continue);
    CHECK(feedStr(r, 1, false, "\"module\":\"X\",") == R::Continue);
    CHECK(feedStr(r, 2, true,  "\"value\":1}") == R::Ready);
    CHECK(std::string(r.out()) == "{\"op\":\"set\",\"module\":\"X\",\"value\":1}");
}

TEST_CASE("a duplicate chunk is rejected and resets the buffer") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "AAA") == R::Continue);
    CHECK(feedStr(r, 1, false, "BBB") == R::Continue);
    // The installer re-sends seq 1 (a misread-timeout retry): out of sequence → Error.
    CHECK(feedStr(r, 1, false, "BBB") == R::Error);
    // Buffer reset: a stale partial can't leak into the next op. A fresh op (seq 0) recovers cleanly.
    CHECK(feedStr(r, 0, true, "{\"ok\":1}") == R::Ready);
    CHECK(std::string(r.out()) == "{\"ok\":1}");
}

TEST_CASE("an out-of-order chunk (skipped seq) is rejected") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "AAA") == R::Continue);
    // seq jumps 0 -> 2 (seq 1 lost): the guard rejects rather than splice a hole.
    CHECK(feedStr(r, 2, true, "CCC") == R::Error);
}

TEST_CASE("a non-zero opening seq (no fresh start) is rejected") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    // First chunk seen is seq 1 (we missed seq 0): only seq 0 may start an op.
    CHECK(feedStr(r, 1, true, "X") == R::Error);
    // seq 0 then starts cleanly.
    CHECK(feedStr(r, 0, true, "Y") == R::Ready);
    CHECK(std::string(r.out()) == "Y");
}

TEST_CASE("overflow past the buffer (minus the NUL) is rejected, not truncated") {
    char buf[8];   // 7 usable bytes + 1 reserved for NUL
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "ABCD") == R::Continue);   // 4 bytes
    // 4 + 4 = 8 >= cap(8): would leave no room for the NUL → Error + reset.
    CHECK(feedStr(r, 1, true, "EFGH") == R::Error);
    // A small op fits and works after the overflow reset.
    CHECK(feedStr(r, 0, true, "{}") == R::Ready);
    CHECK(std::string(r.out()) == "{}");
}

TEST_CASE("exactly buffer-minus-one bytes fits (boundary)") {
    char buf[8];   // 7 usable
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, true, "1234567") == R::Ready);   // 7 bytes + NUL = 8, exactly fits
    CHECK(r.len() == 7);
    CHECK(std::string(r.out()) == "1234567");
}

TEST_CASE("seq 0 mid-stream abandons a partial op and starts fresh") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "partial-") == R::Continue);
    // A new op begins (seq 0) before the previous finished, the old partial is dropped, not concatenated. (Models the installer moving to the next op after an error.)
    CHECK(feedStr(r, 0, true, "{\"fresh\":1}") == R::Ready);
    CHECK(std::string(r.out()) == "{\"fresh\":1}");
}

TEST_CASE("an empty final chunk still completes (last with zero bytes)") {
    char buf[16];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "{}") == R::Continue);
    CHECK(r.feed(1, true, nullptr, 0) == R::Ready);   // trailing empty last frame
    CHECK(std::string(r.out()) == "{}");
}

TEST_CASE("reset() drops a partial op") {
    char buf[16];
    ImprovOpReassembler r(buf, sizeof(buf));
    CHECK(feedStr(r, 0, false, "abc") == R::Continue);
    r.reset();
    // After reset, a chunk with seq 1 is out of order (we're back to awaiting seq 0).
    CHECK(feedStr(r, 1, true, "x") == R::Error);
    CHECK(feedStr(r, 0, true, "ok") == R::Ready);
}

// One APPLY_OP payload as the wire carries it: [0xFC][seq][last][chunk bytes].
static ImprovApplyOpReply applyStr(ImprovOpReassembler& r, std::atomic<bool>& ready, uint8_t seq, uint8_t last, const std::string& s) {
    std::string p;
    p += static_cast<char>(kImprovCmdApplyOp);
    p += static_cast<char>(seq);
    p += static_cast<char>(last);
    p += s;
    return improvApplyOp(r, reinterpret_cast<const uint8_t*>(p.data()), p.size(), ready);
}

TEST_CASE("an APPLY_OP document is acknowledged chunk by chunk and marked ready on its last chunk, on every transport alike") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    std::atomic<bool> ready{false};
    CHECK(applyStr(r, ready, 0, 0, "{\"System\":") == ImprovApplyOpReply::Ack);
    CHECK_FALSE(ready.load());
    CHECK(applyStr(r, ready, 1, 1, "{}}") == ImprovApplyOpReply::Ack);
    CHECK(ready.load());
    CHECK(std::string(r.out()) == "{\"System\":{}}");
}

TEST_CASE("an APPLY_OP chunk is refused while the previous document waits, or when malformed, so nothing overwrites an unapplied document") {
    char buf[128];
    ImprovOpReassembler r(buf, sizeof(buf));
    std::atomic<bool> ready{true};
    CHECK(applyStr(r, ready, 0, 1, "{}") == ImprovApplyOpReply::Refused);   // busy
    ready = false;
    CHECK(applyStr(r, ready, 0, 2, "{}") == ImprovApplyOpReply::Refused);   // `last` is 0 or 1
    const uint8_t shortFrame[] = {kImprovCmdApplyOp, 0};
    CHECK(improvApplyOp(r, shortFrame, sizeof(shortFrame), ready) == ImprovApplyOpReply::Refused);
    CHECK(applyStr(r, ready, 3, 1, "{}") == ImprovApplyOpReply::Refused);   // out of order
    CHECK_FALSE(ready.load());
}
