/// @module HttpServerModule
///
/// A control client receives the modules it subscribes to in full and an outline of the rest, streamed as fragments that no board has to hold whole.
/// The tree here has two roots, so a subscription to one shows what the other leaves out; the loopback test reads the fragmented WebSocket message off a real socket.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"
#include "http_test_client.h"
#include "platform/platform.h"

#include <algorithm> // std::max
#include <cstdio>    // std::snprintf, the wide module's control names
#include <cstring>
#include <string>    // std::string: named explicitly, GCC does not pull it in transitively
#include <vector>

namespace {

struct Knob : public mm::MoonModule {
    uint8_t value = 10;
    void defineControls() override { controls_.addControl("value", value, 0, 100); }
};

// Forty controls, enough to need several fragments at the stream's fragment size.
struct Wide : public mm::MoonModule {
    static constexpr int kCount = 40;
    uint8_t values[kCount] = {};
    void defineControls() override {
        static char names[kCount][4];
        for (int i = 0; i < kCount; i++) {
            std::snprintf(names[i], sizeof(names[i]), "c%02d", i);
            controls_.addControl(names[i], values[i]);
        }
    }
};

struct Box : public mm::MoonModule {};

// Root A holds K; root B holds L and the wide module W.
struct Tree {
    mm::Scheduler s;
    Box* a = new Box();
    Box* b = new Box();
    Knob* k = new Knob();
    Knob* l = new Knob();
    Wide* w = new Wide();
    Tree() {
        a->setName("A"); b->setName("B"); k->setName("K"); l->setName("L"); w->setName("W");
        a->addChild(k);
        b->addChild(l);
        b->addChild(w);
        s.addModule(a);
        s.addModule(b);
        s.setup();
    }
    ~Tree() { mm::Scheduler::deleteTree(a); mm::Scheduler::deleteTree(b); }
};

std::string whole(mm::HttpServerModule& http, const mm::HttpServerModule::Subscription* sub) {
    mm::HttpServerModule::TreeCursor c;
    c.sub = sub;
    mm::JsonSink sink;
    http.writeTreeFragment(sink, c, 0);
    return std::string(sink.data(), sink.size());
}

// The JSON object a module starts, up to the first closing brace after its name: enough to see whether controls ride with it.
std::string objectOf(const std::string& doc, const char* name) {
    const std::string key = std::string("{\"name\":\"") + name + "\"";
    const size_t at = doc.find(key);
    if (at == std::string::npos) return "";
    return doc.substr(at, doc.find("\"children\"", at) == std::string::npos ? std::string::npos : doc.find("\"children\"", at) - at);
}

mm::HttpServerModule::Subscription subscribed(const char* body) {
    mm::HttpServerModule::Subscription sub;
    sub.parse(body);
    return sub;
}

}  // namespace

TEST_CASE("a full state written in fragments of any size equals the state written at once") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    const std::string once = whole(http, nullptr);
    REQUIRE(once.rfind("{\"modules\":[", 0) == 0);
    for (size_t target : {size_t(1), size_t(7), size_t(64), size_t(2048)}) {
        mm::HttpServerModule::TreeCursor c;
        std::string joined;
        int fragments = 0;
        bool done = false;
        while (!done && fragments < 100000) {
            mm::JsonSink sink;
            done = http.writeTreeFragment(sink, c, target);
            joined.append(sink.data(), sink.size());
            fragments++;
        }
        CHECK(joined == once);
        if (target < once.size()) CHECK(fragments > 1);
    }
}

