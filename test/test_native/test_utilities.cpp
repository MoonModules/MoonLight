/**
    @title     MoonBase Unit Tests
    @file      test_utilities.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure functions from Utilities.h/.cpp and Char.h.
    These functions are copied here to avoid ESP32 header dependencies.
    Run with: pio test -e native
**/

#include <unity.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>

// ============================================================
// Pure functions copied from src/MoonBase/Utilities.h/.cpp
// Keep in sync with source when modifying the originals.
// ============================================================

#ifndef MIN
  #define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
  #define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

struct Coord3D {
  int x;
  int y;
  int z;

  Coord3D() : x(0), y(0), z(0) {}
  Coord3D(int x, int y = 0, int z = 0) : x(x), y(y), z(z) {}

  bool operator!=(const Coord3D& rhs) { return x != rhs.x || y != rhs.y || z != rhs.z; }
  bool operator==(const Coord3D rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z; }
  bool operator<(const int rhs) const { return x < rhs && y < rhs && z < rhs; }
  Coord3D operator-(const Coord3D rhs) const { return Coord3D(x - rhs.x, y - rhs.y, z - rhs.z); }
  Coord3D operator-(const int rhs) const { return Coord3D(x - rhs, y - rhs, z - rhs); }
  Coord3D operator+(const Coord3D rhs) const { return Coord3D(x + rhs.x, y + rhs.y, z + rhs.z); }
  Coord3D operator*(const Coord3D rhs) const { return Coord3D(x * rhs.x, y * rhs.y, z * rhs.z); }
  Coord3D operator/(const Coord3D rhs) const { return Coord3D(x / rhs.x, y / rhs.y, z / rhs.z); }
  Coord3D operator/(const int rhs) const { return Coord3D(x / rhs, y / rhs, z / rhs); }
  Coord3D operator%(const Coord3D rhs) const { return Coord3D(x % rhs.x, y % rhs.y, z % rhs.z); }
  Coord3D operator+=(const Coord3D rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
  Coord3D operator/=(const Coord3D rhs) {
    if (rhs.x) x /= rhs.x;
    if (rhs.y) y /= rhs.y;
    if (rhs.z) z /= rhs.z;
    return *this;
  }
  Coord3D maximum(const Coord3D rhs) const { return Coord3D(MAX(x, rhs.x), MAX(y, rhs.y), MAX(z, rhs.z)); }
  unsigned distanceSquared(const Coord3D rhs) const {
    Coord3D delta = (*this - rhs);
    return (delta.x) * (delta.x) + (delta.y) * (delta.y) + (delta.z) * (delta.z);
  }
  bool isOutofBounds(const Coord3D rhs) const { return x < 0 || y < 0 || z < 0 || x >= rhs.x || y >= rhs.y || z >= rhs.z; }
};

inline uint32_t fastDiv255(uint32_t x) { return (x * 0x8081u) >> 23; }

uint16_t gcd(uint16_t a, uint16_t b) {
  while (b != 0) { uint16_t t = b; b = a % b; a = t; }
  return a;
}

uint16_t lcm(uint16_t a, uint16_t b) { return a / gcd(a, b) * b; }

uint16_t crc16(const unsigned char* data_p, size_t length) {
  uint8_t x;
  uint16_t crc = 0xFFFF;
  if (!length) return 0x1D0F;
  while (length--) {
    x = crc >> 8 ^ *data_p++;
    x ^= x >> 4;
    crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x);
  }
  return crc;
}

bool getBitValue(const uint8_t* byteArray, size_t n) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  return (byteArray[byteIndex] >> bitIndex) & 1;
}

void setBitValue(uint8_t* byteArray, size_t n, bool value) {
  size_t byteIndex = n / 8;
  size_t bitIndex = n % 8;
  if (value)
    byteArray[byteIndex] |= (1 << bitIndex);
  else
    byteArray[byteIndex] &= ~(1 << bitIndex);
}

void extractPath(const char* filepath, char* path) {
  const char* lastSlash = strrchr(filepath, '/');
  if (lastSlash != nullptr) {
    size_t pathLength = lastSlash - filepath + 1;
    strlcpy(path, filepath, pathLength);
  } else {
    strcpy(path, "");
  }
}

float distance(float x1, float y1, float z1, float x2, float y2, float z2) {
  return sqrtf((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) + (z1 - z2) * (z1 - z2));
}

// From src/MoonBase/Char.h
inline bool equal(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  return strcmp(a, b) == 0;
}

inline bool equalAZaz09(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a || *b) {
    while (*a && !((*a >= '0' && *a <= '9') || (*a >= 'A' && *a <= 'Z') || (*a >= 'a' && *a <= 'z'))) a++;
    while (*b && !((*b >= '0' && *b <= '9') || (*b >= 'A' && *b <= 'Z') || (*b >= 'a' && *b <= 'z'))) b++;
    if (*a != *b) return false;
    if (*a) { a++; b++; }
  }
  return true;
}

