/// @module SpscRing

/// The lock-free single-producer single-consumer ring behind host audio capture: the miniaudio callback thread pushes samples, AudioService's polled tick pops them. These tests pin the contract a consumer can rely on: strict FIFO order across index wrap, drop-newest on overflow with an honest accepted-count, and an unbroken sequence under real two-thread concurrency.

#include "doctest.h"
#include "core/util/SpscRing.h"

#include <cstdint>
#include <thread>
#include <vector>

// Elements come out in the order they went in, across many times the capacity (the indices wrap).
TEST_CASE("SpscRing delivers strict FIFO order across index wrap") {
    mm::SpscRing<uint32_t, 64> ring;
    uint32_t next = 0, expect = 0;
    for (int round = 0; round < 100; round++) {
        uint32_t in[40];
        for (auto& v : in) v = next++;
        const size_t accepted = ring.push(in, 40);
        REQUIRE(accepted == 40);   // 40 < 63 free after each full drain
        uint32_t out[40];
        REQUIRE(ring.pop(out, 40) == 40);
        for (size_t i = 0; i < 40; i++) CHECK(out[i] == expect++);
    }
}

// A full ring drops the NEWEST data: push reports how much was accepted and what was already queued is untouched, the consumer never sees a gap in the middle, only a truncated tail.
TEST_CASE("SpscRing overflow drops newest, reports the accepted count, and keeps queued data intact") {
    mm::SpscRing<uint32_t, 16> ring;   // 15 usable slots
    uint32_t in[20];
    for (uint32_t i = 0; i < 20; i++) in[i] = i;
    CHECK(ring.push(in, 20) == 15);    // 15 accepted, 5 newest dropped
    CHECK(ring.push(in, 5) == 0);      // full: nothing accepted
    uint32_t out[20];
    const size_t got = ring.pop(out, 20);
    CHECK(got == 15);
    for (uint32_t i = 0; i < 15; i++) CHECK(out[i] == i);   // the accepted prefix, in order
    CHECK(ring.pop(out, 20) == 0);     // drained
}

// Two real threads, producer faster than consumer at times and vice versa: every value that push() accepted arrives exactly once, in order, the acquire/release pairing at work.
TEST_CASE("SpscRing hands an unbroken in-order sequence across two threads") {
    mm::SpscRing<uint32_t, 256> ring;
    constexpr uint32_t kTotal = 200000;

    std::thread producer([&] {
        uint32_t v = 0;
        while (v < kTotal) {
            uint32_t chunk[37];
            uint32_t n = 0;
            while (n < 37 && v + n < kTotal) { chunk[n] = v + n; n++; }
            const size_t accepted = ring.push(chunk, n);
            v += static_cast<uint32_t>(accepted);   // retry whatever was dropped
        }
    });

    uint32_t expect = 0;
    bool ordered = true;
    while (expect < kTotal && ordered) {
        uint32_t out[61];
        const size_t got = ring.pop(out, 61);
        for (size_t i = 0; i < got && ordered; i++) {
            // Recorded rather than REQUIREd inside the loop: an aborting assertion here would skip producer.join() and take the whole test binary down with the thread.
            ordered = (out[i] == expect);
            expect++;
        }
    }
    if (!ordered) { uint32_t drain[61]; while (ring.pop(drain, 61)) {} }   // unblock the producer
    producer.join();
    CHECK(ordered);
    CHECK(expect == kTotal);
}

// Null buffers degrade to zero-count no-ops (the never-crash floor for a core construct).
TEST_CASE("SpscRing treats null buffers as empty operations") {
    mm::SpscRing<uint32_t, 16> ring;
    CHECK(ring.push(nullptr, 8) == 0);
    uint32_t v = 7;
    CHECK(ring.push(&v, 1) == 1);
    CHECK(ring.pop(nullptr, 8) == 0);
    uint32_t out = 0;
    CHECK(ring.pop(&out, 1) == 1);
    CHECK(out == 7);
}