namespace {
// A select, a palette and a list whose options and rows run to many kilobytes, as Drivers' palette and the fixture profile list do.
struct Lots : public mm::MoonModule, public mm::ListSource {
    static constexpr int kOptions = 200;
    uint8_t listRowCount() const override { return 60; }
    void writeListRow(mm::JsonSink& sink, uint8_t row) const override { sink.appendf("{\"id\":%u,\"name\":\"profile %02u\"}", row, row); }
    void writeListRowDetail(mm::JsonSink& sink, uint8_t row) const override {
        sink.appendf("{\"channels\":\"red green blue white amber uv dimmer strobe pan tilt\",\"row\":%u}", row);
    }
    void writeListOptionSets(mm::JsonSink& sink) const override { sink.append("\"kinds\":[\"rgb\",\"rgbw\"]"); }
    uint8_t pick = 0, palette = 0;
    static void paletteOptions(mm::JsonSink& sink) {
        const auto one = [&](int i) { sink.appendf("%s{\"name\":\"palette %03d\",\"colors\":\"ff0000 00ff00 0000ff ffffff\"}", i > 0 ? "," : "", i); };
        if (sink.optionIndex() >= 0) { if (sink.optionIndex() < kOptions) one(sink.optionIndex()); return; }
        for (int i = 0; i < kOptions; i++) one(i);
    }
    void defineControls() override {
        static char names[kOptions][40];
        static const char* options[kOptions];
        for (int i = 0; i < kOptions; i++) {
            std::snprintf(names[i], sizeof(names[i]), "an option with a long name, number %03d", i);
            options[i] = names[i];
        }
        controls_.addSelect("pick", pick, options, kOptions);
        controls_.addPalette("palette", palette, paletteOptions, kOptions);
        controls_.addList("profiles", *this);
    }
};
}  // namespace

// A classic ESP32's page never got its state: one control of 11 to 17 KB went into a single fragment, which no free block that size could hold.
TEST_CASE("a control larger than a fragment streams a row or an option at a time, and reassembles to the whole") {
    mm::Scheduler s;
    auto* lots = new Lots();
    lots->setName("Lots");
    s.addModule(lots);
    s.setup();
    mm::HttpServerModule http;
    http.setScheduler(&s);
    const std::string once = whole(http, nullptr);
    REQUIRE(once.size() > 4 * 2048);
    for (size_t target : {size_t(1), size_t(64), size_t(2048)}) {
        mm::HttpServerModule::TreeCursor c;
        std::string joined;
        size_t largest = 0;
        bool done = false;
        for (int fragments = 0; !done && fragments < 100000; fragments++) {
            mm::JsonSink sink;
            done = http.writeTreeFragment(sink, c, target);
            joined.append(sink.data(), sink.size());
            largest = std::max(largest, sink.size());
        }
        CHECK(joined == once);
        CHECK(largest < target + 200);   // the target and one option past it, never a whole control
    }
    mm::Scheduler::deleteTree(lots);
}

TEST_CASE("one module's JSON is its entry in the full state, children included") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    mm::HttpServerModule::TreeCursor c;
    c.single = t.b;
    mm::JsonSink sink;
    CHECK(http.writeTreeFragment(sink, c, 0));
    const std::string b(sink.data(), sink.size());
    CHECK(whole(http, nullptr).find(b) != std::string::npos);
}

TEST_CASE("a module outside the subscription is sent as its outline, its shape kept and its controls left out") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    const auto sub = subscribed("{\"subscribe\":[\"A\"]}");
    const std::string doc = whole(http, &sub);
    CHECK(objectOf(doc, "K").find("\"controls\"") != std::string::npos);
    CHECK(objectOf(doc, "B").find("\"controls\"") == std::string::npos);
    CHECK(objectOf(doc, "L").find("\"controls\"") == std::string::npos);
    CHECK(doc.find("{\"name\":\"W\"") != std::string::npos);   // the outline keeps every module
    CHECK(doc.size() < whole(http, nullptr).size());
}

TEST_CASE("a subscribed module deep in the tree brings its own controls and not its parent's") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    const auto sub = subscribed("{\"subscribe\":[\"L\"]}");
    const std::string doc = whole(http, &sub);
    CHECK(objectOf(doc, "L").find("\"controls\"") != std::string::npos);
    CHECK(objectOf(doc, "B").find("\"controls\"") == std::string::npos);
    CHECK(objectOf(doc, "W").find("\"controls\"") == std::string::npos);
}

TEST_CASE("the subscribe message names modules, and a star subscribes to the whole tree") {
    mm::HttpServerModule::Subscription sub;
    CHECK(sub.parse("{\"subscribe\":[\"A\",\"System\"]}"));
    CHECK(sub.count == 2);
    CHECK(!sub.all);
    CHECK(sub.parse("{\"subscribe\":[\"*\"]}"));
    CHECK(sub.all);
    CHECK(sub.count == 0);
    CHECK(!sub.parse("{\"subscribe\":\"A\"}"));   // not an array: no subscription read
    CHECK(sub.parse("{\"subscribe\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\"]}"));
    CHECK(sub.count == mm::HttpServerModule::Subscription::kMaxModules);
    CHECK(sub.parse("{\"subscribe\":[]}"));   // an empty list is the outline alone
    CHECK(sub.count == 0);
}

