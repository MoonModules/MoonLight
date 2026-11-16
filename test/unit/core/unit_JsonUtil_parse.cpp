/// @module JsonUtil

/// Pins the recursive JSON reader (mm::json::parse + the walk accessors). The flat helpers (parseString/hasKey/parseInt/parseBool) stay first-match-only and are covered elsewhere; this file exercises the recursive-descent parser that fills a bounded node arena. The headline case is an array of small objects, the persisted device list DevicesModule loads at boot. Robustness is the other half: malformed, truncated, garbage, null, and oversized inputs must fail cleanly with no crash.

#include "doctest.h"
#include "core/util/JsonUtil.h"

#include <cstring>
#include <string>

using namespace mm;

TEST_CASE("parse a flat object reads each typed field") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"a\":1,\"b\":\"x\",\"c\":true}", doc));
    const json::JsonNode* root = doc.rootNode();
    REQUIRE(root != nullptr);
    CHECK(root->type == json::JsonType::Object);

    CHECK(json::readInt(json::member(doc, root, "a")) == 1);

    char s[8];
    CHECK(json::readString(json::member(doc, root, "b"), s, sizeof(s)));
    CHECK(std::strcmp(s, "x") == 0);

    CHECK(json::readBool(json::member(doc, root, "c")) == true);

    // Absent key -> nullptr -> safe defaults.
    CHECK(json::member(doc, root, "missing") == nullptr);
    CHECK(json::readInt(json::member(doc, root, "missing"), -1) == -1);
}

TEST_CASE("parse an array of objects (the persisted device list use case)") {
    json::JsonDoc doc;
    const char* devices =
        "[{\"ip\":\"192.168.1.5\",\"name\":\"WLED\",\"type\":2},"
        "{\"ip\":\"192.168.1.9\",\"name\":\"MM-AB\",\"type\":1}]";
    REQUIRE(json::parse(devices, doc));

    const json::JsonNode* arr = doc.rootNode();
    REQUIRE(arr != nullptr);
    CHECK(arr->type == json::JsonType::Array);
    REQUIRE(json::arraySize(doc, arr) == 2);

    const json::JsonNode* d0 = json::element(doc, arr, 0);
    REQUIRE(d0 != nullptr);
    char ip[24];
    CHECK(json::readString(json::member(doc, d0, "ip"), ip, sizeof(ip)));
    CHECK(std::strcmp(ip, "192.168.1.5") == 0);
    char name[16];
    CHECK(json::readString(json::member(doc, d0, "name"), name, sizeof(name)));
    CHECK(std::strcmp(name, "WLED") == 0);
    CHECK(json::readInt(json::member(doc, d0, "type")) == 2);

    const json::JsonNode* d1 = json::element(doc, arr, 1);
    REQUIRE(d1 != nullptr);
    CHECK(json::readString(json::member(doc, d1, "ip"), ip, sizeof(ip)));
    CHECK(std::strcmp(ip, "192.168.1.9") == 0);
    CHECK(json::readString(json::member(doc, d1, "name"), name, sizeof(name)));
    CHECK(std::strcmp(name, "MM-AB") == 0);
    CHECK(json::readInt(json::member(doc, d1, "type")) == 1);

    // Out-of-range element is safe.
    CHECK(json::element(doc, arr, 2) == nullptr);
    CHECK(json::element(doc, arr, -1) == nullptr);
}

TEST_CASE("parse a nested object") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"outer\":{\"inner\":{\"v\":42}}}", doc));
    const json::JsonNode* outer = json::member(doc, doc.rootNode(), "outer");
    REQUIRE(outer != nullptr);
    CHECK(outer->type == json::JsonType::Object);
    const json::JsonNode* inner = json::member(doc, outer, "inner");
    REQUIRE(inner != nullptr);
    CHECK(json::readInt(json::member(doc, inner, "v")) == 42);
}

TEST_CASE("escaped quotes and backslashes round-trip inside a string value") {
    json::JsonDoc doc;
    // Source bytes: {"s":"a\"b\\c"} -> value is  a"b\c
    REQUIRE(json::parse("{\"s\":\"a\\\"b\\\\c\"}", doc));
    char s[16];
    CHECK(json::readString(json::member(doc, doc.rootNode(), "s"), s, sizeof(s)));
    CHECK(std::strcmp(s, "a\"b\\c") == 0);

    // \n escape decodes to a real newline.
    json::JsonDoc doc2;
    REQUIRE(json::parse("{\"s\":\"line1\\nline2\"}", doc2));
    CHECK(json::readString(json::member(doc2, doc2.rootNode(), "s"), s, sizeof(s)));
    CHECK(std::strcmp(s, "line1\nline2") == 0);
}

TEST_CASE("negative and fractional numbers") {
    json::JsonDoc doc;
    REQUIRE(json::parse("{\"a\":-7,\"b\":3.9}", doc));
    CHECK(json::readInt(json::member(doc, doc.rootNode(), "a")) == -7);
    // Fractional values truncate to int (we never persist floats).
    CHECK(json::readInt(json::member(doc, doc.rootNode(), "b")) == 3);
}

