/// @module FirmwareUpdateModule

/// The WiFi co-processor's status line reads the link first, and asks the chip for its firmware version only over a link that is up.

#include "doctest.h"
#include "platform/coprocessor_status.h"

#include <cstring>

using mm::platform::CoprocessorLink;
using mm::platform::CoprocessorStatus;
using mm::platform::CoprocessorVersion;

namespace {

constexpr CoprocessorLink kNotStarted = CoprocessorLink::NotStarted;
constexpr CoprocessorLink kDown = CoprocessorLink::Down;
constexpr CoprocessorLink kUp = CoprocessorLink::Up;

/// A version query that counts its calls and gives the answer it holds.
struct Query {
    CoprocessorVersion answer;
    int calls = 0;
    CoprocessorVersion operator()() { calls++; return answer; }
};

CoprocessorVersion version(unsigned major, unsigned minor, unsigned patch) {
    CoprocessorVersion v;
    v.answered = true;
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    return v;
}

}  // namespace

TEST_CASE("The co-processor line reads link not started while another interface carries the device, and holds its query") {
    CoprocessorStatus status;
    Query query{version(2, 12, 13)};
    CHECK(std::strcmp(status.line(), "link not started") == 0);
    for (int i = 0; i < 5; i++) CHECK(std::strcmp(status.update(kNotStarted, [&] { return query(); }), "link not started") == 0);
    CHECK(query.calls == 0);
}

TEST_CASE("The co-processor line reads link down, and holds its query, while a started link is not up") {
    CoprocessorStatus status;
    Query query{version(2, 12, 13)};
    for (int i = 0; i < 5; i++) CHECK(std::strcmp(status.update(kDown, [&] { return query(); }), "link down") == 0);
    CHECK(query.calls == 0);
}

TEST_CASE("The co-processor line shows the version the chip reports once the link is up, asked once") {
    CoprocessorStatus status;
    Query query{version(2, 12, 13)};
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 fw 2.12.13") == 0);
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 fw 2.12.13") == 0);
    CHECK(query.calls == 1);
}

TEST_CASE("Time before the handshake spends no attempt, so a link that comes up late still gets its two queries") {
    CoprocessorStatus status;
    Query query;   // unanswered
    for (int i = 0; i < 5; i++) status.update(kNotStarted, [&] { return query(); });
    for (int i = 0; i < 5; i++) status.update(kDown, [&] { return query(); });
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "querying…") == 0);
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 too old to report") == 0);
    CHECK(query.calls == 2);
}

TEST_CASE("Two unanswered queries over a working link read as firmware too old to report, and the line stops asking") {
    CoprocessorStatus status;
    Query query;   // unanswered
    status.update(kUp, [&] { return query(); });
    status.update(kUp, [&] { return query(); });
    for (int i = 0; i < 5; i++) CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 too old to report") == 0);
    CHECK(query.calls == 2);
}

TEST_CASE("A chip that answers 0.0.0 counts as unanswered, as firmware before the version query does") {
    CoprocessorStatus status;
    Query query{version(0, 0, 0)};
    status.update(kUp, [&] { return query(); });
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 too old to report") == 0);
}

TEST_CASE("A link that drops reads link down, and shows the version it learned when it returns, asked once") {
    CoprocessorStatus status;
    Query query{version(2, 12, 13)};
    status.update(kUp, [&] { return query(); });
    CHECK(std::strcmp(status.update(kDown, [&] { return query(); }), "link down") == 0);
    CHECK(std::strcmp(status.update(kUp, [&] { return query(); }), "C6 fw 2.12.13") == 0);
    CHECK(query.calls == 1);
}

TEST_CASE("The line stays in one buffer, so a control bound to it follows every update") {
    CoprocessorStatus status;
    Query query{version(2, 12, 13)};
    const char* bound = status.line();
    CHECK(status.update(kNotStarted, [&] { return query(); }) == bound);
    CHECK(status.update(kDown, [&] { return query(); }) == bound);
    CHECK(status.update(kUp, [&] { return query(); }) == bound);
    CHECK(std::strcmp(bound, "C6 fw 2.12.13") == 0);
}

TEST_CASE("The chip is outdated once settled on another major.minor than the host's, or on no version at all, and not before") {
    CoprocessorStatus matching;
    Query current{version(2, 12, 13)};
    CHECK_FALSE(matching.outdated(2, 12));   // before any query
    matching.update(kUp, [&] { return current(); });
    CHECK_FALSE(matching.outdated(2, 12));
    CHECK(matching.outdated(2, 13));

    CoprocessorStatus old;
    Query silent;   // unanswered
    old.update(kUp, [&] { return silent(); });
    CHECK_FALSE(old.outdated(2, 12));        // one attempt left
    old.update(kUp, [&] { return silent(); });
    CHECK(old.outdated(2, 12));
}