TEST_CASE("a client is patched only with the modules it subscribes to") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    http.subscribeForTest(0, "{\"subscribe\":[\"A\"]}");
    http.subscribeForTest(1, "{\"subscribe\":[\"B\"]}");
    http.subscribeForTest(2, "{\"subscribe\":[]}");
    http.baselineLeafHashesForTest();
    t.k->value = 70;
    t.l->value = 71;
    mm::JsonSink outs[3];
    http.buildClientPatchesForTest(outs, 3);
    const std::string a(outs[0].data()), b(outs[1].data()), none(outs[2].data());
    CHECK(a.find("\"path\":\"K/value\"") != std::string::npos);
    CHECK(a.find("L/value") == std::string::npos);
    CHECK(b.find("\"path\":\"L/value\"") != std::string::npos);
    CHECK(b.find("K/value") == std::string::npos);
    CHECK(none == "{\"patch\":[]}");
}

TEST_CASE("a subscription entry matches a module's type as well as its name") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    t.l->setTypeName("Dial");
    const auto sub = subscribed("{\"subscribe\":[\"Dial\"]}");
    const std::string doc = whole(http, &sub);
    CHECK(objectOf(doc, "L").find("\"controls\"") != std::string::npos);
    CHECK(objectOf(doc, "K").find("\"controls\"") == std::string::npos);
}

TEST_CASE("a subscription change resyncs that client alone, and the same subscription again does not") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    http.clearFullResyncForTest();
    http.subscribeForTest(0, "{\"subscribe\":[\"A\"]}");
    CHECK(http.clientResyncPendingForTest(0));
    CHECK(!http.clientResyncPendingForTest(1));
    CHECK(!http.fullResyncPendingForTest());
    http.subscribeForTest(1, "{\"subscribe\":[\"A\"]}");
    http.subscribeForTest(1, "{\"subscribe\":[\"A\"]}");
    CHECK(http.clientResyncPendingForTest(1));
}

namespace {

constexpr uint16_t kTestPort = 18092;   // 18090 and 18091 belong to the connections and SD card tests

// One masked client frame, as a browser sends it (RFC 6455 §5.3): text by default, or a control opcode.
std::vector<uint8_t> clientFrame(const char* text, uint8_t opcode = 0x1) {
    const size_t n = std::strlen(text);
    std::vector<uint8_t> f = {static_cast<uint8_t>(0x80 | opcode), static_cast<uint8_t>(0x80 | n), 1, 2, 3, 4};
    for (size_t i = 0; i < n; i++) f.push_back(static_cast<uint8_t>(text[i]) ^ f[2 + (i & 3)]);
    return f;
}

// A WebSocket client over loopback, reading the server's unmasked frames as the server ticks.
struct WsClient {
    mm::HttpServerModule& http;
    mm::platform::TcpConnection conn;
    std::vector<uint8_t> in;

    bool open(uint16_t port) {
        if (!connectAndAccept(http, conn, port)) return false;
        const char* upgrade = "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!conn.write(reinterpret_cast<const uint8_t*>(upgrade), std::strlen(upgrade))) return false;
        std::string handshake;
        for (int i = 0; i < 300 && handshake.find("\r\n\r\n") == std::string::npos; i++) {
            http.tick20ms();
            char buf[512];
            const int n = conn.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
            if (n > 0) handshake.append(buf, n);
            mm::platform::delayMs(1);
        }
        return handshake.rfind("HTTP/1.1 101", 0) == 0;
    }

    bool send(const char* text, uint8_t opcode = 0x1) {
        const auto f = clientFrame(text, opcode);
        return conn.write(f.data(), f.size());
    }