inline bool contains(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  return strstr(a, b) != nullptr;
}

// ============================================================
// Tests
// ============================================================

void test_gcd_basic() {
  TEST_ASSERT_EQUAL_UINT16(6, gcd(12, 18));
  TEST_ASSERT_EQUAL_UINT16(1, gcd(7, 13));
  TEST_ASSERT_EQUAL_UINT16(5, gcd(0, 5));
  TEST_ASSERT_EQUAL_UINT16(5, gcd(5, 0));
  TEST_ASSERT_EQUAL_UINT16(4, gcd(12, 8));
}

void test_lcm_basic() {
  TEST_ASSERT_EQUAL_UINT16(36, lcm(12, 18));
  TEST_ASSERT_EQUAL_UINT16(91, lcm(7, 13));
  TEST_ASSERT_EQUAL_UINT16(24, lcm(12, 8));
}

void test_fastDiv255() {
  TEST_ASSERT_EQUAL_UINT32(0, fastDiv255(0));
  TEST_ASSERT_EQUAL_UINT32(1, fastDiv255(255));
  TEST_ASSERT_EQUAL_UINT32(0, fastDiv255(127));
  TEST_ASSERT_EQUAL_UINT32(1, fastDiv255(256));
  TEST_ASSERT_EQUAL_UINT32(2, fastDiv255(510));
  // verify against real division for a range
  for (uint32_t i = 0; i < 256 * 256; i++) {
    TEST_ASSERT_EQUAL_UINT32(i / 255, fastDiv255(i));
  }
}

void test_crc16_basic() {
  TEST_ASSERT_EQUAL_UINT16(0x1D0F, crc16(nullptr, 0));
  const unsigned char data[] = {0x01, 0x02, 0x03};
  uint16_t c1 = crc16(data, 3);
  uint16_t c2 = crc16(data, 3);
  TEST_ASSERT_EQUAL_UINT16(c1, c2);  // deterministic
  // different data should (almost certainly) give different CRC
  const unsigned char data2[] = {0x04, 0x05, 0x06};
  TEST_ASSERT_NOT_EQUAL(c1, crc16(data2, 3));
}

void test_getBitValue_setBitValue() {
  uint8_t bytes[2] = {0, 0};
  TEST_ASSERT_FALSE(getBitValue(bytes, 0));
  setBitValue(bytes, 0, true);
  TEST_ASSERT_TRUE(getBitValue(bytes, 0));
  setBitValue(bytes, 0, false);
  TEST_ASSERT_FALSE(getBitValue(bytes, 0));
  // test across byte boundary
  setBitValue(bytes, 8, true);
  TEST_ASSERT_TRUE(getBitValue(bytes, 8));
  TEST_ASSERT_EQUAL_UINT8(0x00, bytes[0]);
  TEST_ASSERT_EQUAL_UINT8(0x01, bytes[1]);
  // bit 5
  setBitValue(bytes, 5, true);
  TEST_ASSERT_TRUE(getBitValue(bytes, 5));
  TEST_ASSERT_EQUAL_UINT8(0x20, bytes[0]);
}

void test_coord3d_default_constructor() {
  Coord3D c;
  TEST_ASSERT_EQUAL_INT(0, c.x);
  TEST_ASSERT_EQUAL_INT(0, c.y);
  TEST_ASSERT_EQUAL_INT(0, c.z);
}

void test_coord3d_parameterized_constructor() {
  Coord3D c(3, 4, 5);
  TEST_ASSERT_EQUAL_INT(3, c.x);
  TEST_ASSERT_EQUAL_INT(4, c.y);
  TEST_ASSERT_EQUAL_INT(5, c.z);

  Coord3D c2(10);
  TEST_ASSERT_EQUAL_INT(10, c2.x);
  TEST_ASSERT_EQUAL_INT(0, c2.y);
  TEST_ASSERT_EQUAL_INT(0, c2.z);
}

void test_coord3d_equality() {
  TEST_ASSERT_TRUE(Coord3D(1, 2, 3) == Coord3D(1, 2, 3));
  TEST_ASSERT_TRUE(Coord3D(1, 2, 3) != Coord3D(4, 5, 6));
  TEST_ASSERT_FALSE(Coord3D(1, 2, 3) == Coord3D(1, 2, 4));
}

