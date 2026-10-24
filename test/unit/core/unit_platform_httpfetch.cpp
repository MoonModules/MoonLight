/// @module platform

/// Pins the one HTTP client every outbound request shares: a request goes out with its method and body, and its answer comes back. An answer too big for the caller is refused rather than cut. An HTTPS request waits while a board without PSRAM has no internal RAM for a TLS session.

#include "doctest.h"
#include "platform/platform.h"

#include <cstring>
#include <string>

using namespace mm;

namespace {
/// What the stand-in saw last, so a test can say what went out.
std::string lastMethod, lastUrl, lastBody;
int calls = 0;

/// A stand-in answering a canned document, as a weather service would.
int standIn(const char* method, const char* url, const char* reqBody, char* body, size_t bodyLen) {
    calls++;
    lastMethod = method;
    lastUrl = url;
    lastBody = reqBody ? reqBody : "";
    const char* answer = R"({"current":{"temperature_2m":12.3}})";
    if (body && bodyLen) {
        if (std::strlen(answer) + 1 > bodyLen) return 0;
        std::strcpy(body, answer);
    }
    return 200;
}

/// Installs the stand-in for one test and restores the network and the heap figure after it.
struct StandIn {
    StandIn() { calls = 0; platform::setTestHttpFetch(&standIn); }
    ~StandIn() { platform::setTestHttpFetch(nullptr); platform::setTestFreeInternalHeap(0); }
};
}  // namespace

TEST_CASE("a fetch hands back the status and the answer's body") {
    StandIn s;
    char body[128];
    CHECK(platform::httpFetch("GET", "https://api.example/v1?x=1", nullptr, 1000, body, sizeof(body)) == 200);
    CHECK(std::string(body) == R"({"current":{"temperature_2m":12.3}})");
    CHECK(lastMethod == "GET");
    CHECK(lastUrl == "https://api.example/v1?x=1");
}

TEST_CASE("an answer too big for the caller fails rather than arriving cut") {
    StandIn s;
    char body[8];
    CHECK(platform::httpFetch("GET", "https://api.example/v1", nullptr, 1000, body, sizeof(body)) == 0);
    CHECK(body[0] == '\0');
}

TEST_CASE("a POST carries its body, and one that keeps no answer still answers its status") {
    StandIn s;
    CHECK(platform::httpFetch("POST", "https://cloud.example/event", R"({"e":1})", 1000, nullptr, 0) == 200);
    CHECK(lastMethod == "POST");
    CHECK(lastBody == R"({"e":1})");
}

TEST_CASE("an HTTPS request waits while internal RAM cannot hold a TLS session") {
    StandIn s;
    // A board without PSRAM under load: the floor plus one session is not there.
    platform::setTestFreeInternalHeap(platform::kInternalFreeFloor + platform::kTlsSessionBytes - 1);
    CHECK_FALSE(platform::tlsRoom("https://api.example/"));
    CHECK(platform::httpFetch("GET", "https://api.example/", nullptr, 1000, nullptr, 0) == 0);
    CHECK(platform::httpFetch("POST", "https://cloud.example/event", "{}", 1000, nullptr, 0) == 0);
    CHECK(calls == 0);   // nothing went out
    // Cleartext needs no TLS session, so a LAN request still goes.
    CHECK(platform::tlsRoom("http://192.168.1.50/api/state"));
    CHECK(platform::httpFetch("PATCH", "http://192.168.1.50/api/state", "{}", 1000, nullptr, 0) == 200);
    // Room again: the next request goes.
    platform::setTestFreeInternalHeap(platform::kInternalFreeFloor + platform::kTlsSessionBytes);
    CHECK(platform::httpFetch("GET", "https://api.example/", nullptr, 1000, nullptr, 0) == 200);
}

TEST_CASE("a fetch with no method or URL fails cleanly") {
    StandIn s;
    char body[4] = "x";
    CHECK(platform::httpFetch(nullptr, "https://a/", nullptr, 1000, body, sizeof(body)) == 0);
    CHECK(platform::httpFetch("GET", "", nullptr, 1000, body, sizeof(body)) == 0);
    CHECK(body[0] == '\0');
    CHECK(calls == 0);
}