    // The next whole message and the opcode of each of its frames, empty when none arrives within `ticks`.
    std::string message(std::vector<uint8_t>& opcodes, int ticks = 2000) {
        std::string out;
        opcodes.clear();
        for (int i = 0; i < ticks; i++) {
            http.tick20ms();
            uint8_t buf[4096];
            const int n = conn.read(buf, sizeof(buf));
            if (n > 0) in.insert(in.end(), buf, buf + n);
            while (in.size() >= 2) {
                size_t len = in[1] & 0x7F, hdr = 2;
                if (len == 126) { if (in.size() < 4) break; len = (size_t(in[2]) << 8) | in[3]; hdr = 4; }
                if (in.size() < hdr + len) break;
                opcodes.push_back(in[0] & 0x0F);
                const bool fin = (in[0] & 0x80) != 0;
                out.append(reinterpret_cast<const char*>(in.data() + hdr), len);
                in.erase(in.begin(), in.begin() + static_cast<long>(hdr + len));
                if (fin) return out;
            }
            if (n <= 0) mm::platform::delayMs(1);
        }
        return "";
    }
};

// Released however the test ends: a live server left behind keeps the static hooks pointing at it.
struct Served {
    Tree t;
    mm::HttpServerModule http;
    Served() { http.setScheduler(&t.s); http.port = kTestPort; http.setup(); }
    ~Served() { http.release(); }
};

}  // namespace

TEST_CASE("the full state streams as one fragmented message that reassembles to the subscribed document") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("{\"subscribe\":[\"B\"]}"));
    std::vector<uint8_t> opcodes;
    const std::string message = c.message(opcodes);
    REQUIRE(opcodes.size() > 1);   // W's forty controls take more than one fragment
    CHECK(opcodes.front() == 0x1);
    for (size_t i = 1; i < opcodes.size(); i++) CHECK(opcodes[i] == 0x0);
    const auto expected = subscribed("{\"subscribe\":[\"B\"]}");
    CHECK(message == whole(sv.http, &expected));
}

// Below the headroom the stream once waited for room a classic ESP32 never has, so its page got no state. It now takes its turn with the other sends.
TEST_CASE("the state stream still arrives whole while internal RAM is short") {
    struct Heap { ~Heap() { mm::platform::setTestFreeInternalHeap(0); } } heap;   // restored however the test ends
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    mm::platform::setTestFreeInternalHeap(mm::HttpServerModule::kSendHeadroom - 1);
    REQUIRE(c.send("{\"subscribe\":[\"B\"]}"));
    std::vector<uint8_t> opcodes;
    const std::string message = c.message(opcodes);
    const auto expected = subscribed("{\"subscribe\":[\"B\"]}");
    CHECK(message == whole(sv.http, &expected));
}

TEST_CASE("a client that subscribes is sent a full state, and the client already there is not") {
    Served sv;
    WsClient first{sv.http, {}, {}}, second{sv.http, {}, {}};
    REQUIRE(first.open(kTestPort));
    REQUIRE(first.send("{\"subscribe\":[\"A\"]}"));
    std::vector<uint8_t> opcodes;
    REQUIRE(first.message(opcodes).rfind("{\"modules\":[", 0) == 0);

    REQUIRE(second.open(kTestPort));
    REQUIRE(second.send("{\"subscribe\":[\"B\"]}"));
    CHECK(second.message(opcodes).rfind("{\"modules\":[", 0) == 0);
    CHECK(first.message(opcodes, 50).empty());   // no patch is due and no document is owed
}

TEST_CASE("a Ping is answered with a Pong carrying the same payload") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("are you there", 0x9));
    std::vector<uint8_t> opcodes;
    CHECK(c.message(opcodes) == "are you there");
    REQUIRE(opcodes.size() == 1);
    CHECK(opcodes[0] == 0xA);
}

TEST_CASE("a Close is answered with a Close echoing its status code, and the connection ends") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("\x03\xe8" "bye", 0x8));   // 1000, normal closure, and a reason
    std::vector<uint8_t> opcodes;
    CHECK(c.message(opcodes) == "\x03\xe8");
    REQUIRE(opcodes.size() == 1);
    CHECK(opcodes[0] == 0x8);
    uint8_t buf[16];
    int n = -1;
    for (int i = 0; i < 300 && n != 0; i++) { sv.http.tick20ms(); n = c.conn.read(buf, sizeof(buf)); mm::platform::delayMs(1); }
    CHECK(n == 0);   // the server closed its side
}

