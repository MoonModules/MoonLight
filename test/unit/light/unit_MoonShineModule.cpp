/// @module MoonShineModule

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/util/JsonSink.h"
#include "platform/platform_clock.h"   // millis
#include "platform/platform_tasks.h"   // TaskInfo, setTestTaskSnapshot
#include "core/util/ModuleFactory.h"
#include "light/layers/MoonShineModule.h"
#include "module_types.h"

#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

// Layouts, Effects and Drivers as the app builds them, with two layers (the first holds an effect and a modifier, the second an effect) and one driver, then the Pipeline beside them.
struct PipelineTree {
    Scheduler s;
    MoonShineModule* pipeline = nullptr;
    PipelineTree() {
        registerModuleTypes();
        for (const char* top : {"Layouts", "Effects", "Drivers"}) s.addModule(createTopLevel(top));
        pipeline = static_cast<MoonShineModule*>(ModuleFactory::create("MoonShineModule"));
        pipeline->setName("MoonShine");
        s.addModule(pipeline);
        s.setup();
        applyStateDocument(s, R"({"Layouts":{"Grid":{"type":"GridLayout","width":8,"height":4}}})");
        applyStateDocument(s, R"({"Effects":{"A":{"type":"Layer","E1":{"type":"PulseEffect"},"M1":{"type":"CheckerboardModifier"}},"B":{"type":"Layer","E2":{"type":"PulseEffect"}}}})");
        applyStateDocument(s, R"({"Drivers":{"D1":{"type":"PreviewDriver"}}})");
        s.prepareTree();
    }
    ~PipelineTree() { s.release(); }
    std::string row(ListRow i) const {
        JsonSink sink;
        pipeline->writeListRow(sink, i);
        return sink.data();
    }
    ListRow rows() const { return pipeline->listRowCount(); }
    // The first row whose name is `name`, or an empty string.
    std::string named(const char* name) const {
        const std::string key = std::string("\"name\":\"") + name + "\"";
        for (ListRow i = 0; i < rows(); i++) if (row(i).find(key) != std::string::npos) return row(i);
        return "";
    }
};

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("MoonShineModule: the rows run layout, effects, modifiers, layer per layer, then Drivers and its drivers") {
    PipelineTree t;
    CHECK(has(t.named("Grid"), "\"stage\":0"));
    CHECK(has(t.named("Grid"), "\"own\":"));
    CHECK(has(t.named("E1"), "\"stage\":1"));
    CHECK(has(t.named("M1"), "\"stage\":2"));
    CHECK(has(t.named("A"), "\"stage\":3"));
    CHECK(has(t.named("E1"), "\"group\":\"A\""));
    CHECK(has(t.named("A"), "\"group\":\"A\""));
    CHECK(has(t.named("Drivers"), "\"stage\":4"));
    CHECK_FALSE(has(t.named("Drivers"), "\"group\""));
    CHECK(has(t.named("D1"), "\"stage\":5"));
    // Within a layer the effects come before the modifiers, and the layer closes its group.
    ListRow e1 = 0, m1 = 0, a = 0;
    for (ListRow i = 0; i < t.rows(); i++) {
        if (has(t.row(i), "\"name\":\"E1\"")) e1 = i;
        if (has(t.row(i), "\"name\":\"M1\"")) m1 = i;
        if (has(t.row(i), "\"name\":\"A\"")) a = i;
    }
    CHECK(e1 < m1);
    CHECK(m1 < a);
}

TEST_CASE("MoonShineModule: a layer's wire to Drivers carries the layer buffer's bytes") {
    PipelineTree t;
    const size_t bytes = static_cast<Layer*>(t.s.firstByName("A"))->buffer().bytes();
    REQUIRE(bytes == 8u * 4u * 3u);
    CHECK(has(t.named("A"), ("\"edges\":[{\"to\":\"Drivers\",\"bytes\":" + std::to_string(bytes) + "}]").c_str()));
}

TEST_CASE("MoonShineModule: an effect's wire goes to the layer's modifier, else to the layer") {
    PipelineTree t;
    CHECK(has(t.named("E1"), "\"to\":\"M1\""));
    CHECK(has(t.named("M1"), "\"to\":\"A\""));
    CHECK(has(t.named("E2"), "\"to\":\"B\""));
}

TEST_CASE("MoonShineModule: a layout's wires reach every layer and carry no bytes") {
    PipelineTree t;
    const std::string g = t.named("Grid");
    CHECK(has(g, "{\"to\":\"A\"}"));
    CHECK(has(g, "{\"to\":\"B\"}"));
}

TEST_CASE("MoonShineModule: a disabled layer stays a row with no wires, nor do its effects") {
    PipelineTree t;
    t.s.firstByName("B")->setEnabled(false);
    CHECK(has(t.named("B"), "\"enabled\":false"));
    CHECK(has(t.named("B"), "\"edges\":[]"));
    CHECK(has(t.named("E2"), "\"edges\":[]"));
    CHECK_FALSE(has(t.named("Grid"), "\"to\":\"B\""));
}