TEST_CASE("malformed inputs fail cleanly without crashing") {
    json::JsonDoc doc;

    CHECK_FALSE(json::parse(nullptr, doc));
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("", doc));
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("[{\"ip\":", doc));        // truncated
    CHECK_FALSE(doc.valid());

    CHECK_FALSE(json::parse("{\"a\":1", doc));          // unbalanced brace
    CHECK_FALSE(json::parse("}{][", doc));              // garbage
    CHECK_FALSE(json::parse("{\"a\":}", doc));          // missing value
    CHECK_FALSE(json::parse("{\"a\" 1}", doc));         // missing colon
    CHECK_FALSE(json::parse("{\"a\":1}garbage", doc));  // trailing garbage
    CHECK_FALSE(json::parse("\"unterminated", doc));    // unterminated string
    CHECK_FALSE(json::parse("[1,2,", doc));             // trailing comma + truncation

    // After a failed parse the doc stays invalid; accessors on it are safe.
    CHECK_FALSE(doc.valid());
    CHECK(doc.rootNode() == nullptr);
    CHECK(json::member(doc, doc.rootNode(), "x") == nullptr);
    CHECK(json::arraySize(doc, doc.rootNode()) == 0);
}

TEST_CASE("no node cap: a large array parses (heap-grown node pool)") {
    // The node pool is heap-allocated and grows as needed, there is NO fixed node cap. An array far larger than the old 128-node limit must PARSE, not fail. The pool grows via realloc, and nodes are index-addressed so the growth never dangles a child/next reference.
    const int N = 5000;
    std::string big = "[";
    for (int i = 0; i < N; i++) { if (i) big += ","; big += "1"; }
    big += "]";

    json::JsonDoc doc;
    REQUIRE(json::parse(big.c_str(), doc));
    CHECK(doc.valid());
    CHECK(json::arraySize(doc, doc.rootNode()) == N);   // every element present, none dropped
    // Spot-check a late element to prove the index links survived the realloc growth.
    CHECK(json::readInt(json::element(doc, doc.rootNode(), N - 1), -1) == 1);
}

TEST_CASE("overflow safety: nesting deeper than kMaxDepth fails cleanly") {
    // Depth is STILL bounded (the one remaining cap, it guards the ESP32 task stack against a pathologically-nested input). Nesting past kMaxDepth fails cleanly, no stack blow.
    std::string deep;
    for (int i = 0; i < json::kMaxDepth + 5; i++) deep += "[";
    for (int i = 0; i < json::kMaxDepth + 5; i++) deep += "]";

    json::JsonDoc doc;
    CHECK_FALSE(json::parse(deep.c_str(), doc));
    CHECK_FALSE(doc.valid());
}

TEST_CASE("no length cap: a long input parses (heap-sized text arena)") {
    // The text arena is heap-allocated sized to the input, no fixed length cap. A long string value (well past the old 4096-byte buffer) must parse. Build a valid ~10 KB JSON string.
    std::string huge = "\"";
    huge.append(10000, 'x');
    huge += "\"";
    json::JsonDoc doc;
    REQUIRE(json::parse(huge.c_str(), doc));
    CHECK(doc.valid());
}

// parseString must DECODE the JSON string escapes our own writer emits (JsonSink/writeJsonString), \" \\ \n \r \t \b \f, so reader and writer are symmetric. A multi-line value (a script with `\n`) must arrive as a real newline, not a literal backslash-n.
TEST_CASE("parseString decodes the standard JSON string escapes (symmetric with the writer)") {
    char out[64];
    json::parseString("{\"s\":\"a\\nb\\tc\"}", "s", out, sizeof(out));
    CHECK(std::strcmp(out, "a\nb\tc") == 0);          // \n and \t decoded

    json::parseString("{\"s\":\"q=\\\"x\\\" back=\\\\\"}", "s", out, sizeof(out));
    CHECK(std::strcmp(out, "q=\"x\" back=\\") == 0);  // \" and \\ still work

    // \r \b \f, the remaining named escapes the writer emits
    json::parseString("{\"s\":\"r\\rb\\bf\\f\"}", "s", out, sizeof(out));
    CHECK(std::strcmp(out, "r\rb\bf\f") == 0);

    // \u00XX, the writer emits this for control bytes < 0x20; the reader decodes the low byte
    json::parseString("{\"s\":\"x\\u0001y\\u001f\"}", "s", out, sizeof(out));
    CHECK(out[0] == 'x'); CHECK(out[1] == 0x01); CHECK(out[2] == 'y'); CHECK(out[3] == 0x1f);

    // a multi-line script value (the MoonLive Stage-1 case)
    json::parseString("{\"source\":\"uint8_t s = 1;\\nsetRGB(s,0,0,255);\"}",
                      "source", out, sizeof(out));
    CHECK(std::strchr(out, '\n') != nullptr);          // real newline, so the // comment ends
    CHECK(std::strstr(out, "setRGB") != nullptr);      // the statement survives on its own line
}

// JSON allows whitespace around a member's colon, newlines included, so every simple reader finds its key however a client formats the body.
TEST_CASE("the simple readers find a key with any whitespace around its colon, and pass over its text inside a value") {
    char out[16] = {};
    json::parseString("{\"name\":\n\t\"Dial\"}", "name", out, sizeof(out));
    CHECK(std::string(out) == "Dial");
    json::parseString("{\"name\" : \"Knob\"}", "name", out, sizeof(out));
    CHECK(std::string(out) == "Knob");
    CHECK(json::parseInt("{\"to\" :\r\n 3}", "to") == 3);
    CHECK(json::parseBool("{\"on\"\n:\ntrue}", "on"));
    CHECK(json::hasKey("{\"id\" : null}", "id"));
    // "name" as a value's text, before the member that holds it, is no key
    json::parseString("{\"label\":\"name\",\"name\":\"Lamp\"}", "name", out, sizeof(out));
    CHECK(std::string(out) == "Lamp");
}
