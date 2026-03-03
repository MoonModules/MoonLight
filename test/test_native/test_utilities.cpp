/**
    @title     MoonBase Unit Tests
    @file      test_utilities.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure functions from PureFunctions.h and Coord3D.h.
    Kept free of ESP32/Arduino header dependencies.
    Run with: pio test -e native
**/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Coord3D.h"
#include "PureFunctions.h"

// ============================================================
// Tests
// ============================================================

TEST_CASE("gcd") {
  CHECK_EQ(gcd(12, 18), 6);
  CHECK_EQ(gcd(7, 13), 1);
  CHECK_EQ(gcd(0, 5), 5);
  CHECK_EQ(gcd(5, 0), 5);
  CHECK_EQ(gcd(12, 8), 4);
}

TEST_CASE("lcm") {
  CHECK_EQ(lcm(12, 18), 36);
  CHECK_EQ(lcm(7, 13), 91);
  CHECK_EQ(lcm(12, 8), 24);
  CHECK_EQ(lcm(0, 0), 0);
  CHECK_EQ(lcm(0, 5), 0);
  CHECK_EQ(lcm(5, 0), 0);
}

TEST_CASE("fastDiv255") {
  CHECK_EQ(fastDiv255(0), 0);
  CHECK_EQ(fastDiv255(255), 1);
  CHECK_EQ(fastDiv255(127), 0);
  CHECK_EQ(fastDiv255(256), 1);
  CHECK_EQ(fastDiv255(510), 2);
  // verify against real division for a range
  for (uint32_t i = 0; i < 256 * 256; i++) {
    CHECK_EQ(fastDiv255(i), i / 255);
  }
}

TEST_CASE("crc16") {
  CHECK_EQ(crc16(nullptr, 0), 0x1D0F);
  const unsigned char data[] = {0x01, 0x02, 0x03};
  uint16_t c1 = crc16(data, 3);
  uint16_t c2 = crc16(data, 3);
  CHECK_EQ(c1, c2);  // deterministic
  // different data should (almost certainly) give different CRC
  const unsigned char data2[] = {0x04, 0x05, 0x06};
  CHECK_NE(c1, crc16(data2, 3));
}

TEST_CASE("getBitValue and setBitValue") {
  uint8_t bytes[2] = {0, 0};
  CHECK_FALSE(getBitValue(bytes, 0));
  setBitValue(bytes, 0, true);
  CHECK(getBitValue(bytes, 0));
  setBitValue(bytes, 0, false);
  CHECK_FALSE(getBitValue(bytes, 0));
  // test across byte boundary
  setBitValue(bytes, 8, true);
  CHECK(getBitValue(bytes, 8));
  CHECK_EQ(bytes[0], 0x00);
  CHECK_EQ(bytes[1], 0x01);
  // bit 5
  setBitValue(bytes, 5, true);
  CHECK(getBitValue(bytes, 5));
  CHECK_EQ(bytes[0], 0x20);
}

TEST_CASE("Coord3D") {
  SUBCASE("default constructor") {
    Coord3D c;
    CHECK_EQ(c.x, 0);
    CHECK_EQ(c.y, 0);
    CHECK_EQ(c.z, 0);
  }

  SUBCASE("parameterized constructor") {
    Coord3D c(3, 4, 5);
    CHECK_EQ(c.x, 3);
    CHECK_EQ(c.y, 4);
    CHECK_EQ(c.z, 5);

    Coord3D c2(10);
    CHECK_EQ(c2.x, 10);
    CHECK_EQ(c2.y, 0);
    CHECK_EQ(c2.z, 0);
  }

  SUBCASE("equality") {
    CHECK(Coord3D(1, 2, 3) == Coord3D(1, 2, 3));
    CHECK(Coord3D(1, 2, 3) != Coord3D(4, 5, 6));
    CHECK_FALSE(Coord3D(1, 2, 3) == Coord3D(1, 2, 4));
  }

  SUBCASE("arithmetic") {
    Coord3D a(10, 20, 30);
    Coord3D b(3, 4, 5);
    Coord3D sum = a + b;
    CHECK_EQ(sum.x, 13);
    CHECK_EQ(sum.y, 24);
    CHECK_EQ(sum.z, 35);

    Coord3D diff = a - b;
    CHECK_EQ(diff.x, 7);
    CHECK_EQ(diff.y, 16);
    CHECK_EQ(diff.z, 25);

    Coord3D prod = a * b;
    CHECK_EQ(prod.x, 30);
    CHECK_EQ(prod.y, 80);
    CHECK_EQ(prod.z, 150);

    Coord3D div = a / b;
    CHECK_EQ(div.x, 3);
    CHECK_EQ(div.y, 5);
    CHECK_EQ(div.z, 6);
  }

  SUBCASE("distanceSquared") {
    Coord3D a(0, 0, 0);
    Coord3D b(3, 4, 0);
    CHECK_EQ(a.distanceSquared(b), 25);

    Coord3D c(1, 2, 3);
    Coord3D d(4, 6, 3);
    CHECK_EQ(c.distanceSquared(d), 25);  // 9 + 16 + 0
  }

  SUBCASE("isOutofBounds") {
    Coord3D bounds(10, 10, 10);
    CHECK_FALSE(Coord3D(0, 0, 0).isOutofBounds(bounds));
    CHECK_FALSE(Coord3D(9, 9, 9).isOutofBounds(bounds));
    CHECK(Coord3D(10, 0, 0).isOutofBounds(bounds));
    CHECK(Coord3D(-1, 0, 0).isOutofBounds(bounds));
    CHECK(Coord3D(0, -1, 0).isOutofBounds(bounds));
  }

  SUBCASE("maximum") {
    Coord3D a(1, 5, 3);
    Coord3D b(4, 2, 6);
    Coord3D m = a.maximum(b);
    CHECK_EQ(m.x, 4);
    CHECK_EQ(m.y, 5);
    CHECK_EQ(m.z, 6);
  }
}