TEST_CASE("MoonShineModule: Drivers wires each enabled driver, the pipeline is derived and draws as a flow") {
    PipelineTree t;
    CHECK(has(t.named("Drivers"), "\"to\":\"D1\""));
    CHECK(t.pipeline->listAsFlow());
    CHECK_FALSE(t.pipeline->persistsList());
    bool found = false;
    for (uint8_t i = 0; i < t.pipeline->controls().count(); i++)
        found = found || std::strcmp(t.pipeline->controls()[i].name, "pipeline") == 0;
    CHECK(found);
}

namespace {
/// A number field of a row, or -1 where the row has none.
long field(const std::string& row, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = row.find(k);
    return at == std::string::npos ? -1 : std::stol(row.substr(at + k.size()));
}
/// The amounts of a bar's parts, in order.
std::vector<long> partsOf(const std::string& row) {
    std::vector<long> out;
    for (size_t at = row.find("\",", row.find("\"parts\":[")); at != std::string::npos && at < row.size(); at = row.find("\",", at + 1)) {
        out.push_back(std::stol(row.substr(at + 2)));
        if (row.compare(row.find(']', at) + 1, 1, "]") == 0) break;   // the last part closes the array
    }
    return out;
}
/// A node's chip on `lane`, 0 where it spends no time there.
long chip(const std::string& row, int lane) {
    const size_t t = row.find("\"time\":[");
    if (t == std::string::npos) return 0;
    const std::string pairs = row.substr(t + 8, row.find("]]", t) == std::string::npos ? 0 : row.find("]]", t) - t - 7);
    const std::string k = "[" + std::to_string(lane) + ",";
    const size_t at = pairs.find(k);
    return at == std::string::npos ? 0 : std::stol(pairs.substr(at + k.size()));
}
long sum(const std::vector<long>& v) { long s = 0; for (long x : v) s += x; return s; }
/// Tick until the scheduler has averaged one timing window.
void timed(PipelineTree& t) {
    const uint32_t start = platform::millis();
    while (t.s.frameUs() == 0 && platform::millis() - start < 1500) t.s.tick();
    REQUIRE(t.s.frameUs() > 0);
}
std::vector<std::string> rowsWhere(const PipelineTree& t, const char* part) {
    std::vector<std::string> out;
    for (ListRow i = 0; i < t.rows(); i++) if (has(t.row(i), part)) out.push_back(t.row(i));
    return out;
}
void namesOf(const MoonModule* m, std::vector<std::string>& out) {
    out.push_back(m->name());
    for (uint8_t i = 0; i < m->childCount(); i++) namesOf(m->child(i), out);
}
}  // namespace

TEST_CASE("MoonShineModule: the bars come first, a memory bar per type and a time bar per lane, then the nodes") {
    PipelineTree t;
    const auto bars = rowsWhere(t, "\"bar\":");
    REQUIRE(bars.size() >= 2);
    for (size_t i = 0; i < bars.size(); i++) CHECK(t.row(static_cast<ListRow>(i)) == bars[i]);
    CHECK(has(bars[0], "\"bar\":\"memory\""));
    CHECK(has(bars[0], "\"name\":\"heap\""));          // a computer reports no heap: one bar of what MoonLight took
    CHECK_FALSE(has(bars[0], "\"free\""));
    CHECK(has(bars[1], "\"name\":\"render loop\""));
    CHECK(has(t.row(static_cast<ListRow>(bars.size())), "\"name\":\"Grid\""));
}

TEST_CASE("MoonShineModule: every module outside the pipeline is a node of the Modules band, each module once") {
    PipelineTree t;
    std::vector<std::string> names;
    for (uint8_t i = 0; i < t.s.moduleCount(); i++) namesOf(t.s.module(i), names);
    for (const auto& n : names) CHECK_MESSAGE(rowsWhere(t, ("\"name\":\"" + n + "\"").c_str()).size() == 1, n);
    for (const char* other : {"MoonShine", "Layouts", "Effects"}) CHECK_MESSAGE(has(t.named(other), "\"group\":\"Modules\""), other);
    CHECK(has(t.named("Effects"), "\"edges\":[]"));    // a band's node has no wires, whatever column it takes
    CHECK_FALSE(has(t.named("Grid"), "\"group\""));
    CHECK_FALSE(has(t.named("Drivers"), "\"group\""));
}

TEST_CASE("MoonShineModule: a module below the top level names its top-level module, so the page can fold it in; a top level names none") {
    PipelineTree t;
    MoonModule* top = ModuleFactory::create("TasksModule");
    top->setName("Outer");
    MoonModule* mid = ModuleFactory::create("TasksModule");
    mid->setName("Middle");
    MoonModule* leaf = ModuleFactory::create("TasksModule");
    leaf->setName("Leaf");
    REQUIRE(mid->addChild(leaf));
    REQUIRE(top->addChild(mid));
    t.s.addModule(top);
    CHECK_FALSE(has(t.named("Outer"), "\"parent\""));
    CHECK(has(t.named("Middle"), "\"parent\":\"Outer\""));
    CHECK(has(t.named("Leaf"), "\"parent\":\"Outer\""));   // the top level, however deep: the page folds one level
    CHECK_FALSE(has(t.named("Grid"), "\"parent\""));          // the pipeline's nodes are never folded
}

