/// @module sha1

#include "doctest.h"
#include "core/system/Sha1.h"

#include <cstdio>
#include <string>

namespace {
std::string hex(const std::string& in) {
    uint8_t d[20];
    mm::sha1(reinterpret_cast<const uint8_t*>(in.data()), in.size(), d);
    char buf[41];
    for (int i = 0; i < 20; i++) std::snprintf(buf + i * 2, 3, "%02x", d[i]);
    return std::string(buf);
}
}  // namespace

// The digest matches the published FIPS 180-4 vectors: the empty string, "abc", and the two-block example.
TEST_CASE("sha1 matches the published test vectors") {
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// A message of 55, 56 and 64 bytes hashes to the known digest: 55 leaves room for the length, 56 forces a second padding block, 64 is a whole block with all the padding after it.
TEST_CASE("sha1 pads correctly around the block boundary") {
    CHECK(hex(std::string(55, 'a')) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(hex(std::string(56, 'a')) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(hex(std::string(64, 'a')) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
}

// One million 'a' hashes to the published digest, far past the 503 bytes the old fixed buffer could pad.
TEST_CASE("sha1 hashes an input of any size") {
    CHECK(hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}
