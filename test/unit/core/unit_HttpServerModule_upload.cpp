/// @module HttpServerModule
/// @also FileManagerModule
///
/// An upload to flash and a download from it move a bounded slice per tick, so a slow client never holds the render loop; driven over loopback.

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "platform/platform.h"
#include "http_loopback.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kTestPort = 18093;   // 18090 to 18092 belong to the connections, SD card and subscription tests
using namespace http_loopback;


// Sends a POST header declaring `declared` body bytes to `path`, then the first `body` bytes.
void postStart(mm::platform::TcpConnection& client, const char* path, size_t declared, const std::string& body) {
    char req[256];
    const int reqLen = std::snprintf(req, sizeof(req),
        "POST /api/file?path=%s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", path, declared);
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), static_cast<size_t>(reqLen)));
    if (!body.empty()) REQUIRE(client.write(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
}




// A fresh flash root under a temp dir for one test.
struct FlashFixture {
    char root[256];
    FlashFixture() {
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "%s/mm_upload_test_%u",
                      std::filesystem::temp_directory_path().string().c_str(), counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        mm::platform::fsSetRoot(root);
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~FlashFixture() noexcept {
        mm::platform::fsSetRoot("");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("a flash upload larger than one chunk streams across ticks and lands whole, answered once it commits") {
    FlashFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    const std::string payload = pattern(20000);
    postStart(client, "/big.bin", payload.size(), payload);
    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 200", 12) == 0);

    char back[20001];   // fsRead null-terminates
    CHECK(mm::platform::fsRead("/big.bin", back, sizeof(back)) == static_cast<int>(payload.size()));
    CHECK(std::memcmp(back, payload.data(), payload.size()) == 0);

    client.close();
    http.release();
}

// The bug this pins: a flash upload drained its whole body in one tick, so a slow client held the render loop past the 12 s watchdog and rebooted the board.
TEST_CASE("a flash upload whose client stalls never holds a tick, and once the stall guard trips leaves no file and frees its slot") {
    FlashFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    postStart(client, "/stuck.bin", 10000, pattern(100));   // declares 10000 bytes, sends 100, then goes silent

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) {
        http.tick20ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    CHECK(elapsed.count() < 1000);   // twenty ticks with a silent client: nothing waits on it

    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(mm::platform::millis() + 60000);   // well past the stall guard
    for (int i = 0; i < 10; i++) http.tick20ms();
    CHECK(readAfterClose(client) == 0);                    // closed, no response
    CHECK(mm::platform::fsSize("/stuck.bin") < 0);         // the partial file never committed

    mm::platform::TcpConnection client2;
    CHECK(postSmall(http, client2, kTestPort, "/ok.bin", "hi").rfind("HTTP/1.1 200", 0) == 0);   // the slot is free again

    client2.close();
    http.release();
}

// A flash download once wrote its whole file in one call, so a client that stopped reading held the render loop until the write gave up.
TEST_CASE("a flash download whose client stops reading never holds a tick, and the stall guard closes it") {
    FlashFixture fix;
    const std::string big = pattern(16u << 20);   // more than the loopback socket buffers hold
    REQUIRE(mm::platform::fsWriteAtomic("/big.bin", big.data(), big.size()));
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));
    const char* req = "GET /api/file?path=/big.bin HTTP/1.1\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));

    long long slowestMs = 0;
    for (int i = 0; i < 20; i++) {
        const auto t0 = std::chrono::steady_clock::now();
        http.tick20ms();
        slowestMs = std::max<long long>(slowestMs, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(slowestMs < 50);   // the reader takes nothing: each tick writes what the socket holds and moves on

    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(mm::platform::millis() + 60000);   // well past the stall guard
    for (int i = 0; i < 5; i++) http.tick20ms();
    // The client finds what the socket held, then the close: never the whole file.
    size_t got = 0;
    int n = -1;
    static char buf[65536];
    for (int ms = 0; ms < 5000 && n != 0; ) {
        n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) got += static_cast<size_t>(n);
        else if (n < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); ms++; }
    }
    CHECK(n == 0);
    CHECK(got < big.size());

    client.close();
    http.release();
}

TEST_CASE("a flash upload over the flash size bound is refused with 413 before any byte lands") {
    FlashFixture fix;
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(http, client, kTestPort));

    postStart(client, "/huge.bin", 300 * 1024, "");
    const std::string reply = pumpUntil(http, client, 12);
    CHECK(std::strncmp(reply.c_str(), "HTTP/1.1 413", 12) == 0);
    CHECK(mm::platform::fsSize("/huge.bin") < 0);

    client.close();
    http.release();
}

// A 16 MB /cfg.json, more than the loopback socket buffers hold, downloaded by a reader that reads nothing, so the download stays open, and an upload of "new" over it.
struct HeldDownload {
    const std::string old = pattern(16u << 20);
    const char* get = "GET /api/file?path=/cfg.json HTTP/1.1\r\n\r\n";
    mm::HttpServerModule http;
    mm::platform::TcpConnection reader, writer;
    HeldDownload() {
        REQUIRE(mm::platform::fsWriteAtomic("/cfg.json", old.data(), old.size()));
        http.port = kTestPort;
        http.setup();
        REQUIRE(connectAndAccept(http, reader, kTestPort));
        REQUIRE(reader.write(reinterpret_cast<const uint8_t*>(get), std::strlen(get)));
        for (int i = 0; i < 5; i++) http.tick20ms();   // the download starts and stalls
        REQUIRE(connectAndAccept(http, writer, kTestPort));
        postStart(writer, "/cfg.json", 3, "new");
    }
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~HeldDownload() noexcept {
        reader.close();
        writer.close();
        http.release();
    }
};

// The bug this pins: LittleFS cannot replace an open file, so a Restore writing a config file another client was downloading failed with 500 and lost its settings.
TEST_CASE("a file being downloaded is replaced once its reader is done, and a new read meanwhile answers 503") {
    FlashFixture fix;
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);   // frozen at an even millisecond, where a wait start of `now | 1` lay ahead of now and gave up at once
    HeldDownload held;
    const std::string early = pumpUntil(held.http, held.writer, 12, 20);
    CHECK_MESSAGE(early.empty(), early);   // no answer while the file is open, where it answered 500

    mm::platform::TcpConnection late;
    REQUIRE(connectAndAccept(held.http, late, kTestPort));
    REQUIRE(late.write(reinterpret_cast<const uint8_t*>(held.get), std::strlen(held.get)));
    CHECK(pumpUntil(held.http, late, 12).rfind("HTTP/1.1 503", 0) == 0);   // a new read waits for the write, so readers cannot starve it

    held.reader.close();   // the download ends, and the write commits
    CHECK(pumpUntil(held.http, held.writer, 12).rfind("HTTP/1.1 200", 0) == 0);
    char back[8] = {};
    CHECK(mm::platform::fsRead("/cfg.json", back, sizeof(back)) == 3);
    CHECK(std::strcmp(back, "new") == 0);
    late.close();
}

// A reader still reading after a whole stall window does not hold an upload forever: the upload answers 503, its client retries, and the file stays as it was.
TEST_CASE("an upload waiting a whole stall window for a slow reader answers 503 and leaves the file as it was") {
    FlashFixture fix;
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);
    HeldDownload held;
    std::string reply;
    static char buf[4096];
    for (int second = 1; second <= 12 && reply.empty(); second++) {
        held.reader.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));   // a little each second: the download progresses, so its own stall guard stays quiet
        mm::platform::setTestNowMs(1000 + second * 1000);
        for (int i = 0; i < 3; i++) { mm::platform::delayMs(1); held.http.tick20ms(); }
        reply = pumpUntil(held.http, held.writer, 12, 3);
    }
    CHECK(reply.rfind("HTTP/1.1 503", 0) == 0);
    char head[4] = {};
    CHECK(mm::platform::fsRead("/cfg.json", head, sizeof(head)) > 0);
    CHECK(std::memcmp(head, held.old.data(), 3) == 0);   // the old file, whole: nothing half-committed
}
