/// @module JsonUtil

/// Pins what a script's `json` values stand on: addressing a document by JSON Pointer (RFC 6901), reading decimals as fixed point, editing in place and writing it back as text.

#include "doctest.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"

#include <cstring>
#include <string>

using namespace mm;

namespace {
/// A node, or a document, written back as text.
std::string text(const json::JsonDoc& doc, const json::JsonNode* n) {
    JsonSink sink;
    json::write(sink, doc, n);
    return sink.data();
}
std::string text(const json::JsonDoc& doc) { return text(doc, doc.rootNode()); }
}  // namespace

TEST_CASE("a JSON Pointer finds every value of RFC 6901's own example") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"foo":["bar","baz"],"":0,"a/b":1,"c%d":2,"e^f":3,"g|h":4,"i\\j":5,"k\"l":6," ":7,"m~n":8})", doc));
    CHECK(json::at(doc, "") == doc.rootNode());
    CHECK(text(doc, json::at(doc, "/foo")) == R"(["bar","baz"])");
    CHECK(text(doc, json::at(doc, "/foo/0")) == R"("bar")");
    CHECK(json::readInt(json::at(doc, "/")) == 0);
    CHECK(json::readInt(json::at(doc, "/a~1b")) == 1);
    CHECK(json::readInt(json::at(doc, "/c%d")) == 2);
    CHECK(json::readInt(json::at(doc, "/e^f")) == 3);
    CHECK(json::readInt(json::at(doc, "/g|h")) == 4);
    CHECK(json::readInt(json::at(doc, "/i\\j")) == 5);
    CHECK(json::readInt(json::at(doc, "/k\"l")) == 6);
    CHECK(json::readInt(json::at(doc, "/ ")) == 7);
    CHECK(json::readInt(json::at(doc, "/m~0n")) == 8);
}

TEST_CASE("a pointer that leaves the document finds nothing") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"results":[{"title":"x"}]})", doc));
    CHECK(json::at(doc, "/results/1") == nullptr);        // past the end
    CHECK(json::at(doc, "/results/01") == nullptr);       // a leading zero is not an index
    CHECK(json::at(doc, "/results/0/title/x") == nullptr); // into a string
    CHECK(json::at(doc, "results") == nullptr);           // no leading slash
    CHECK(json::at(doc, nullptr) == nullptr);
    json::JsonDoc empty;
    CHECK(json::at(empty, "") == nullptr);
}

TEST_CASE("a decimal reads as fixed point and truncated as a whole number") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"t":21.5,"n":-0.5,"k":1e3,"q":2.5e-1,"big":123456,"tiny":0.00001})", doc));
    CHECK(json::readFixed(json::at(doc, "/t")) == 21 * 65536 + 32768);
    CHECK(json::readInt(json::at(doc, "/t")) == 21);
    CHECK(json::readFixed(json::at(doc, "/n")) == -32768);
    CHECK(json::readInt(json::at(doc, "/n")) == 0);
    CHECK(json::readInt(json::at(doc, "/k")) == 1000);   // the exponent counts
    CHECK(json::readFixed(json::at(doc, "/q")) == 16384);
    CHECK(json::readFixed(json::at(doc, "/big")) == INT32_MAX);   // past fixed point's range: saturated
    CHECK(json::readInt(json::at(doc, "/big")) == 123456);
    CHECK(json::readFixed(json::at(doc, "/tiny")) == 1);  // rounded to the nearest 1/65536
    CHECK(json::readFixed(json::at(doc, "/missing"), -1) == -1);
}

TEST_CASE("a parsed document writes back exactly as it was written") {
    const char* in = R"({"a":[1,2.50,-3e2],"b":"x\"y","c":null,"d":true,"e":{}})";
    json::JsonDoc doc;
    REQUIRE(json::parse(in, doc));
    CHECK(text(doc) == in);
}

TEST_CASE("a set creates the members and objects its pointer names") {
    json::JsonDoc doc;
    REQUIRE(json::setInt(doc, "/Effects/Layer/opacity", 128));
    REQUIRE(json::setString(doc, "/Effects/Layer/Text/text", "21.5 C"));
    REQUIRE(json::setFixed(doc, "/temp", 21 * 65536 + 32768));
    CHECK(text(doc) == R"({"Effects":{"Layer":{"opacity":128,"Text":{"text":"21.5 C"}}},"temp":21.5000})");
    // A second set replaces rather than adding a twin.
    REQUIRE(json::setInt(doc, "/Effects/Layer/opacity", 0));
    CHECK(json::readInt(json::at(doc, "/Effects/Layer/opacity"), -1) == 0);
    CHECK(text(doc, json::at(doc, "/Effects/Layer")) == R"({"opacity":0,"Text":{"text":"21.5 C"}})");
}

TEST_CASE("a set into an array replaces an element or appends at '-'") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"list":[1,2]})", doc));
    CHECK(json::setInt(doc, "/list/1", 5));
    CHECK(json::setInt(doc, "/list/-", 9));
    CHECK_FALSE(json::setInt(doc, "/list/7", 1));        // past the end
    CHECK_FALSE(json::setInt(doc, "/list/0/x", 1));      // through a number
    CHECK(text(doc) == R"({"list":[1,5,9]})");
}

