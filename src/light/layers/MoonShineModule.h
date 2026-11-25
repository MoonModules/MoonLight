#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"      // topOfType: the containers this list reads
#include "core/module/Control.h"        // ListSource
#include "core/system/TasksModule.h"    // the system tasks' snapshot, refreshed once a second
#include "core/util/JsonSink.h"
#include "core/util/HeapBytes.h"        // each node's bytes per memory type
#include "core/util/ModuleFactory.h"    // tableBytes: the type table's part of the memory bars
#include "core/util/ScratchBuffer.h"    // the snapshot's rows
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/drivers/DriverBase.h"
#include "light/drivers/Drivers.h"      // renderWaitAvgUs, renderSplitActive: the encode lane

#include "platform/platform_heap.h"   // totalHeap, freeHeap, allocatedBytes
#include "platform/platform_tasks.h"   // TaskInfo, kTaskCpuUnmeasured

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace mm {

/// The running device as a flow of nodes: what it renders, stage by stage, its other modules and its system tasks, each with its time and memory.
///
/// The MoonShine card under System, a diagram that reads left to right.
///
/// @guide
///
/// ## Reading the flow
///
/// The bars on top split each memory type and each thread's frame into the modules' share, waits, unaccounted and free or idle. A node's chips show what it holds per memory type and its time per thread; the selector switches bytes and microseconds to percentages. The magenta node sets the frame rate. A ▸ count opens a group.
///
/// @technical
///
/// **A derived list.** Every row is read from the tree when the page asks, and not saved: the bars first, then the pipeline's nodes, then every other module in a band named "Modules", then the system tasks in a band named "System tasks". A parent's time includes its children's, and its own time leaves out the children it ticks itself.
///
/// **The memory bars.** One per memory type the board has, since internal RAM is scarce and PSRAM plentiful, each balancing on its own: the modules' `heapOf(this, classSize()) + heapBytes()`, the type table, "unaccounted" (the used heap no module reports, 0 at least) and "free". Used is `platform::totalHeap() - platform::freeHeap()` per type.
///
/// **On a computer**, which reports no heap, the one bar is "heap": what MoonLight took through its own allocator (`platform::allocatedBytes()`), with no free part.
///
/// **The time bars.** The frame's wall time (`Scheduler::frameUs`) per lane, each closing with its idle time so it adds up to the frame. The render loop: the modules' work, its waits for the workers and the encode task, the scheduler's work between modules. A render worker: the layers it ran, averaged over the workers (`WorkerPool::takeLoad`). The encode task: the drivers' ticks.
///
/// **The lanes.** A node's `time` gives its own time per lane: 0 the render loop, 1 the render workers, 2 the encode task, 3 a system task. A layer's nodes split by the share of the layer a worker ran (`Layer::workerUs`), a driver runs on the encode task while Drivers splits the frame, and Effects and Drivers leave out their waits, which the render bar shows as waiting.
///
/// **The bottleneck.** Of the render loop, the workers and the encode task, the one with the least idle sets the frame rate; its largest node is marked `bottleneck`.
///
/// **System tasks.** From TasksModule's snapshot: each task's core, least free stack and CPU time, as its share of the frame. The page groups them per core.
///
/// **Folding.** A module of the Modules band below the top level names its top-level module in `parent`; the page sums a folded top level's subtree from those rows, so the device writes each module once.
///
/// **One snapshot per second.** The figures and the rows in order are taken when the timing is published, at the next document's first row, or when the tree changes (`Scheduler::structureVersion`), into a buffer held from the first read until `release()`: one walk of the tree per snapshot. The rows carry no detail, and the page subscribes to MoonShine only while System is shown.
///
/// **Stages.** Layouts 0, effects 1, modifiers 2, layers 3, the Drivers blend 4, drivers 5. The edges name the next stage's nodes, and a wire is drawn as wide as the bytes it carries each frame. A node of the two bands takes the columns in turn, six to a row.
class MoonShineModule : public MoonModule, public ListSource {
public:
    /// The rows drawn as a flow.
    bool listAsFlow() const override { return true; }
    /// A derived list, rebuilt from the tree on every read.
    bool persistsList() const override { return false; }

    /// The bars and the nodes of the last snapshot.
    ListRow listRowCount() const override {
        const Snapshot& s = snapshot();
        return static_cast<ListRow>(s.v.barCount() + s.count);
    }

    /// Append one row: a bar, or a node with its figures, its stage and its wires.
    void writeListRow(JsonSink& sink, ListRow row) const override {
        const Snapshot& s = snapshot(row == 0);
        if (row < s.v.barCount()) { writeBar(sink, s.v, row); return; }
        const ListRow i = static_cast<ListRow>(row - s.v.barCount());
        if (i < s.count) writeNode(sink, s.v, nodes_[i]);
        else sink.append("{}");
    }

    /// No detail: the flow draws the rows alone, and a click opens the module's own card.
    void writeListRowDetail(JsonSink& sink, ListRow /*row*/) const override { sink.append("{}"); }

    /// The figures moved: the next read takes a new snapshot.
    void publishTiming(uint32_t frameCount) MM_NONBLOCKING override {
        MoonModule::publishTiming(frameCount);
        stale_ = true;
    }

    /// Declare the pipeline list.
    void defineControls() override {
        MoonModule::defineControls();
        /// The device as bars, then nodes.
        /// @field name Its name.
        /// @field bar "memory" or "time".
        /// @field type Memory type, or module type.
        /// @field lane A time bar's thread.
        /// @field parts A bar's parts, adding up.
        /// @field stage A node's column.
        /// @field us Time per frame, children included.
        /// @field own Time per frame, children left out.
        /// @field time Own time per thread.
        /// @field internal Bytes in internal RAM.
        /// @field psram Bytes in PSRAM.
        /// @field size Object size.
        /// @field bottleneck Sets the frame rate.
        /// @field group Its box.
        /// @field parent Its top-level module.
        /// @field core A task's core.
        /// @field stackFree A task's least free stack.
        controls_.addList("pipeline", *this);   // this module is the ListSource
    }

    /// The lanes of the time bars and the nodes' chips.
    enum Lane : uint8_t { kRender, kWorkers, kEncode, kTasks, kLanes };

private:
    static constexpr uint8_t kStages = 6;   ///< the pipeline's columns, which the bands share

    /// The memory figures, each type balancing on its own.
    struct Memory {
        HeapBytes modules, types, unaccounted, free;
        bool psram = false;    ///< the board has PSRAM: a bar per type
        bool totals = false;   ///< the platform reports its heap, so the bars close with free
    };
    /// The frame's figures, the last second's averages.
    struct Time {
        Effects::LayerLoad load;
        uint32_t effects = 0, drivers = 0;   ///< the two containers' ticks
        uint32_t roots = 0, others = 0;      ///< every top-level tick, and those of the modules besides the two
        uint32_t encode = 0, wait = 0;       ///< the drivers' ticks on the encode task, and the render loop's wait for it
        uint32_t tick = 0, frame = 0;        ///< the scheduler's tick and the frame period
        bool workers = false;                ///< render workers run beside the render loop
        bool encodeTask = false;             ///< the drivers run on the encode task
    };
    /// Everything one read needs, gathered once.
    struct View {
        const MoonModule* layouts = nullptr;
        const Effects* effects = nullptr;
        const Drivers* drivers = nullptr;
        const TasksModule* tasks = nullptr;
        Memory mem;
        Time time;
        const MoonModule* bottleneck = nullptr;
        uint8_t memoryBars() const { return mem.psram ? 2 : 1; }
        uint8_t barCount() const { return static_cast<uint8_t>(memoryBars() + 1 + time.workers + time.encodeTask); }
    };

    /// Which band a node is drawn in.
    enum class Band : uint8_t { Pipeline, Modules, Tasks };
    /// One node's place: a module, or a system task.
    struct Node {
        const MoonModule* m;                         ///< the module, null for a task
        uint8_t stage;                               ///< its column
        const MoonModule* layer;                     ///< the layer an effect, a modifier or a layer row belongs to, else null
        Band band = Band::Pipeline;
        const platform::TaskInfo* task = nullptr;    ///< the system task, for a task node
    };
    /// A node's own time per lane.
    struct LaneUs { uint32_t us[kLanes] = {}; };

    static uint32_t over(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }
    static size_t overSize(size_t a, size_t b) { return a > b ? a - b : 0; }

    static const MoonModule* top(const char* type) {
        const Scheduler* s = Scheduler::instance();
        return s ? s->topOfType(type) : nullptr;
    }

    // ---- the figures ----

    /// The three containers the walks and the wires read, looked up once per snapshot.
    static void findContainers(View& v) {
        v.layouts = top("Layouts");
        v.effects = static_cast<const Effects*>(top("Effects"));
        v.drivers = static_cast<const Drivers*>(top("Drivers"));
    }

    /// The memory and time figures, in one pass over the tree.
    static void gatherFigures(View& v) {
        const Scheduler* s = Scheduler::instance();
        v.mem.modules = {};
        for (uint8_t i = 0; s && i < s->moduleCount(); i++) gatherModules(s->module(i), v.mem.modules);
        closeMemory(v.mem);
        if (s) gatherTime(*s, v);
    }

    /// Every module's bytes.
    static void gatherModules(const MoonModule* m, HeapBytes& sum) {
        sum += heapOf(m, m->classSize()) + m->heapBytes();
        for (uint8_t i = 0; i < m->childCount(); i++) gatherModules(m->child(i), sum);
    }

    /// The type table, the used heap no module reports and the free heap, per type.
    static void closeMemory(Memory& mem) {
        const size_t heapTotal = platform::totalHeap(), heapFree = platform::freeHeap();
        const size_t internalTotal = platform::totalInternalHeap(), internalFree = platform::freeInternalHeap();
        mem.types = ModuleFactory::tableBytes();   // the one owner that is no module: the table every type registers in
        mem.totals = heapTotal > heapFree;
        mem.psram = heapTotal > internalTotal && heapFree >= internalFree;
        mem.free = mem.totals ? HeapBytes{heapFree, mem.psram ? heapFree - internalFree : 0} : HeapBytes{};
        // A computer reports no heap, so there used is what MoonLight took itself.
        const HeapBytes used = mem.totals ? HeapBytes{heapTotal - heapFree, mem.psram ? overSize(heapTotal - internalTotal, mem.free.psram) : 0}
                                          : HeapBytes{platform::allocatedBytes()};
        const HeapBytes held = mem.modules + mem.types;
        const size_t unPsram = overSize(used.psram, held.psram);
        mem.unaccounted = {overSize(used.internal(), held.internal()) + unPsram, unPsram};
    }

    /// The last second's averages from the tree.
    static void gatherTime(const Scheduler& sched, View& v) {
        Time& t = v.time;
        t.frame = sched.frameUs();
        t.tick = sched.tickTimeUs();
        if (v.effects) {
            t.load = v.effects->layerLoad();
            t.effects = v.effects->tickTimeUs();
            t.workers = v.effects->renderWorkers() > 0;
        }
        if (v.drivers) {
            t.drivers = v.drivers->tickTimeUs();
            t.encodeTask = v.drivers->renderSplitActive();
        }
        for (uint8_t i = 0; i < sched.moduleCount(); i++) {
            const MoonModule* m = sched.module(i);
            t.roots += m->tickTimeUs();
            if (m != v.effects && m != v.drivers) t.others += m->tickTimeUs();
        }
        if (!t.encodeTask) return;
        t.wait = v.drivers->renderWaitAvgUs();
        for (uint8_t i = 0; i < v.drivers->childCount(); i++)
            if (v.drivers->child(i)->role() == ModuleRole::Driver) t.encode += v.drivers->child(i)->tickTimeUs();
    }

    /// A lane's idle time per frame.
    static uint32_t idle(const Time& t, Lane lane) {
        switch (lane) {
            case kWorkers: return over(t.frame, t.load.workerUs);
            case kEncode: return over(t.frame, t.encode);
            default: return over(t.frame, t.tick);
        }
    }

    // ---- the bars ----

    /// The `k`th lane this device runs.
    static Lane laneOfBar(const View& v, ListRow k) {
        if (k == 0) return kRender;
        return k == 1 && v.time.workers ? kWorkers : kEncode;
    }

    static void writeBar(JsonSink& sink, const View& v, ListRow row) {
        if (row < v.memoryBars()) writeMemoryBar(sink, v.mem, static_cast<uint8_t>(row));
        else writeTimeBar(sink, v.time, laneOfBar(v, row - v.memoryBars()));
    }

    /// One memory type's bar: the modules, the type table, the unaccounted rest, and free where the platform says.
    static void writeMemoryBar(JsonSink& sink, const Memory& mem, uint8_t type) {
        const auto part = [&](const HeapBytes& h) { return static_cast<unsigned>(!mem.psram ? h.total : type == 1 ? h.psram : h.internal()); };
        const char* name = !mem.totals ? "heap" : type == 1 ? "PSRAM" : "internal RAM";
        sink.appendf("{\"name\":\"%s\",\"bar\":\"memory\",\"type\":%u,\"parts\":[[\"modules\",%u],[\"module types\",%u],[\"unaccounted\",%u]",
                     name, static_cast<unsigned>(type), part(mem.modules), part(mem.types), part(mem.unaccounted));
        if (mem.totals) sink.appendf(",[\"free\",%u]", part(mem.free));
        sink.append("]}");
    }

    /// One lane's bar: its work, its waits and its idle, adding up to the frame.
    static void writeTimeBar(JsonSink& sink, const Time& t, Lane lane) {
        static constexpr const char* kLaneNames[] = {"render loop", "layer workers", "encode task"};
        sink.appendf("{\"name\":\"%s\",\"bar\":\"time\",\"lane\":%u,\"parts\":[", kLaneNames[lane], static_cast<unsigned>(lane));
        if (lane == kRender) {
            const uint32_t work = over(t.effects, t.load.hereUs + t.load.joinUs) + t.load.hereUs + over(t.drivers, t.wait) + t.others;
            sink.appendf("[\"modules\",%u],[\"waiting\",%u],[\"between modules\",%u],[\"idle\",%u]", static_cast<unsigned>(work),
                         static_cast<unsigned>(t.load.joinUs + t.wait), static_cast<unsigned>(over(t.tick, t.roots)), static_cast<unsigned>(idle(t, kRender)));
        } else {
            const uint32_t busy = lane == kWorkers ? t.load.workerUs : t.encode;
            sink.appendf("[\"modules\",%u],[\"idle\",%u]", static_cast<unsigned>(busy), static_cast<unsigned>(idle(t, lane)));
        }
        sink.append("]}");
    }

    // ---- the lanes and the bottleneck ----

    /// A module's own time, less the wait Effects and Drivers spend on another thread, which the render bar shows as waiting.
    static uint32_t ownOf(const View& v, const MoonModule* m) {
        if (m == v.effects) return over(m->ownTimeUs(), v.time.load.joinUs);
        if (m == v.drivers) return over(m->ownTimeUs(), v.time.wait);
        return m->ownTimeUs();
    }

    /// The part of `own` a render worker ran: the layer's worker share of its time.
    static uint32_t workerPart(const MoonModule* layer, uint32_t own) {
        const auto* l = static_cast<const Layer*>(layer);
        const uint32_t total = l->tickTimeUs();
        return total ? static_cast<uint32_t>(uint64_t{own} * std::min(l->workerUs(), total) / total) : 0;
    }

    /// A node's own time per lane.
    static LaneUs laneTimes(const View& v, const Node& n) {
        LaneUs t;
        if (n.task) {
            if (n.task->cpuPermille != platform::kTaskCpuUnmeasured) t.us[kTasks] = static_cast<uint32_t>(uint64_t{n.task->cpuPermille} * v.time.frame / 1000);
            return t;
        }
        const uint32_t own = ownOf(v, n.m);
        if (n.layer) {
            t.us[kWorkers] = workerPart(n.layer, own);
            t.us[kRender] = own - t.us[kWorkers];
        } else if (v.time.encodeTask && n.m->parent() == v.drivers && n.m->role() == ModuleRole::Driver) {
            t.us[kEncode] = own;
        } else {
            t.us[kRender] = own;
        }
        return t;
    }

    /// The largest module on the lane with the least idle, the one that sets the frame rate; null while no module takes time.
    static const MoonModule* bottleneckOf(const View& v, const Node* nodes, ListRow count) {
        Lane busiest = kRender;
        if (v.time.workers && idle(v.time, kWorkers) < idle(v.time, busiest)) busiest = kWorkers;
        if (v.time.encodeTask && idle(v.time, kEncode) < idle(v.time, busiest)) busiest = kEncode;
        const MoonModule* best = nullptr;
        uint32_t most = 0;
        for (ListRow i = 0; i < count; i++) {
            const uint32_t us = nodes[i].m ? laneTimes(v, nodes[i]).us[busiest] : 0;
            if (us > most) { most = us; best = nodes[i].m; }
        }
        return best;
    }

    // ---- the snapshot ----

    /// The figures and the rows in order, taken once and written row by row.
    struct Snapshot {
        View v;
        ListRow count = 0;      ///< the nodes in `nodes_`
        uint32_t version = UINT32_MAX;   ///< the tree's modification count it was taken at, none before the first
    };
    mutable Snapshot snap_;
    mutable ScratchBuffer<Node> nodes_{*this};   ///< the nodes in row order, grown as the tree grows
    mutable bool stale_ = true;                  ///< the figures moved since the snapshot

    /// The snapshot, taken again once the tree changes, and once the figures moved at the start of a document (`start`, its first row), so one document never mixes two.
    const Snapshot& snapshot(bool start = false) const {
        if ((start && stale_) || snap_.version != treeVersion()) take();
        return snap_;
    }
    /// The tree's modification count, 0 with no scheduler.
    static uint32_t treeVersion() {
        const Scheduler* s = Scheduler::instance();
        return s ? s->structureVersion() : 0;
    }
    /// One walk for the rows, then one pass for the figures, after the rows' buffer grew, so its bytes count in the memory bars as on their node.
    void take() const {
        View& v = snap_.v;
        v = View{};
        findContainers(v);
        snap_.version = treeVersion();
        stale_ = false;
        ListRow n = 0;
        walk(v, [&](const Node& node) {
            // The Modules band comes before the tasks band, so the task table is found before its rows.
            if (node.m && !v.tasks && std::strcmp(node.m->typeName(), "TasksModule") == 0) v.tasks = static_cast<const TasksModule*>(node.m);
            if (!nodes_.ensure(n + 1u)) return false;   // memory refused: the list holds the nodes that fit
            nodes_[n++] = node;
            return true;
        });
        snap_.count = n;
        gatherFigures(v);
        v.bottleneck = bottleneckOf(v, nodes_.data(), n);
    }

    // ---- the walk ----

    /// The layouts, stage 0. The walk visits the rows in order and stops at the first visitor that returns false; each step answers whether to go on.
    template <typename F> static bool walkLayouts(const View& v, F& f) {
        for (uint8_t i = 0; v.layouts && i < v.layouts->childCount(); i++)
            if (!f(Node{v.layouts->child(i), 0, nullptr})) return false;
        return true;
    }
    template <typename F> static bool walkRole(const MoonModule* layer, ModuleRole role, uint8_t stage, F& f) {
        for (uint8_t k = 0; k < layer->childCount(); k++)
            if (layer->child(k)->role() == role && !f(Node{layer->child(k), stage, layer})) return false;
        return true;
    }
    template <typename F> static bool walkLayer(const MoonModule* layer, F& f) {
        return walkRole(layer, ModuleRole::Effect, 1, f) && walkRole(layer, ModuleRole::Modifier, 2, f) && f(Node{layer, 3, layer});
    }
    template <typename F> static bool walkLayers(const View& v, F& f) {
        for (uint8_t i = 0; v.effects && i < v.effects->childCount(); i++)
            if (v.effects->child(i)->role() == ModuleRole::Layer && !walkLayer(v.effects->child(i), f)) return false;
        return true;
    }
    template <typename F> static bool walkDrivers(const View& v, F& f) {
        if (!v.drivers) return true;
        if (!f(Node{v.drivers, 4, nullptr})) return false;
        for (uint8_t i = 0; i < v.drivers->childCount(); i++)
            if (v.drivers->child(i)->role() == ModuleRole::Driver && !f(Node{v.drivers->child(i), 5, nullptr})) return false;
        return true;
    }

    /// Whether the pipeline's walk visits `m`, mirroring the walks above.
    static bool inPipeline(const View& v, const MoonModule* m) {
        const MoonModule* p = m->parent();
        const ModuleRole r = m->role();
        if (!p) return m == v.drivers;
        if (p == v.layouts) return true;
        if (p == v.drivers) return r == ModuleRole::Driver;
        if (p == v.effects) return r == ModuleRole::Layer;
        return p->role() == ModuleRole::Layer && p->parent() == v.effects && (r == ModuleRole::Effect || r == ModuleRole::Modifier);
    }
    /// Every module outside the pipeline, depth first, the band's columns taken in turn.
    template <typename F> static bool walkOthers(const View& v, const MoonModule* m, uint16_t& index, F& f) {
        if (!inPipeline(v, m) && !f(Node{m, static_cast<uint8_t>(index++ % kStages), nullptr, Band::Modules})) return false;
        for (uint8_t k = 0; k < m->childCount(); k++)
            if (!walkOthers(v, m->child(k), index, f)) return false;
        return true;
    }
    template <typename F> static bool walkModules(const View& v, F& f) {
        const Scheduler* s = Scheduler::instance();
        uint16_t index = 0;
        for (uint8_t i = 0; s && i < s->moduleCount(); i++)
            if (!walkOthers(v, s->module(i), index, f)) return false;
        return true;
    }
    template <typename F> static bool walkTasks(const View& v, F& f) {
        for (uint8_t i = 0; v.tasks && i < v.tasks->taskCount(); i++)
            if (!f(Node{nullptr, static_cast<uint8_t>(i % kStages), nullptr, Band::Tasks, &v.tasks->task(i)})) return false;
        return true;
    }
    template <typename F> static void walk(const View& v, F&& f) {
        (void)(walkLayouts(v, f) && walkLayers(v, f) && walkDrivers(v, f) && walkModules(v, f) && walkTasks(v, f));
    }

    // ---- the wires ----

    /// The wires of one row, written as a JSON array.
    struct EdgeWriter {
        JsonSink& sink;
        bool first = true;
        void add(const MoonModule* to, size_t bytes) {
            sink.append(first ? "{\"to\":" : ",{\"to\":");
            first = false;
            sink.writeJsonString(to->name());
            if (bytes) sink.appendf(",\"bytes\":%u", static_cast<unsigned>(bytes));
            sink.append("}");
        }
    };

    /// The bytes of a layer's buffer, which every wire inside the pipeline carries.
    static size_t layerBytes(const MoonModule* layer) {
        return layer ? static_cast<const Layer*>(layer)->buffer().bytes() : 0;
    }

    /// The first enabled modifier of `layer` after child index `from`, else null.
    static const MoonModule* nextModifier(const MoonModule* layer, int from) {
        for (uint8_t k = static_cast<uint8_t>(from + 1); k < layer->childCount(); k++)
            if (layer->child(k)->role() == ModuleRole::Modifier && layer->child(k)->enabled()) return layer->child(k);
        return nullptr;
    }

    static int indexIn(const MoonModule* parent, const MoonModule* m) {
        for (uint8_t k = 0; k < parent->childCount(); k++) if (parent->child(k) == m) return k;
        return -1;
    }

    /// A layout maps to every enabled layer.
    static void layoutEdges(EdgeWriter& w, const View& v) {
        for (uint8_t i = 0; v.effects && i < v.effects->childCount(); i++)
            if (v.effects->child(i)->role() == ModuleRole::Layer && v.effects->child(i)->enabled()) w.add(v.effects->child(i), 0);
    }

    /// An effect or a modifier feeds the next enabled modifier of its layer, else the layer.
    static void chainEdge(EdgeWriter& w, const Node& n) {
        const MoonModule* next = nextModifier(n.layer, n.stage == 1 ? -1 : indexIn(n.layer, n.m));
        w.add(next ? next : n.layer, layerBytes(n.layer));
    }

    /// Drivers hands each enabled driver its window of the first enabled layer's buffer.
    static void driversEdges(EdgeWriter& w, const View& v, const MoonModule* drivers) {
        const Layer* out = v.effects ? v.effects->firstEnabledLayer() : nullptr;
        for (uint8_t i = 0; i < drivers->childCount(); i++) {
            const MoonModule* d = drivers->child(i);
            if (d->role() != ModuleRole::Driver || !d->enabled()) continue;
            const size_t lights = out ? static_cast<const DriverBase*>(d)->windowLength(out->buffer().count()) : 0;
            w.add(d, out ? lights * out->channelsPerLight() : 0);
        }
    }

    /// The wires of a pipeline node that is on: toward the next stage.
    static void liveEdges(EdgeWriter& w, const View& v, const Node& n) {
        switch (n.stage) {
            case 0: layoutEdges(w, v); break;
            case 1: case 2: chainEdge(w, n); break;
            case 3: if (v.drivers) w.add(v.drivers, layerBytes(n.layer)); break;
            case 4: driversEdges(w, v, n.m); break;
            default: break;
        }
    }

    /// A node of a band, a disabled node, or a node of a disabled layer, has no wires.
    static void writeEdges(JsonSink& sink, const View& v, const Node& n) {
        sink.append(",\"edges\":[");
        EdgeWriter w{sink};
        if (n.band == Band::Pipeline && n.m->enabled() && (!n.layer || n.layer->enabled())) liveEdges(w, v, n);
        sink.append("]");
    }

    // ---- the nodes ----

    /// The lanes a node spends time on, as `[lane, us]` pairs.
    static void writeTime(JsonSink& sink, const LaneUs& t) {
        sink.append(",\"time\":[");
        bool first = true;
        for (uint8_t l = 0; l < kLanes; l++) {
            if (!t.us[l]) continue;
            sink.appendf(first ? "[%u,%u]" : ",[%u,%u]", static_cast<unsigned>(l), static_cast<unsigned>(t.us[l]));
            first = false;
        }
        sink.append("]");
    }

    /// A band node's box, and below the top level the top-level module it belongs to, so the page can fold a module into its top level.
    static void writeGroup(JsonSink& sink, const Node& n) {
        if (n.band == Band::Modules) {
            sink.append(",\"group\":\"Modules\"");
            const MoonModule* top = n.m;
            while (top->parent()) top = top->parent();
            if (top != n.m) { sink.append(",\"parent\":"); sink.writeJsonString(top->name()); }
        } else if (n.layer) { sink.append(",\"group\":"); sink.writeJsonString(n.layer->name()); }
    }

    /// A system task: its CPU time where measured, its core and its least free stack.
    static void writeTask(JsonSink& sink, const View& v, const Node& n) {
        const platform::TaskInfo& t = *n.task;
        const LaneUs lanes = laneTimes(v, n);
        sink.append("{\"name\":");
        sink.writeJsonString(t.name);
        sink.appendf(",\"type\":\"task\",\"stage\":%u,\"us\":%u,\"own\":%u,\"core\":%d,\"stackFree\":%u,\"enabled\":true",
                     static_cast<unsigned>(n.stage), static_cast<unsigned>(lanes.us[kTasks]), static_cast<unsigned>(lanes.us[kTasks]),
                     static_cast<int>(t.core), static_cast<unsigned>(t.stackFreeBytes));
        writeTime(sink, lanes);
        sink.append(",\"group\":\"System tasks\",\"edges\":[]}");
    }

    static void writeNode(JsonSink& sink, const View& v, const Node& n) {
        if (n.task) { writeTask(sink, v, n); return; }
        static constexpr const char* kSeverities[] = {"status", "warning", "error"};
        const HeapBytes held = heapOf(n.m, n.m->classSize()) + n.m->heapBytes();
        sink.append("{\"name\":");
        sink.writeJsonString(n.m->name());
        sink.append(",\"type\":");
        sink.writeJsonString(n.m->typeName());
        sink.appendf(",\"stage\":%u,\"us\":%u,\"own\":%u,\"size\":%u,\"internal\":%u,\"psram\":%u,\"enabled\":%s",
                     static_cast<unsigned>(n.stage), static_cast<unsigned>(n.m->tickTimeUs()), static_cast<unsigned>(n.m->ownTimeUs()),
                     static_cast<unsigned>(n.m->classSize()), static_cast<unsigned>(held.internal()), static_cast<unsigned>(held.psram),
                     n.m->enabled() ? "true" : "false");
        writeTime(sink, laneTimes(v, n));
        if (n.m == v.bottleneck) sink.append(",\"bottleneck\":true");
        if (n.m->status() && n.m->severity() != Severity::Status)
            sink.appendf(",\"severity\":\"%s\"", kSeverities[static_cast<int>(n.m->severity())]);
        writeGroup(sink, n);
        writeEdges(sink, v, n);
        sink.append("}");
    }
};

}  // namespace mm
