/// @module HttpServerModule

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "core/module/Scheduler.h"
#include "core/util/JsonSink.h"
#include "platform/platform_heap.h"   // setTestAllocRefusedFrom

#include <cstring>

// A long list, MoonShine's graph among them, written whole in one tick held a classic ESP32's or an S3's frame for 10 ms once a second; the walk writes it a row per slice instead.
TEST_CASE("the second's walk writes a list a row at a time over several ticks and patches it whole once") {
    struct Rows final : mm::MoonModule, mm::ListSource {
        int base = 0;
        mm::ListRow listRowCount() const override { return 5; }
        void writeListRow(mm::JsonSink& sink, mm::ListRow r) const override { sink.appendf("{\"n\":%d}", base + r); }
        void writeListRowDetail(mm::JsonSink& sink, mm::ListRow) const override { sink.append("{}"); }
        void defineControls() override { controls_.addList("rows", *this); }
    };
    mm::Scheduler s;
    auto* rows = new Rows();
    rows->setName("Rows");
    s.addModule(rows);
    s.setup();
    mm::HttpServerModule http;
    http.setScheduler(&s);
    http.subscribeForTest(0, "{\"subscribe\":[\"*\"]}");
    http.baselineLeafHashesForTest();
    rows->base = 10;
    http.startValueWalkForTest();
    int steps = 1;
    bool partSeen = false;
    while (!http.stepValueWalkForTest(1) && steps < 100) {
        steps++;
        mm::JsonSink sofar;
        http.walkPatchForTest(sofar);
        partSeen |= std::strstr(sofar.data(), "Rows/rows") != nullptr;
    }
    CHECK(steps == 4 + 5);         // a step per header leaf, then a step per row
    CHECK_FALSE(partSeen);         // a part-written list stays out of the patch
    mm::JsonSink patch;
    CHECK(http.walkPatchForTest(patch) == 1);
    CHECK(std::strstr(patch.data(), "{\"path\":\"Rows/rows\",\"value\":[{\"n\":10},{\"n\":11},{\"n\":12},{\"n\":13},{\"n\":14}],\"detail\":[{},{},{},{},{}]}") != nullptr);
    // The row-at-a-time list is the one the full state writes, so its hash meets the baseline's and an unchanged list goes out once.
    mm::JsonSink whole;
    mm::writeControlValue(whole, rows->controls()[0]);
    mm::writeListDetail(whole, rows->controls()[0]);
    CHECK(std::strstr(patch.data(), whole.data()) != nullptr);
    http.release();
    s.deleteTree(rows);
}

namespace {
// A module with one list of five rows, each `{"n":base+row}`, padded to `pad` characters so a row can outgrow the walk's small buffer.
struct PaddedRows final : mm::MoonModule, mm::ListSource {
    int base = 0;
    int pad = 0;
    mm::ListRow listRowCount() const override { return 5; }
    void writeListRow(mm::JsonSink& sink, mm::ListRow r) const override { sink.appendf("{\"n\":%d,\"p\":\"%*s\"}", base + r, pad, ""); }
    void writeListRowDetail(mm::JsonSink& sink, mm::ListRow) const override { sink.append("{}"); }
    void defineControls() override { controls_.addList("rows", *this); }
};
}  // namespace

// A walk that stops inside one module's list and then skips that module, as when its last viewer looks elsewhere, writes the next list from its own first row.
TEST_CASE("the second's walk starts a list afresh when the list it was writing drops out of view mid-walk") {
    mm::Scheduler s;
    auto* a = new PaddedRows();
    a->setName("A");
    auto* b = new PaddedRows();
    b->setName("B");
    s.addModule(a);
    s.addModule(b);
    s.setup();
    mm::HttpServerModule http;
    http.setScheduler(&s);
    http.subscribeForTest(0, "{\"subscribe\":[\"B\"]}");
    http.subscribeForTest(1, "{\"subscribe\":[\"A\"]}");
    http.baselineLeafHashesForTest();
    a->base = 10;
    b->base = 20;
    http.startValueWalkForTest();
    for (int i = 0; i < 6; i++) http.stepValueWalkForTest(1);   // A's four header leaves and two of its rows
    http.subscribeForTest(1, "{\"subscribe\":[\"B\"]}");          // A's last viewer looks elsewhere
    for (int i = 0; i < 100 && !http.stepValueWalkForTest(1); i++) {}
    mm::JsonSink patch;
    http.walkPatchForTest(patch);
    CHECK(std::strstr(patch.data(), "{\"path\":\"B/rows\",\"value\":[{\"n\":20,") != nullptr);   // B's list opens with its own first row
    CHECK(std::strstr(patch.data(), "{\"n\":10,") == nullptr);                                  // and carries none of A's
    http.release();
    s.deleteTree(a);
    s.deleteTree(b);
}

// A value the heap cannot hold is cut short, and a cut value is invalid JSON that a page drops whole; the leaf goes out only whole.
TEST_CASE("a leaf value cut short by memory stays out of the patch") {
    struct Refuse { ~Refuse() { mm::platform::setTestAllocRefusedFrom(0); } } refuse;
    mm::Scheduler s;
    auto* rows = new PaddedRows();
    rows->setName("Rows");
    rows->pad = 200;   // past the walk's small buffer, so the value needs a block of its own
    s.addModule(rows);
    s.setup();
    mm::HttpServerModule http;
    http.setScheduler(&s);
    http.subscribeForTest(0, "{\"subscribe\":[\"*\"]}");
    http.baselineLeafHashesForTest();
    rows->base = 5;
    mm::platform::setTestAllocRefusedFrom(1024);   // the value's block is refused
    mm::JsonSink none;
    CHECK(http.buildStatePatchForTest(none) == 0);
    http.release();
    s.deleteTree(rows);
}
