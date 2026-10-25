/// BLE request framing, backpressure and rejected-input recovery.
#include "doctest.h"
#include "core/util/BleMessage.h"
#include <algorithm>
#include <string>

TEST_CASE("BLE requests assemble across the minimum ATT payload without partial JSON writes") {
    mm::BleMessage message;
    const std::string line = R"({"$v":1,"$id":1,"$op":"get"})" "\n";
    char out[513];
    for (size_t i = 0; i < line.size(); i += 20) {
        const size_t count = std::min(size_t{20}, line.size() - i);
        const auto result = message.feed(reinterpret_cast<const uint8_t*>(line.data() + i), count);
        CHECK(result == (i + count == line.size() ? mm::BleMessage::Result::Ready : mm::BleMessage::Result::Partial));
        if (i + count != line.size()) CHECK(message.take(out, sizeof(out)) == 0);
    }
    CHECK(message.take(out, sizeof(out)) == line.size() - 1);
    CHECK(std::string(out) == line.substr(0, line.size() - 1));
}

TEST_CASE("BLE backpressure keeps an acknowledged request until the scheduler takes it") {
    mm::BleMessage message;
    const uint8_t bytes[] = {'{','}','\n'};
    REQUIRE(message.feed(bytes, sizeof(bytes)) == mm::BleMessage::Result::Ready);
    CHECK(message.feed(bytes, sizeof(bytes)) == mm::BleMessage::Result::Busy);
    char small[2], out[8];
    CHECK(message.take(small, sizeof(small)) == 0);
    REQUIRE(message.take(out, sizeof(out)) == 2);
    CHECK(std::string(out) == "{}");
    CHECK(message.take(out, sizeof(out)) == 0);
}

TEST_CASE("BLE oversized and NUL-containing lines cannot leak a tail into the next request") {
    mm::BleMessage message;
    const std::string exact = std::string(512, 'x') + '\n';
    CHECK(message.feed(reinterpret_cast<const uint8_t*>(exact.data()), exact.size()) == mm::BleMessage::Result::Ready);
    char exactOut[513];
    CHECK(message.take(exactOut, sizeof(exactOut)) == 512);
    std::string oversized(514, 'x'); oversized += '\n';
    CHECK(message.feed(reinterpret_cast<const uint8_t*>(oversized.data()), oversized.size()) == mm::BleMessage::Result::Invalid);
    const uint8_t bad[] = {'{',0,'}','\n'};
    CHECK(message.feed(bad, sizeof(bad)) == mm::BleMessage::Result::Invalid);
    const uint8_t good[] = {'{','}','\r','\n'};
    CHECK(message.feed(good, sizeof(good)) == mm::BleMessage::Result::Ready);
    char out[513]; REQUIRE(message.take(out, sizeof(out)) == 2);
    CHECK(std::string(out) == "{}");
}

TEST_CASE("BLE disconnect reset drops partial JSON and a batched write is refused whole") {
    mm::BleMessage message;
    const uint8_t partial[] = {'{','"','x','"',':'};
    REQUIRE(message.feed(partial, sizeof(partial)) == mm::BleMessage::Result::Partial);
    message.reset();
    const std::string batch = "{}\n{}\n";
    CHECK(message.feed(reinterpret_cast<const uint8_t*>(batch.data()), batch.size()) == mm::BleMessage::Result::Invalid);
    char out[513]; CHECK(message.take(out, sizeof(out)) == 0);
}
