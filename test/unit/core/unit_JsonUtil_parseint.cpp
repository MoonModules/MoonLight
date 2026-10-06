/// @module parse
/// @also JsonUtil

/// Pins mm::parseIntStr and mm::json::parseInt on it; a non-number arrives as the fallback, since every caller clamps or compares the result.

#include "doctest.h"
#include "core/util/JsonUtil.h"
#include "core/util/parse.h"

#include <climits>

using namespace mm;

TEST_CASE("a JSON integer value is read up to the character that ends it") {
    // Digits run until the JSON punctuation that follows them, the reader is handed a pointer into the middle of a body, not a clean NUL-terminated number.
    CHECK(parseIntStr("7,\"next\":1") == 7);
    CHECK(parseIntStr("42}") == 42);
    CHECK(parseIntStr("-5,") == -5);
    CHECK(parseIntStr("0}") == 0);
}

TEST_CASE("text that does not start with a number reads as the fallback, not as zero-by-accident") {
    // The distinction atoi cannot make: it returns 0 for both "0" and "abc". A caller that treats 0 as "absent" needs the two to be separable, so the fallback is explicit.
    CHECK(parseIntStr("abc") == 0);
    CHECK(parseIntStr("") == 0);
    CHECK(parseIntStr(nullptr) == 0);
    CHECK(parseIntStr("abc", -1) == -1);
    CHECK(parseIntStr("", -1) == -1);
    // A real zero still reads as zero, the fallback must not swallow the valid value.
    CHECK(parseIntStr("0", -1) == 0);
}

TEST_CASE("a value too large to represent reads as the fallback instead of wrapping") {
    // atoi on an out-of-range value is undefined behavior, and a narrowing caller would store a different valid number: a Hue id of "65537" becomes light 1.
    CHECK(parseIntStr("99999999999999999999") == 0);
    CHECK(parseIntStr("99999999999999999999", -1) == -1);
    CHECK(parseIntStr("-99999999999999999999", -1) == -1);
    // The boundaries themselves still convert.
    CHECK(parseIntStr("2147483647") == INT_MAX);
    CHECK(parseIntStr("-2147483648") == INT_MIN);
    // One past each boundary: a 64-bit `long` (desktop) holds it and only the INT_MAX compare rejects it; a 32-bit one (ESP32) saturates with ERANGE.
    CHECK(parseIntStr("2147483648", -1) == -1);
    CHECK(parseIntStr("-2147483649", -1) == -1);
}

TEST_CASE("parseInt reads a key's integer value, and absent keys read as zero") {
    CHECK(json::parseInt("{\"a\":1,\"b\":22}", "b") == 22);
    CHECK(json::parseInt("{\"a\": 7}", "a") == 7);        // space after the colon (json.dumps)
    CHECK(json::parseInt("{\"a\":1}", "missing") == 0);
    CHECK(json::parseInt(nullptr, "a") == 0);
    CHECK(json::parseInt("{\"a\":1}", nullptr) == 0);   // no key to look for = absent
    // A non-numeric value for a present key is not a number: 0, same as absent.
    CHECK(json::parseInt("{\"a\":\"text\"}", "a") == 0);
}

// A hand-written value, in a catalog entry or a curl, is often quoted, and an I2C address follows its datasheet's hex; both read as the number they write.
TEST_CASE("a quoted number and a 0x hex number read as the value they write") {
    CHECK(parseIntStr("0x18}") == 24);
    CHECK(parseIntStr("0X7f,") == 127);
    CHECK(parseIntStr("017") == 17);   // a leading zero stays decimal, never octal
    CHECK(json::parseInt("{\"addr\":\"0x18\"}", "addr") == 24);
    CHECK(json::parseInt("{\"addr\": \"0x18\"}", "addr") == 24);
    CHECK(json::parseInt("{\"pin\":\"17\"}", "pin") == 17);
}