TEST_CASE("a client asking for its view again gets it, carrying the id it asked with") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("{\"subscribe\":[\"A\"]}"));
    std::vector<uint8_t> opcodes;
    const std::string first = c.message(opcodes);
    CHECK(first.find("\"resync\"") == std::string::npos);   // the subscription's answer carries no id
    REQUIRE(c.send("{\"resync\":7}"));
    const std::string again = c.message(opcodes);
    CHECK(again.rfind("{\"modules\":[", 0) == 0);
    const std::string tail = ",\"resync\":7}";
    REQUIRE(again.size() > tail.size());
    CHECK(again.compare(again.size() - tail.size(), tail.size(), tail) == 0);
}

TEST_CASE("a Ping while the full state streams is answered between frames, and the document stays whole") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("{\"subscribe\":[\"B\"]}"));
    REQUIRE(c.send("mid", 0x9));
    // Every frame read whole: a Pong landing inside a data frame would break the framing the reader walks.
    std::string document, pong;
    bool fin = false;
    std::vector<uint8_t> opcodes;
    for (int i = 0; i < 2000 && (!fin || pong.empty()); i++) {
        sv.http.tick20ms();
        uint8_t buf[4096];
        const int n = c.conn.read(buf, sizeof(buf));
        if (n > 0) c.in.insert(c.in.end(), buf, buf + n);
        while (c.in.size() >= 2) {
            size_t len = c.in[1] & 0x7F, hdr = 2;
            if (len == 126) { if (c.in.size() < 4) break; len = (size_t(c.in[2]) << 8) | c.in[3]; hdr = 4; }
            if (c.in.size() < hdr + len) break;
            const uint8_t op = c.in[0] & 0x0F;
            const std::string body(reinterpret_cast<const char*>(c.in.data() + hdr), len);
            if (op == 0xA) pong = body;
            else { opcodes.push_back(op); document += body; if (c.in[0] & 0x80) fin = true; }
            c.in.erase(c.in.begin(), c.in.begin() + static_cast<long>(hdr + len));
        }
        if (n <= 0) mm::platform::delayMs(1);
    }
    CHECK(pong == "mid");
    REQUIRE(fin);
    const auto expected = subscribed("{\"subscribe\":[\"B\"]}");
    CHECK(document == whole(sv.http, &expected));
}

TEST_CASE("a tree changed between fragments still ends the document in valid JSON") {
    Tree t;
    mm::HttpServerModule http;
    http.setScheduler(&t.s);
    mm::HttpServerModule::TreeCursor c;
    std::string joined;
    bool done = false;
    int fragments = 0;
    while (!done && fragments < 100000) {
        mm::JsonSink sink;
        done = http.writeTreeFragment(sink, c, 16);
        joined.append(sink.data(), sink.size());
        // Mid-way through B, its wide child goes: the cursor's path now names a module that is not there.
        if (++fragments == 40) { t.b->removeChild(t.w); delete t.w; t.w = nullptr; }
    }
    REQUIRE(done);
    mm::json::JsonDoc doc;
    CHECK(mm::json::parse(joined.c_str(), doc));
}

TEST_CASE("patches and the WLED frame wait while a client's document is under way") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    REQUIRE(c.send("{\"subscribe\":[\"A\"]}"));
    std::vector<uint8_t> opcodes;
    REQUIRE(c.message(opcodes).rfind("{\"modules\":[", 0) == 0);
    sv.http.holdStreamForTest(0);   // a document under way, as a slow client's would be
    sv.t.k->value = 99;
    sv.http.tick1s();               // the per-second push: the patch and the WLED frame both due
    uint8_t buf[256];
    mm::platform::delayMs(20);
    CHECK(c.conn.read(buf, sizeof(buf)) == -1);   // nothing written into the open message
}

TEST_CASE("a client that never subscribes is sent the WLED frame and no module state") {
    Served sv;
    WsClient c{sv.http, {}, {}};
    REQUIRE(c.open(kTestPort));
    for (int i = 0; i < 3; i++) { sv.http.tick20ms(); sv.http.tick1s(); }
    std::vector<uint8_t> opcodes;
    const std::string first = c.message(opcodes, 200);
    REQUIRE(!first.empty());
    CHECK(first.rfind("{\"state\":", 0) == 0);
    CHECK(first.find("\"modules\"") == std::string::npos);
    CHECK(first.find("\"patch\"") == std::string::npos);
}