void test_coord3d_arithmetic() {
  Coord3D a(10, 20, 30);
  Coord3D b(3, 4, 5);
  Coord3D sum = a + b;
  TEST_ASSERT_EQUAL_INT(13, sum.x);
  TEST_ASSERT_EQUAL_INT(24, sum.y);
  TEST_ASSERT_EQUAL_INT(35, sum.z);

  Coord3D diff = a - b;
  TEST_ASSERT_EQUAL_INT(7, diff.x);
  TEST_ASSERT_EQUAL_INT(16, diff.y);
  TEST_ASSERT_EQUAL_INT(25, diff.z);

  Coord3D prod = a * b;
  TEST_ASSERT_EQUAL_INT(30, prod.x);
  TEST_ASSERT_EQUAL_INT(80, prod.y);
  TEST_ASSERT_EQUAL_INT(150, prod.z);

  Coord3D div = a / b;
  TEST_ASSERT_EQUAL_INT(3, div.x);
  TEST_ASSERT_EQUAL_INT(5, div.y);
  TEST_ASSERT_EQUAL_INT(6, div.z);
}

void test_coord3d_distanceSquared() {
  Coord3D a(0, 0, 0);
  Coord3D b(3, 4, 0);
  TEST_ASSERT_EQUAL_UINT(25, a.distanceSquared(b));

  Coord3D c(1, 2, 3);
  Coord3D d(4, 6, 3);
  TEST_ASSERT_EQUAL_UINT(25, c.distanceSquared(d));  // 9 + 16 + 0
}

void test_coord3d_isOutofBounds() {
  Coord3D bounds(10, 10, 10);
  TEST_ASSERT_FALSE(Coord3D(0, 0, 0).isOutofBounds(bounds));
  TEST_ASSERT_FALSE(Coord3D(9, 9, 9).isOutofBounds(bounds));
  TEST_ASSERT_TRUE(Coord3D(10, 0, 0).isOutofBounds(bounds));
  TEST_ASSERT_TRUE(Coord3D(-1, 0, 0).isOutofBounds(bounds));
  TEST_ASSERT_TRUE(Coord3D(0, -1, 0).isOutofBounds(bounds));
}

void test_coord3d_maximum() {
  Coord3D a(1, 5, 3);
  Coord3D b(4, 2, 6);
  Coord3D m = a.maximum(b);
  TEST_ASSERT_EQUAL_INT(4, m.x);
  TEST_ASSERT_EQUAL_INT(5, m.y);
  TEST_ASSERT_EQUAL_INT(6, m.z);
}

void test_extractPath() {
  char path[64];
  extractPath("/foo/bar/baz.txt", path);
  TEST_ASSERT_EQUAL_STRING("/foo/bar", path);

  extractPath("nodir.txt", path);
  TEST_ASSERT_EQUAL_STRING("", path);

  extractPath("/root.txt", path);
  TEST_ASSERT_EQUAL_STRING("", path);
}

void test_distance() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, distance(0, 0, 0, 3, 4, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, distance(1, 2, 3, 1, 2, 3));
}

void test_equal() {
  TEST_ASSERT_TRUE(equal("hello", "hello"));
  TEST_ASSERT_FALSE(equal("hello", "world"));
  TEST_ASSERT_FALSE(equal(nullptr, "hello"));
  TEST_ASSERT_FALSE(equal("hello", nullptr));
  TEST_ASSERT_FALSE(equal(nullptr, nullptr));
}

void test_equalAZaz09() {
  TEST_ASSERT_TRUE(equalAZaz09("Hello World", "HelloWorld"));
  TEST_ASSERT_TRUE(equalAZaz09("foo-bar", "foo bar"));
  TEST_ASSERT_TRUE(equalAZaz09("test_123", "test 123"));
  TEST_ASSERT_FALSE(equalAZaz09("abc", "def"));
  TEST_ASSERT_FALSE(equalAZaz09(nullptr, "test"));
}

void test_contains_func() {
  TEST_ASSERT_TRUE(contains("hello world", "world"));
  TEST_ASSERT_FALSE(contains("hello", "world"));
  TEST_ASSERT_FALSE(contains(nullptr, "test"));
  TEST_ASSERT_FALSE(contains("test", nullptr));
}

// ============================================================

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gcd_basic);
  RUN_TEST(test_lcm_basic);
  RUN_TEST(test_fastDiv255);
  RUN_TEST(test_crc16_basic);
  RUN_TEST(test_getBitValue_setBitValue);
  RUN_TEST(test_coord3d_default_constructor);
  RUN_TEST(test_coord3d_parameterized_constructor);
  RUN_TEST(test_coord3d_equality);
  RUN_TEST(test_coord3d_arithmetic);
  RUN_TEST(test_coord3d_distanceSquared);
  RUN_TEST(test_coord3d_isOutofBounds);
  RUN_TEST(test_coord3d_maximum);
  RUN_TEST(test_extractPath);
  RUN_TEST(test_distance);
  RUN_TEST(test_equal);
  RUN_TEST(test_equalAZaz09);
  RUN_TEST(test_contains_func);
  return UNITY_END();
}