TEST_CASE("extractPath") {
  char path[64];
  CHECK(extractPath("/foo/bar/baz.txt", path, sizeof(path)) == 8);
  CHECK(strcmp(path, "/foo/bar") == 0);

  CHECK(extractPath("nodir.txt", path, sizeof(path)) == 0);
  CHECK(strcmp(path, "") == 0);

  CHECK(extractPath("/root.txt", path, sizeof(path)) == 0);
  CHECK(strcmp(path, "") == 0);

  // truncation: buffer holds only 4 bytes → 3 chars + null terminator
  char tiny[4];
  CHECK(extractPath("/foo/bar/baz.txt", tiny, sizeof(tiny)) == 3);
  CHECK(strcmp(tiny, "/fo") == 0);

  // zero-size buffer: no writes, returns 0
  CHECK(extractPath("/foo/bar/baz.txt", nullptr, 0) == 0);
}

TEST_CASE("distance") {
  CHECK(distance(0, 0, 0, 3, 4, 0) == doctest::Approx(5.0f).epsilon(0.001f));
  CHECK(distance(1, 2, 3, 1, 2, 3) == doctest::Approx(0.0f).epsilon(0.001f));
}

TEST_CASE("equal") {
  CHECK(equal("hello", "hello"));
  CHECK_FALSE(equal("hello", "world"));
  CHECK_FALSE(equal(nullptr, "hello"));
  CHECK_FALSE(equal("hello", nullptr));
  CHECK_FALSE(equal(nullptr, nullptr));
}

TEST_CASE("equalAZaz09") {
  CHECK(equalAZaz09("Hello World", "HelloWorld"));
  CHECK(equalAZaz09("foo-bar", "foo bar"));
  CHECK(equalAZaz09("test_123", "test 123"));
  CHECK_FALSE(equalAZaz09("abc", "def"));
  CHECK_FALSE(equalAZaz09(nullptr, "test"));
}

TEST_CASE("contains") {
  CHECK(contains("hello world", "world"));
  CHECK_FALSE(contains("hello", "world"));
  CHECK_FALSE(contains(nullptr, "test"));
  CHECK_FALSE(contains("test", nullptr));
}

// ============================================================
// FastLED fl::map_range tests
// ============================================================

// #include "fl/map_range.h"

// TEST_CASE("fl::map_range") {
//   SUBCASE("basic") {
//     CHECK_EQ(fl::map_range(5L, 0L, 16L, 0L, 32L), 10);
//     CHECK_EQ(fl::map_range(0L, 0L, 16L, 0L, 25L), 0);
//     CHECK_EQ(fl::map_range(16L, 0L, 16L, 0L, 25L), 25);  // exact at boundary
//   }

//   SUBCASE("exact boundaries") {
//     // Key advantage over Arduino map(): exact boundary values guaranteed
//     CHECK_EQ(fl::map_range((uint8_t)255, (uint8_t)0, (uint8_t)255, (uint8_t)0, (uint8_t)255), 255);
//     CHECK_EQ(fl::map_range((uint8_t)0, (uint8_t)0, (uint8_t)255, (uint8_t)0, (uint8_t)255), 0);

//     // u16 boundaries
//     CHECK_EQ(fl::map_range((uint16_t)65535, (uint16_t)0, (uint16_t)65535, (uint16_t)0, (uint16_t)15), 15);
//     CHECK_EQ(fl::map_range((uint16_t)0, (uint16_t)0, (uint16_t)65535, (uint16_t)0, (uint16_t)15), 0);
//   }

//   SUBCASE("pixel index") {
//     // The bug we fixed with Arduino map(): map(UINT16_MAX, 0, UINT16_MAX, 0, size) = size (out of bounds)
//     int size = 16;
//     CHECK_EQ(fl::map_range((long)65535, 0L, 65535L, 0L, (long)(size - 1)), size - 1);
//     CHECK_EQ(fl::map_range(0L, 0L, 65535L, 0L, (long)(size - 1)), 0);
//   }

//   SUBCASE("clamped") {
//     // fl::map_range_clamped prevents extrapolation — unlike Arduino map()
//     CHECK_EQ(fl::map_range_clamped(20L, 0L, 10L, 0L, 10L), 10);   // clamped to 10
//     CHECK_EQ(fl::map_range_clamped(-5L, 0L, 10L, 0L, 10L), 0);    // clamped to 0
//   }

//   SUBCASE("u8 specialization") {
//     // u8 specialization with overflow protection
//     CHECK_EQ(fl::map_range((uint8_t)128, (uint8_t)0, (uint8_t)255, (uint8_t)0, (uint8_t)255), 128);
//     CHECK_EQ(fl::map_range((uint8_t)0, (uint8_t)0, (uint8_t)255, (uint8_t)0, (uint8_t)100), 0);
//     CHECK_EQ(fl::map_range((uint8_t)255, (uint8_t)0, (uint8_t)255, (uint8_t)0, (uint8_t)100), 100);
//   }
// }
