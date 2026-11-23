/// @module BeatClock

/// The receiver rules of the beat section, on synthesized packets at 120 BPM: a beat every 500 ms, a packet every 25 ms as audio sync sends them.

#include "doctest.h"
#include "core/util/BeatClock.h"

#include <cstdint>

using namespace mm;

namespace {
/// A packet's beat at `ms` for a source at 120 BPM that locked at 0, its bar known from beat 1.
AudioBeat at120(uint32_t ms) {
    AudioBeat b;
    b.bpm100 = 12000;
    b.count = static_cast<uint16_t>(ms / 500u);
    b.phase = static_cast<uint8_t>((ms % 500u) * 256u / 500u);
    b.inBar = static_cast<uint8_t>((ms / 500u) % 4u + 1u);
    b.confidence = 255;
    return b;
}
}  // namespace

TEST_CASE("a packet a beat apart runs the phase between them, and the beat starts once on each count") {
    BeatClock c;
    int beats = 0;
    uint8_t phaseMid = 0;
    // One packet a beat, read every 10 ms: the clock fills the 50 ticks between packets.
    for (uint32_t ms = 0; ms < 3000; ms += 10) {
        if (ms % 500u == 0) c.take(at120(ms), ms);
        const BeatNow n = c.tick(ms);
        if (n.onBeat) beats++;
        if (ms == 1250) phaseMid = n.phase;
    }
    CHECK(beats == 5);   // beats 1 to 5; the lock at 0 is the start, not a beat
    CHECK(phaseMid > 118);
    CHECK(phaseMid < 138);   // half way through the beat
}

TEST_CASE("the beat a reader sees keeps the source's tempo, bar and confidence") {
    BeatClock c;
    c.take(at120(1500), 1500);   // beat 3, the bar's fourth
    const BeatNow n = c.tick(1500);
    CHECK(n.live());
    CHECK(n.bpm100 == 12000);
    CHECK(n.inBar == 4);
    CHECK(n.confidence == 255);
    const BeatNow next = c.tick(2010);   // run on past the last packet into the next beat
    CHECK(next.onBeat);
    CHECK(next.inBar == 1);   // the bar counts on from the packet
}

TEST_CASE("packets with jitter move the beat less than they move themselves") {
    BeatClock c;
    c.take(at120(0), 0);
    c.tick(0);
    // Packets arrive on time and 20 ms late in turn, ±5 phase steps around their mean lateness of 10 ms; the clock, pulled a quarter of each error, stays closer to that mean.
    int worst = 0;
    for (uint32_t k = 1; k < 80; k++) {
        const uint32_t sent = k * 25u, arrived = sent + (k % 2 ? 20u : 0u);
        c.take(at120(sent), arrived);
        const int phase = c.tick(arrived).phase;
        const int mean = static_cast<int>(((arrived - 10u) % 500u) * 256u / 500u);
        int dev = phase - mean;
        if (dev > 128) dev -= 256;
        if (dev < -128) dev += 256;
        if (k > 20 && (dev < 0 ? -dev : dev) > worst) worst = dev < 0 ? -dev : dev;
    }
    CHECK(worst <= 3);
}

TEST_CASE("a jump of several beats is one beat start, and the count follows the source") {
    BeatClock c;
    c.take(at120(0), 0);
    c.tick(0);
    c.take(at120(2000), 2000);   // four beats on, the packets between lost
    const BeatNow n = c.tick(2000);
    CHECK(n.onBeat);
    CHECK(n.count == 4);
}

TEST_CASE("the beat ends four beats after the last packet, and a section with no tempo changes nothing") {
    BeatClock c;
    c.take(at120(0), 0);
    CHECK(c.tick(1900).live());
    AudioBeat none;
    c.take(none, 1950);   // no tempo is no beat, so the clock runs on
    CHECK(c.tick(1990).live());
    CHECK_FALSE(c.tick(2100).live());   // 4 × 500 ms after the packet
    CHECK(c.tick(2100).bpm100 == 0);
}

TEST_CASE("a source at another tempo sets the clock at once, and a count that wraps past 65535 stays one beat") {
    BeatClock c;
    AudioBeat b = at120(0);
    b.count = 65535;
    c.take(b, 0);
    c.tick(0);
    b.count = 0;   // the next beat, the 16-bit count wrapping
    c.take(b, 500);
    BeatNow n = c.tick(500);
    CHECK(n.onBeat);
    CHECK(n.count == 0);
    b.bpm100 = 14000;   // a new track at 140 BPM
    b.count = 1;
    c.take(b, 900);
    n = c.tick(900);
    CHECK(n.bpm100 == 14000);
    CHECK(n.count == 1);
}

TEST_CASE("a source that moves several beats back is taken at once, as a sender change or a deck handover is") {
    BeatClock c;
    c.take(at120(51000), 51000);   // beat 102
    c.tick(51000);
    AudioBeat b = at120(51000);
    b.count = 98;                  // another sender, four beats behind
    c.take(b, 51010);
    BeatNow n = c.tick(51010);
    CHECK(n.count == 98);
    n = c.tick(51510);             // the next beat starts on time rather than four beats late
    CHECK(n.onBeat);
    CHECK(n.count == 99);
}

TEST_CASE("the phrase counts on from the packet and wraps at its length") {
    BeatClock c;
    AudioBeat b = at120(0);
    b.inPhrase = 31; b.phraseBeats = 32;
    c.take(b, 0);
    CHECK(c.tick(0).inPhrase == 31);
    CHECK(c.tick(510).inPhrase == 32);
    const BeatNow n = c.tick(1010);
    CHECK(n.inPhrase == 1);        // the next phrase
    CHECK(n.phraseBeats == 32);
}

TEST_CASE("a new tempo one beat behind shows the new count at once, and its next beat starts") {
    BeatClock c;
    AudioBeat b = at120(5000);   // beat 10
    c.take(b, 5000);
    c.tick(5000);
    b.bpm100 = 14000;            // a new track at 140 BPM, its count one behind
    b.count = 9;
    b.phase = 0;
    c.take(b, 5010);
    BeatNow n = c.tick(5010);
    CHECK(n.count == 9);
    CHECK_FALSE(n.onBeat);
    n = c.tick(5010 + 430);      // a beat at 140 BPM is 429 ms
    CHECK(n.onBeat);
    CHECK(n.count == 10);
}
