/**
    @title     MoonBase Unit Tests
    @file      test_utilities.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure functions from PureFunctions.h and Coord3D.h.
    Kept free of ESP32/Arduino header dependencies.
    Run with: pio test -e native
**/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "MoonBase/utilities/BoardNames.h"
#include "MoonBase/utilities/Char.h"
#include "MoonBase/utilities/Coord3D.h"
#include "MoonBase/utilities/PureFunctions.h"

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
// Char<N> tests (included directly from Char.h — no copy!)
// ============================================================

TEST_CASE("Char: constructor from string literal") {
  Char<16> c("hello");
  CHECK(strcmp(c.c_str(), "hello") == 0);
}

TEST_CASE("Char: default constructor is empty") {
  Char<16> c;
  CHECK(c.length() == 0);
  CHECK(strcmp(c.c_str(), "") == 0);
}

TEST_CASE("Char: assignment from const char*") {
  Char<16> c;
  c = "world";
  CHECK(strcmp(c.c_str(), "world") == 0);
}

TEST_CASE("Char: comparison operators") {
  Char<16> a("test");
  CHECK(a == "test");
  CHECK(a != "other");

  Char<16> b("test");
  CHECK(a == b);
}

TEST_CASE("Char: length and c_str") {
  Char<16> c("hello");
  CHECK(c.length() == 5);
  CHECK(strcmp(c.c_str(), "hello") == 0);

  Char<16> empty;
  CHECK(empty.length() == 0);
}

TEST_CASE("Char: substring") {
  Char<16> c("hello world");
  Char<16> sub = c.substring(6, 11);
  CHECK(sub == "world");

  Char<16> sub2 = c.substring(0, 5);
  CHECK(sub2 == "hello");
}

TEST_CASE("Char: indexOf and contains") {
  Char<32> c("hello world");
  CHECK(c.indexOf("world") == 6);
  CHECK(c.indexOf("xyz") == SIZE_MAX);
  CHECK(c.contains("hello"));
  CHECK(c.contains("world"));
  CHECK_FALSE(c.contains("xyz"));
}

TEST_CASE("Char: format (printf-style)") {
  Char<32> c;
  c.format("val=%d str=%s", 42, "ok");
  CHECK(c == "val=42 str=ok");
}

TEST_CASE("Char: split with callback") {
  Char<32> c("one,two,three");
  int count = 0;
  const char* expected[] = {"one", "two", "three"};
  c.split(",", [&](const char* token, uint8_t seq) {
    CHECK(strcmp(token, expected[seq]) == 0);
    count++;
  });
  CHECK(count == 3);
  // original string should be restored after split
  CHECK(c == "one,two,three");
}

TEST_CASE("Char: concatenation + and +=") {
  Char<32> a("hello");
  Char<32> b = a + " world";
  CHECK(b == "hello world");

  Char<32> c("foo");
  c += "bar";
  CHECK(c == "foobar");

  Char<32> d("num");
  d += 42;
  CHECK(d == "num42");
}

TEST_CASE("Char: truncation when exceeding buffer") {
  Char<6> c("hello world");  // buffer is 6, fits 5 chars + null
  CHECK(c.length() == 5);
  CHECK(c == "hello");
}

TEST_CASE("Char: converting constructor between sizes") {
  Char<32> big("hello world");
  Char<8> small(big);  // truncates to 7 chars + null
  CHECK(small == "hello w");
  CHECK(small.length() == 7);
}

TEST_CASE("Char: operator[] access") {
  Char<16> c("abc");
  CHECK(c[0] == 'a');
  CHECK(c[1] == 'b');
  CHECK(c[2] == 'c');
  CHECK(c[100] == '\0');  // out of bounds returns null
}

TEST_CASE("Char: toInt and toFloat") {
  Char<16> i("42");
  CHECK(i.toInt() == 42);

  Char<16> f("3.14");
  CHECK(f.toFloat() == doctest::Approx(3.14f).epsilon(0.01f));
}

TEST_CASE("Char: assignment from String (std::string)") {
  String s = "from string";
  Char<16> c;
  c = s;
  CHECK(c == "from string");
}

TEST_CASE("Char: += with String") {
  Char<32> c("hello ");
  String s = "world";
  c += s;
  CHECK(c == "hello world");
}

TEST_CASE("Char: += with another Char") {
  Char<32> a("hello ");
  Char<16> b("world");
  a += b;
  CHECK(a == "hello world");
}

TEST_CASE("Char: cross-size assignment") {
  Char<32> big("long string here");
  Char<8> small;
  small = big;
  CHECK(small == "long st");
}

TEST_CASE("Char: non-member operator+ (const char* + Char)") {
  Char<32> c("world");
  Char<32> result = "hello " + c;
  CHECK(result == "hello world");
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

// ============================================================
// BoardName::fromLegacyId — tests the actual header
// ============================================================

TEST_CASE("BoardName::fromLegacyId") {
  SUBCASE("all valid indices return names[i]") {
    for (int i = 0; i < (int)BoardName::count; i++) {
      CHECK_EQ(BoardName::fromLegacyId(i), BoardName::names[i]);
    }
  }
  SUBCASE("negative index returns names[0]") {
    CHECK_EQ(BoardName::fromLegacyId(-1), BoardName::names[0]);
  }
  SUBCASE("index >= count returns names[0]") {
    CHECK_EQ(BoardName::fromLegacyId((int)BoardName::count),     BoardName::names[0]);
    CHECK_EQ(BoardName::fromLegacyId((int)BoardName::count + 99), BoardName::names[0]);
  }
  SUBCASE("spot-check known entries") {
    CHECK_EQ(std::string(BoardName::fromLegacyId(0)),  "");
    CHECK_EQ(std::string(BoardName::fromLegacyId(5)),  "QuinLED Dig-Octa v2");
    CHECK_EQ(std::string(BoardName::fromLegacyId(8)),  "SE16 v1");
    CHECK_EQ(std::string(BoardName::fromLegacyId(19)), "Olimex ESP32-POE");
  }
}