TEST_CASE("MoonShineModule: the memory bar's modules part is what the nodes hold, each node per memory type") {
    PipelineTree t;
    long held = 0;
    for (const auto& r : rowsWhere(t, "\"internal\":")) held += field(r, "internal") + field(r, "psram");
    const auto parts = partsOf(rowsWhere(t, "\"bar\":\"memory\"")[0]);
    REQUIRE(parts.size() == 3);   // modules, module types, unaccounted; a computer has no free part
    CHECK(parts[0] == held);
    CHECK(parts[1] == static_cast<long>(ModuleFactory::tableBytes().total));
    CHECK(field(t.named("Grid"), "internal") >= field(t.named("Grid"), "size"));   // the object itself counts
}

TEST_CASE("MoonShineModule: each time bar adds up to the frame") {
    PipelineTree t;
    timed(t);
    const auto bars = rowsWhere(t, "\"bar\":\"time\"");
    REQUIRE_FALSE(bars.empty());
    for (const auto& b : bars) CHECK_MESSAGE(sum(partsOf(b)) == static_cast<long>(t.s.frameUs()), b);
}

TEST_CASE("MoonShineModule: a layer's chips split its own time between the render loop and the workers") {
    PipelineTree t;
    timed(t);
    for (const char* n : {"A", "E1", "B"}) {
        const std::string r = t.named(n);
        CHECK_MESSAGE(chip(r, 0) + chip(r, 1) == field(r, "own"), r);
        CHECK(chip(r, 2) == 0);
    }
    CHECK(chip(t.named("Grid"), 1) == 0);   // a layout runs on the render loop
}

TEST_CASE("MoonShineModule: the bottleneck is the one node with the largest chip on the lane with the least idle") {
    PipelineTree t;
    timed(t);
    const auto marked = rowsWhere(t, "\"bottleneck\":true");
    REQUIRE(marked.size() == 1);
    int busiest = 0;
    long least = -1;
    for (const auto& b : rowsWhere(t, "\"bar\":\"time\"")) {
        const long idle = partsOf(b).back();
        if (least < 0 || idle < least) { least = idle; busiest = static_cast<int>(field(b, "lane")); }
    }
    for (const auto& r : rowsWhere(t, "\"time\":[")) CHECK(chip(marked[0], busiest) >= chip(r, busiest));
}

TEST_CASE("MoonShineModule: a layer averages the time a worker ran it over the frames, as its tick time") {
    PipelineTree t;
    auto* a = static_cast<Layer*>(t.s.firstByName("A"));
    a->publishTiming(1);   // start from an empty window
    a->addWorkerUs(3000);
    a->addWorkerUs(1000);
    a->publishTiming(4);
    CHECK(a->workerUs() == 1000);
    a->publishTiming(4);
    CHECK(a->workerUs() == 0);   // the window starts again
}

TEST_CASE("MoonShineModule: the system tasks are nodes with their core, their stack, and their CPU time where the board measures it") {
    platform::TaskInfo tasks[2];
    std::strncpy(tasks[0].name, "wifi", sizeof(tasks[0].name));
    tasks[0].stackFreeBytes = 1200;
    tasks[0].cpuPermille = 250;
    tasks[0].core = 1;
    std::strncpy(tasks[1].name, "ipc0", sizeof(tasks[1].name));
    tasks[1].stackFreeBytes = 600;
    struct Restore { ~Restore() { platform::setTestTaskSnapshot(nullptr, 0, ""); } } restore;
    platform::setTestTaskSnapshot(tasks, 2, "");
    PipelineTree t;
    MoonModule* tm = ModuleFactory::create("TasksModule");
    tm->setName("Tasks");
    t.s.addModule(tm);
    timed(t);
    tm->tick1s();   // the snapshot refreshes once a second
    const std::string wifi = t.named("wifi"), ipc = t.named("ipc0");
    CHECK(has(wifi, "\"group\":\"System tasks\""));
    CHECK(has(wifi, "\"type\":\"task\""));
    CHECK(field(wifi, "stackFree") == 1200);
    CHECK(has(wifi, "\"core\":1"));   // the page groups the tasks per core
    CHECK(has(ipc, "\"core\":-1"));
    CHECK(chip(wifi, 3) == static_cast<long>(250u * t.s.frameUs() / 1000));
    CHECK(has(ipc, "\"time\":[]"));   // unmeasured: no chip rather than a zero
    CHECK(field(ipc, "stackFree") == 600);
    CHECK_FALSE(has(wifi, "\"bottleneck\""));
}