TEST_CASE("a value copies between documents, whole objects included") {
    json::JsonDoc feed, show;
    REQUIRE(json::parse(R"({"results":[{"title":"Rocket \"Lab\" launches","n":3.25}]})", feed));
    REQUIRE(json::parse(R"({"Text":{"text":""}})", show));
    CHECK(json::copyValue(show, "/Text/text", feed, "/results/0/title"));
    CHECK(json::copyValue(show, "/item", feed, "/results/0"));
    CHECK_FALSE(json::copyValue(show, "/x", feed, "/results/4"));
    CHECK(text(show) == R"({"Text":{"text":"Rocket \"Lab\" launches"},"item":{"title":"Rocket \"Lab\" launches","n":3.25}})");
}

TEST_CASE("a value copies into its own subtree without looping") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"a":{"x":1}})", doc));
    CHECK(json::copyValue(doc, "/a/b", doc, "/a"));
    CHECK(text(doc) == R"({"a":{"x":1,"b":{"x":1}}})");
}

TEST_CASE("an edited copy outlives the document it came from") {
    json::JsonDoc dst;
    {
        json::JsonDoc src;
        REQUIRE(json::parse(R"({"s":"kept","n":1.75})", src));
        REQUIRE(json::copyValue(dst, "/v", src, ""));
    }
    CHECK(text(dst) == R"({"v":{"s":"kept","n":1.75}})");
}

TEST_CASE("a \\u escape decodes to UTF-8, a surrogate pair to one character") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"t":"it’s é 🚀 A \ud800x"})", doc));
    char s[64];
    REQUIRE(json::readString(json::at(doc, "/t"), s, sizeof(s)));
    CHECK(std::string(s) == "it\xE2\x80\x99s \xC3\xA9 \xF0\x9F\x9A\x80 A ?x");
    // A high half followed by an escape that is not its low half: the half reads '?', and the escape after it still decodes.
    REQUIRE(json::parse(R"({"t":"\ud83d\u0041\ud83d\ude80"})", doc));
    REQUIRE(json::readString(json::at(doc, "/t"), s, sizeof(s)));
    CHECK(std::string(s) == "?A\xF0\x9F\x9A\x80");
}

TEST_CASE("a quoted number reads as the number it starts with, as services quote theirs") {
    json::JsonDoc doc;
    REQUIRE(json::parse(R"({"price":"81201.915","pct":"0.2585%","score":"3","name":"Arsenal","n":7})", doc));
    long w = 0; int32_t f = 0;
    REQUIRE(json::readNumber(json::at(doc, "/price"), w, f));
    CHECK(w == 81201);
    REQUIRE(json::readNumber(json::at(doc, "/pct"), w, f));
    CHECK(f == 16941);   // 0.2585 in Q16.16, rounded
    REQUIRE(json::readNumber(json::at(doc, "/score"), w, f));
    CHECK(w == 3);
    REQUIRE(json::readNumber(json::at(doc, "/n"), w, f));
    CHECK(w == 7);
    CHECK_FALSE(json::readNumber(json::at(doc, "/name"), w, f));
    CHECK_FALSE(json::readNumber(json::at(doc, "/missing"), w, f));
}

TEST_CASE("text joins onto a string, from the caller or from another document's value") {
    json::JsonDoc feed, show;
    REQUIRE(json::parse(R"({"data":{"amount":"81201.915"},"t":9.4,"ok":true})", feed));
    REQUIRE(json::appendText(show, "/text", "BTC $"));
    REQUIRE(json::appendValue(show, "/text", feed, "/data/amount"));
    REQUIRE(json::appendText(show, "/text", " / "));
    REQUIRE(json::appendValue(show, "/text", feed, "/t"));
    REQUIRE(json::appendValue(show, "/text", feed, "/ok"));
    CHECK_FALSE(json::appendValue(show, "/text", feed, "/nope"));
    CHECK(text(show) == R"({"text":"BTC $81201.915 / 9.4true"})");
}

// The demo edits its bands each time a feed changes: the text and the nodes an edit replaces were kept until the document went, so a device ran out of memory within days.
TEST_CASE("an edit repeated forever holds the memory of one, its replaced text and nodes reused") {
    json::JsonDoc feed, show;
    REQUIRE(json::parse(R"({"item":{"title":"news","n":3.25,"tags":["a","b"]}})", feed));
    REQUIRE(json::parse(R"({"Text":{"text":""}})", show));
    int owned = 0, nodes = 0;
    for (int round = 0; round < 1000; round++) {
        REQUIRE(json::setString(show, "/Text/text", "BTC "));
        REQUIRE(json::appendValue(show, "/Text/text", feed, "/item/n"));
        REQUIRE(json::setFixed(show, "/Text/speed", 98304));
        REQUIRE(json::copyValue(show, "/item", feed, "/item"));
        if (round == 0) { owned = show.ownedCount; nodes = show.count; continue; }
        if (show.ownedCount != owned || show.count != nodes) break;   // one report, not a thousand
    }
    CHECK(show.ownedCount == owned);   // every key and string the document holds, once
    CHECK(show.count == nodes);
    CHECK(text(show) == R"({"Text":{"text":"BTC 3.25","speed":1.5000},"item":{"title":"news","n":3.25,"tags":["a","b"]}})");
}
