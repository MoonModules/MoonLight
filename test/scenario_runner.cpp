/// Scenario runner: reads scenario JSON files and replays their steps in-process.
///
/// The same JSON files run against a live system through `moondeck/scenario/run_live_scenario.py`, so one scenario serves both tiers.
///
/// @moreinfo
///
/// ## Why the parser decodes escapes
///
/// Appending the character after a backslash raw turned `\n` into a literal `n`, so a multi-line string arrived as one line.
/// The failure was invisible until `write_file` staged a script and the compiler reported "expected '(' after the function name" on a file that looked correct in the JSON.
/// So every escape the parser accepts is decoded to the byte it names.
///
/// ## Why a uXXXX escape warns instead of decoding
///
/// No scenario needs one, and a half-done UTF-16 surrogate decoder would be worse than not having one.
/// It must not pass through silently either. Appending a literal `u` stages a script containing `u00e9`, and the failure then surfaces as a confusing compile error in a file that looks right in the JSON.
/// So the runner says so, loudly, once, and keeps the escape visible rather than half-decoding it.
///
/// ## Why the target key is always the host desktop
///
/// The per-step `expected[<target>]` lookup needs a target key, and the in-process runner builds for the host only.
/// There is no cross-compiled scenario_runner, so the key is always `desktop-<host-os>`, matching the `run_live_scenario.py` convention.
///
/// ## How a scenario value reaches a control
///
/// A `set_control` step finds the module by id and the control by name, writes the typed value, then mirrors `HttpServerModule::handleSetControl`.
/// That means calling `onControlChanged()` and, when `affectsPrepare()` returns true, triggering `Scheduler::prepareTree()` so the pipeline reconciles.
/// The bridge runs JsonVal through raw JSON text into `mm::applyControlValue`, so this file hand-rolls none of the per-ControlType dispatch that Control.cpp owns.
/// A tiny `{"v":VALUE}` wrapper built with JsonSink in heap-grow mode carries it, since writeNumber, writeBool and writeJsonString each produce JSON-correct text per JsonVal type.
/// Re-serialize-then-parse costs nothing worth counting in test code, at most a hundred set_control ops per scenario.
/// Strict policy: an out-of-range Uint8, Int16 or Select fails the step, so a scenario-authoring bug surfaces instead of silently clamping into a boundary value.
/// This is stricter than the pre-refactor behavior for Uint8 and Int16, which clamped silently, and matches it for Select and IPv4, which already failed; no scenario relied on the clamp.
///
/// ## Why a delete purges the whole subtree
///
/// Deleting a module that has registered children, a Layer with an effect child say, would leave their ids pointing at freed memory.
/// So every `modules` entry whose pointer lies in the root's subtree is erased first, root and all descendants, which means walking the live tree while the subtree is still intact.
/// `purgeSubtree` runs before `deleteTree`, and `replace_module` and `clear_children` mirror it for the same reason.
///
/// ## Why fixtures wire props at construct time
///
/// The fixture phase runs before the scheduler starts, so `set_control` cannot apply grid dimensions yet.
/// Without a construct-time apply, `props.width` and `props.height` were silently ignored and the grid stayed at the layout's default, masking the real scenario size.
/// GridBlacksLayout takes the same apply; its dark-column controls blackStart and blackCount are set later through `set_control`, which works post-start.
/// Effects gets the container's Layouts, mirroring main.cpp's `effectsContainer->setLayouts`, and re-propagates it to every child Effects at each prepareTree, so a Layer added later picks it up.
/// Drivers prefers binding the Effects container, since the active Layer is re-resolved at every prepareTree. So a Layer cleared and rebuilt mid-scenario is picked up, and pinning one Layer is the fallback for older fixtures.
/// PreviewDriver needs no scenario-specific wiring: it reads its Layer and sparse source buffer through Drivers' passBufferToDrivers, and owns its own scratch buffers.
/// No broadcaster is wired, the harness has no WS server, so sendFrame and sendCoordTable early-return on the null broadcaster while the light-extraction work still runs for honest tick measurement.
///
/// ## Why skip_on exists
///
/// A scenario-level `skip_on` allowlist names host targets that lack a capability the scenario exercises.
/// Today that is the MoonLive scenarios opting out on desktop-windows and desktop-linux, because the desktop JIT is arm64-only. An x86_64 host renders dark, so the scenario's "buffer non-zero" check fails for a platform-capability reason it is the wrong vehicle to assert.
/// An absent or empty `skip_on` runs everywhere, the existing default.
/// It is the field `run_scenario.py` honours, and keeping the C++ runner in step stops KPI collection, which calls mm_scenarios directly, from counting a skipped scenario as a failure.
///
/// ## The construct and mutate modes
///
/// The `mode` field says what shape the scenario expects the world to be in; see docs/reference/testing.md § Scenario modes.
/// A construct scenario builds the pipeline from an empty scheduler and runs in-process only. A live device's main.cpp owns the top-level shape, and only the in-process runner can provide an empty scheduler.
/// A mutate scenario assumes a wired pipeline. In-process replays the embedded `fixture` array first, an array of add_module steps in the same shape as `steps`, then the steps. Live runs the steps directly against whatever is wired.
/// A mutate scenario without a fixture can still run live, the device being its own fixture, but cannot run in-process.
/// The default is construct, for back-compatibility with the scenarios that pre-date the field and build their pipelines explicitly.
/// Bespoke convention: the construct and mutate split, plus fixture and reset, is MoonLight-specific rather than borrowed from an off-the-shelf BDD framework.
/// It exists because the same JSON serves an in-process runner that owns the scheduler and a live runner that does not, main.cpp doing that there.
/// xUnit fixtures are the closest analog for `fixture`, and SQL BEGIN/ROLLBACK for `reset`.
///
/// ## The fixture, reset and steps sections
///
/// Three sections run in order, each with its own banner so the output is easy to scan.
/// Fixture carries add_module steps, in-process only, and builds the wired pipeline.
/// Reset carries set_control steps to a known state, runs on both tiers, and makes a scenario start from the same place regardless of previous runs.
/// Steps is the scenario proper.
///
/// ## Why the scheduler starts lazily
///
/// Steps are processed in order, and the first measure step, or the end of the scenario, flips the scheduler into setup and running mode.
/// After that a mid-scenario add_module or set_control step mutates the running pipeline, the same shape as the live runner driving changes over REST.
/// A mid-scenario add therefore also calls defineControls, setup and prepareTree at once, mirroring what HttpServerModule does on `/api/modules`.
/// Starting after the fixture is what makes set_control work at all, since controls are populated in defineControls during setup.
/// Per-step heap snapshots roll forward, so each measuring step reports its delta against the previous one.
///
/// ## Why an unknown module type fails
///
/// A failed create is a failed scenario: naming an unregistered type would otherwise pass while testing nothing.
/// The `optional` flag is for a type genuinely unavailable here, and it is honoured on add_module, set_control, measure, remove_module and delete_module and nowhere else.
/// Carrying it elsewhere reads as an escape hatch that does not exist, so the runner fails such a step rather than accepting the word.
///
/// ## Why write_file exists
///
/// It stages a file the way the UI's editor does, so a scenario can drive the script loop end-to-end. Write a script, point a module's `script` control at it, and measure.
/// It uses the primitive the HTTP save path uses, fsWriteAtomic, so a scenario exercises the file the device would actually read. It also mkdir -p's the parent, because a scenario names `/moonlive/x.mle` without staging the directory.
/// The interesting cases have no shipped file to select. A deliberately broken script, proving the device degrades rather than dies, could not live in moonlive/, where unit_MoonLiveScripts compiles all of them.
/// Neither could an edit that changes a script's control set, proving controls re-derive and keep their values.
/// A malformed step and a failed write are both failed scenarios. Every later step then runs against a file that was never staged, and the run could still report PASSED.
/// Skipping either silently is what makes a typo'd key pass here and fail on hardware, where `run_live_scenario.py` already treats it as an error.
///
/// ## Why remove_module and delete_module are aliases
///
/// Both are accepted so a scenario reads identically here and on the live runner, which uses `delete_module`.
/// The two runners must never diverge on op names, or a scenario silently no-ops on one tier.
/// The op removes a child from its parent, mirroring `HttpServerModule::handleDeleteModule`: remove from parent, release, recursive delete, rebuild pipeline state.
/// Only child modules can be removed, top-level modules being policy-fixed, and a non-editable submodule such as Board, Preview or Improv is apparatus rather than content.
/// `replace_module` mirrors `handleReplaceModule` under the same rules, and re-registers the fresh module under the same scenario id so later steps still address it by that id.
///
/// ## Why clear_children exists
///
/// It is the "prepare my own canvas" primitive: a scenario assumes nothing about the device's starting tree, clears a container, then adds what it needs.
/// Children are deleted including ones the scenario never added, such as a live device's pre-existing effects and modifiers, leaving the container itself.
/// It mirrors remove_module's release looped over all children, walking back-to-front since removeChild compacts the array in place, and skipping the non-editable submodules the live device also keeps.
///
/// ## Why a measure step has two spellings
///
/// `"measure": true` is the explicit flag, used alongside a set_control to measure after a mutation.
/// `op: "measure"` is the implicit-measure shape, a snapshot step with no other side effects, so scenarios can interleave snapshots without faking a control write.
/// The measurement block honours both.
/// A `bounds.fps.min_pct` is relative to a live baseline, used by the WiFi-versus-Eth scenarios, and only the live runner has a baseline to compare against.
/// In-process cannot enforce it, so it logs a clear skip and users see why the bound was not applied instead of it silently becoming "FPS > 0".
///
/// ## What the heap numbers mean
///
/// `heap=` is the absolute free heap after the measurement window, which is what `observed.<target>.free_heap` consumes: the rolling promise is on actual free heap rather than on a delta.
/// On desktop `freeHeap()` returns 0, meaning unlimited, and the value renders as 0, which the runner treats as "no heap assertion".
/// `(step: ±N)` is the signed step delta from the pre-step heap to the post-measurement heap, useful for diagnosing which step consumed memory but not what the contract asserts on.
/// It is kept for human-readable diagnostics.
/// A heap-delta bound fails a step that grew the heap by more than `max_delta_bytes` against the previous measurement, which catches a leak or an unintended alloc.
///
/// ## Why the block size is internal RAM only
///
/// `maxInternalAllocBlock` reports the largest contiguous block in internal RAM, which diagnoses internal-heap fragmentation.
/// Fragmentation silently degrades the Layer LUT, whose buffer needs 60 to 90 KB contiguous at 128×128 with mirror, and dropping mirror does not change free_heap.
/// The internal-only variant is what makes the signal work on PSRAM boards too, where `maxAllocBlock` would report about 8 MB regardless of internal pressure.
/// On desktop it reads 0, meaning unlimited.
///
/// ## The performance contract
///
/// Each measure step may carry a `contract[<target>]` block with `tick_us` and `free_heap`, optional `tick_tolerance_pct`, `heap_tolerance_pct` and `tolerance_us`, and `set_by` plus `reason` describing when and why the contract was set.
/// Contracts are hand-blessed promises, renegotiated with `--update-contract --reason`; the whole block is optional, and the live runner shares this shape so the same scenarios serve both tiers.
/// `tick_us` is a ceiling, so faster than contract is good news and the tolerance absorbs upward jitter only: a speedup never fails.
/// `free_heap` is a floor, meaningful on ESP32 and reported as unlimited on desktop. The device must deliver at least this much, more is fine, and less by more than the tolerance is a regression.
/// `max_alloc_block` is a floor too, because the LUT and driver buffers need a single contiguous chunk far larger than total free heap once fragmentation kicks in.
/// A scenario opts into that assertion when its workload depends on a specific minimum, the mirror LUT degrading silently when the block will not fit; see `src/light/layers/Layer.h` Layer::rebuildLUT.
/// The field is optional and skipped on desktop, where the value is always 0.
///
/// ## Why the tolerance defaults differ per target
///
/// The per-target defaults reflect run-to-run variance rather than indifference.
/// A desktop target carries multi-process OS jitter, so 20 percent plus a 200us absolute floor, and the floor dominates below about 1ms tick, the realistic case.
/// An esp32 target is a bounded RTOS with lwIP and EMAC jitter, so 10 percent plus a 5us floor, realistic for the bounded RTOS clock.
/// An absolute floor is needed because OS scheduling jitter dwarfs any percentage tolerance at sub-millisecond ticks.
/// KEEP IN SYNC: the live runner re-declares the same defaults in the contract-block handler of `moondeck/scenario/run_live_scenario.py`, and tuning one without the other silently desyncs the two tiers.
///
/// ## Why a filesystem error escapes main
///
/// Directory iteration can throw `filesystem_error`, a scenarios/ directory deleted mid-run being the case.
/// Letting it escape main is the correct outcome for a CLI test runner. It terminates with a diagnostic and a non-zero status, which is exactly what a harness needs to see.
/// Discovery is recursive so the core/ and light/ split picks up every JSON without each subfolder needing its own loop.
///
/// ## What a measurement covers
///
/// A measure step ticks for a span of wall time rather than a fixed frame count, and reports the average tick.
/// The frame count measured the wrong thing: 200 back-to-back ticks span well under a millisecond.
/// An effect whose animation arrives on a beat has nothing to do inside a window that short.
/// A span is the clock the effects read, so a window of wall time holds that much animation on any host.
///
/// The window is sized to the slowest shipped default, which is PulseEffect at its default 40 bpm: it emits every 1500 ms and returns early on every tick in between.
/// A window shorter than one of those intervals can fall entirely between two emissions and report an idle effect as the effect's cost.
///
/// The frame cap is the other half of the same rule, sized ABOVE what the window can reach rather than as a round number.
/// An early-outing effect on a desktop ticks about three thousand times per millisecond, so a cap of 200k ended the loop after 65 ms of a 1600 ms window.
/// Every sample then stopped short of the animation it was meant to cover.
/// So reaching the cap before the clock is reported rather than passed off as a full span.
///
/// ## What a green run is allowed to mean
///
/// A scenario that did not run returns `kSkipped` rather than 0, because a skip counted as a pass is how a suite that stopped testing reads as green.
/// Ten of eleven scenarios once skipped for a missing fixture while the summary said `11 passed`, hiding three assertion bugs for a commit.
/// The same reason makes a scenario that runs to the end with zero checks a failure: it is indistinguishable from one whose every step silently did nothing.
/// `expect_control` therefore also takes `not_equals`, for a value that moves per release where the only stable claim is a negative one.
/// An empty string is what a read-only control renders when its source is missing, so `not_equals: ""` asserts that a card reports anything at all.
///

#include "module_types.h"

#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Control.h"
#include "core/util/JsonSink.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/GridBlacksLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>

static void printModuleMemory(mm::MoonModule* mod, int indent) {
    if (!mod) return;
    for (int i = 0; i < indent; i++) std::printf("  ");
    std::printf("%s: sizeof=%zu heap=%zu\n",
                mod->name() ? mod->name() : "?",
                mod->classSize(), mod->dynamicBytes());
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        printModuleMemory(mod->child(i), indent + 1);
    }
}
#include <string>
#include <map>
#include <filesystem>
#include <vector>

// Minimal JSON value, enough for scenario files (flat objects, arrays of objects)
/// A JSON value as the API renders it, so a scenario may write a number or a bool unquoted.
static std::string asWritten(const struct JsonVal& v);

struct JsonVal {
    enum Type { Null, String, Number, Bool, Object, Array };
    Type type = Null;
    std::string str;
    double num = 0;
    bool boolean = false;
    std::map<std::string, JsonVal> obj;
    std::vector<JsonVal> arr;

    bool has(const char* key) const { return obj.count(key) > 0; }
    const JsonVal& operator[](const char* key) const {
        static JsonVal null;
        auto it = obj.find(key);
        return it != obj.end() ? it->second : null;
    }
    const char* c_str() const { return str.c_str(); }
    int asInt() const { return static_cast<int>(num); }
};

/// Defined here, where JsonVal is complete.
static std::string asWritten(const JsonVal& v) {
    switch (v.type) {
        case JsonVal::Bool:   return v.boolean ? "true" : "false";
        case JsonVal::Number: {
            char buf[32];   // integral values render without a decimal point, as a control shows them
            if (v.num == static_cast<long long>(v.num))
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
            else
                std::snprintf(buf, sizeof(buf), "%g", v.num);
            return buf;
        }
        default: return v.str;
    }
}

// Minimal JSON parser
struct JsonParser {
    const char* p;

    void skipWs() { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }

    JsonVal parse() {
        skipWs();
        if (*p == '"') return parseString();
        if (*p == '{') return parseObject();
        if (*p == '[') return parseArray();
        if (*p == 't' || *p == 'f') return parseBool();
        if (*p == 'n') { p += 4; return {}; }
        return parseNumber();
    }

    JsonVal parseString() {
        p++; // skip opening "
        JsonVal v; v.type = JsonVal::String;
        while (*p && *p != '"') {
            // Decode the escape rather than dropping the backslash: @xref{why-the-parser-decodes-escapes}.
            if (*p == '\\') {
                p++;
                switch (*p) {
                    case 'n':  v.str += '\n'; p++; break;
                    case 't':  v.str += '\t'; p++; break;
                    case 'r':  v.str += '\r'; p++; break;
                    case 'b':  v.str += '\b'; p++; break;
                    case 'f':  v.str += '\f'; p++; break;
                    case '"':  v.str += '"';  p++; break;
                    case '\\': v.str += '\\'; p++; break;
                    case '/':  v.str += '/';  p++; break;
                    // A \uXXXX escape is warned about rather than decoded: @xref{why-a-uxxxx-escape-warns-instead-of-decoding}.
                    case 'u':
                        std::printf("  WARN  \\uXXXX escape is not supported; "
                                    "write the character directly in the JSON\n");
                        v.str += "\\u";           // keep it visible rather than half-decoding
                        p++;
                        break;
                    default:   if (*p) v.str += *p++; break;
                }
            }
            else v.str += *p++;
        }
        if (*p == '"') p++;
        return v;
    }

    JsonVal parseNumber() {
        JsonVal v; v.type = JsonVal::Number;
        const char* start = p;
        if (*p == '-') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
        v.num = std::strtod(start, nullptr);
        return v;
    }

    JsonVal parseBool() {
        JsonVal v; v.type = JsonVal::Bool;
        if (*p == 't') { v.boolean = true; p += 4; }
        else { v.boolean = false; p += 5; }
        return v;
    }

    JsonVal parseObject() {
        p++; // skip {
        JsonVal v; v.type = JsonVal::Object;
        skipWs();
        while (*p && *p != '}') {
            auto key = parseString();
            skipWs(); p++; skipWs(); // skip :
            v.obj[key.str] = parse();
            skipWs();
            if (*p == ',') { p++; skipWs(); }
        }
        if (*p == '}') p++;
        return v;
    }

    JsonVal parseArray() {
        p++; // skip [
        JsonVal v; v.type = JsonVal::Array;
        skipWs();
        while (*p && *p != ']') {
            v.arr.push_back(parse());
            skipWs();
            if (*p == ',') { p++; skipWs(); }
        }
        if (*p == ']') p++;
        return v;
    }
};

static JsonVal parseJson(const std::string& text) {
    JsonParser parser{text.c_str()};
    return parser.parse();
}

static std::string readFile(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}



// Target key for the per-step expected[<target>] lookup: @xref{why-the-target-key-is-always-the-host-desktop}.
static const char* hostTarget() {
#if defined(__APPLE__)
    return "desktop-macos";
#elif defined(_WIN32)
    return "desktop-windows";
#elif defined(__linux__)
    return "desktop-linux";
#else
    return "desktop-unknown";
#endif
}

// Apply a set_control step in-process, true when the write applied and false on a lookup miss or unsupported type: @xref{how-a-scenario-value-reaches-a-control}.
static bool applySetControl(mm::Scheduler& scheduler,
                            mm::MoonModule* target,
                            const char* controlName,
                            const JsonVal& value) {
    if (!target || !controlName) return false;
    auto& controls = target->controls();
    for (uint8_t i = 0; i < controls.count(); i++) {
        const auto& c = controls[i];
        if (!c.name || std::strcmp(c.name, controlName) != 0) continue;
        // Bridge JsonVal to raw JSON text to mm::applyControlValue, under a strict policy: @xref{how-a-scenario-value-reaches-a-control}.
        mm::JsonSink wrapper;
        wrapper.append("{\"v\":");
        switch (value.type) {
            case JsonVal::Number: wrapper.writeNumber(value.num); break;
            case JsonVal::Bool:   wrapper.writeBool(value.boolean); break;
            case JsonVal::String: wrapper.writeJsonString(value.str.c_str()); break;
            default:              wrapper.append("null"); break;
        }
        wrapper.append("}");
        mm::ApplyResult r = mm::applyControlValue(c, wrapper.data(), "v",
                                                  mm::ApplyPolicy::Strict);
        if (r != mm::ApplyResult::Ok) return false;
        if (c.type == mm::ControlType::Select) target->rebuildControls();
        target->onControlChanged(controlName);
        if (target->affectsPrepare(controlName)) {
            scheduler.prepareTree();
        }
        return true;
    }
    return false;
}

// Module registry for scenario replay
struct ScenarioContext {
    mm::Scheduler scheduler;
    std::map<std::string, mm::MoonModule*> modules;

    // Modules are heap-allocated by the factory; Scheduler::release owns and deletes them.
    mm::MoonModule* createModule(const char* type) {
        return mm::ModuleFactory::create(type);
    }

    // Erase every `modules` entry in `root`'s subtree, called BEFORE deleteTree(root): @xref{why-a-delete-purges-the-whole-subtree}.
    void purgeSubtree(mm::MoonModule* root) {
        if (!root) return;
        for (uint8_t i = 0; i < root->childCount(); i++) purgeSubtree(root->child(i));
        for (auto it = modules.begin(); it != modules.end();) {
            it = (it->second == root) ? modules.erase(it) : std::next(it);
        }
    }

    void wireModule(const char* type, const char* id, const JsonVal& step) {
        auto* mod = modules[id];
        if (!mod) return;

        // Wire parent/child
        if (step.has("parent_id")) {
            const char* parentId = step["parent_id"].c_str();
            auto* parent = modules[parentId];
            if (parent) {
                parent->addChild(mod);
            }
        }

        // Wire props (only when the step has any).
        if (step.has("props")) {
            auto& props = step["props"];
            if (std::strcmp(type, "Effects") == 0) {
                // Wire the container's Layouts, the self-healing path the device relies on: @xref{why-fixtures-wire-props-at-construct-time}.
                if (props.has("layouts")) {
                    auto* layoutsModule = static_cast<mm::Layouts*>(modules[props["layouts"].str]);
                    if (layoutsModule) static_cast<mm::Effects*>(mod)->setLayouts(layoutsModule);
                }
            } else if (std::strcmp(type, "Layer") == 0) {
                auto* layer = static_cast<mm::Layer*>(mod);
                if (props.has("layouts")) {
                    auto* layoutsModule = static_cast<mm::Layouts*>(modules[props["layouts"].str]);
                    if (layoutsModule) layer->setLayouts(layoutsModule);
                }
                if (props.has("channelsPerLight")) {
                    layer->setChannelsPerLight(static_cast<uint8_t>(props["channelsPerLight"].num));
                }
            } else if (std::strcmp(type, "Drivers") == 0) {
                // Prefer binding the Effects container, pinning a Layer being the fallback: @xref{why-fixtures-wire-props-at-construct-time}.
                if (props.has("effects")) {
                    auto* effectsModule = static_cast<mm::Effects*>(modules[props["effects"].str]);
                    if (effectsModule) static_cast<mm::Drivers*>(mod)->setEffects(effectsModule);
                } else if (props.has("layer")) {
                    auto* layerModule = static_cast<mm::Layer*>(modules[props["layer"].str]);
                    if (layerModule) static_cast<mm::Drivers*>(mod)->setLayer(layerModule);
                }
            } else if (std::strcmp(type, "GridLayout") == 0) {
                // Grid dimensions are set at construct time: @xref{why-fixtures-wire-props-at-construct-time}.
                auto* grid = static_cast<mm::GridLayout*>(mod);
                if (props.has("width"))  grid->width  = static_cast<mm::lengthType>(props["width"].num);
                if (props.has("height")) grid->height = static_cast<mm::lengthType>(props["height"].num);
                if (props.has("depth"))  grid->depth  = static_cast<mm::lengthType>(props["depth"].num);
            } else if (std::strcmp(type, "GridBlacksLayout") == 0) {
                // The same construct-time dimension apply as GridLayout: @xref{why-fixtures-wire-props-at-construct-time}.
                auto* grid = static_cast<mm::GridBlacksLayout*>(mod);
                if (props.has("width"))  grid->width  = static_cast<mm::lengthType>(props["width"].num);
                if (props.has("height")) grid->height = static_cast<mm::lengthType>(props["height"].num);
                if (props.has("depth"))  grid->depth  = static_cast<mm::lengthType>(props["depth"].num);
            }
        }

        // PreviewDriver needs no scenario-specific wiring: @xref{why-fixtures-wire-props-at-construct-time}.
    }
};

static constexpr int WARMUP_FRAMES = 10;

/// How long a measurement runs, in the wall clock the effects read, long enough to cover one whole interval of the slowest shipped default: @xref{what-a-measurement-covers}.
static constexpr uint32_t MEASURE_WINDOW_MS = 1600;

/// A ceiling on one window's frames, so a pathologically fast tick cannot spin without bound, sized above what the window can reach on the fastest host: @xref{what-a-measurement-covers}.
static constexpr int MEASURE_FRAME_CAP = 8000000;

struct Result {
    bool passed = true;
    int checks = 0;
    int failures = 0;

    void check(bool condition, const char* name) {
        checks++;
        if (condition) {
            std::printf("  PASS  %s\n", name);
        } else {
            std::printf("  FAIL  %s\n", name);
            passed = false;
            failures++;
        }
    }
};

/// What runScenario returns for a scenario that did not run: @xref{what-a-green-run-is-allowed-to-mean}.
static constexpr int kSkipped = 2;

static int runScenario(const char* path) {
    // The firmware's own type registry, so a scenario can name any module a device can: src/module_types.cpp. Idempotent, so calling it per scenario is a no-op after the first.
    mm::registerModuleTypes();

    std::string text = readFile(path);
    if (text.empty()) {
        std::printf("Cannot read scenario file: %s\n", path);
        return 1;
    }

    auto scenario = parseJson(text);
    std::printf("=== Scenario: %s ===\n", scenario["name"].c_str());
    std::printf("%s\n", scenario["description"].c_str());
    std::printf("Target: %s\n\n", hostTarget());

    // Honour a scenario-level `skip_on` allowlist of host targets: @xref{why-skip-on-exists}.
    if (scenario.has("skip_on")) {
        for (auto& t : scenario["skip_on"].arr) {
            if (t.str == hostTarget()) {
                std::printf("  SKIP (skip_on %s)\n---\nSKIPPED\n", hostTarget());
                return kSkipped;
            }
        }
    }

    // The mode field says what shape the scenario expects the world to be in, defaulting to construct: @xref{the-construct-and-mutate-modes}.
    std::string mode = scenario.has("mode") ? scenario["mode"].str : std::string("construct");

    if (mode == "mutate") {
        // In-process replays the fixture before the scenario's actual steps: @xref{the-construct-and-mutate-modes}.
        if (!scenario.has("fixture") || scenario["fixture"].arr.empty()) {
            std::printf("  SKIP (mutate scenario with no fixture — runs live only)\n");
            return kSkipped;
        }
    } else if (mode != "construct") {
        std::printf("  FAIL — unknown mode: %s (expected construct or mutate)\n", mode.c_str());
        return 1;
    }

    // Legacy tier flag: live_only still honoured for any scenario that uses it. Newer scenarios should prefer mode=mutate (with/without fixture) instead.
    if (scenario.has("live_only") && scenario["live_only"].boolean) {
        std::printf("  SKIP (live_only)\n");
        return kSkipped;
    }

    ScenarioContext ctx;
    Result result;

    // Lazy-setup model, processing steps in order: @xref{why-the-scheduler-starts-lazily}.
    bool schedulerStarted = false;
    size_t heapBefore = mm::platform::freeHeap();
    size_t heapAfter = heapBefore;       // updated on setup + after every measure
    auto ensureStarted = [&]() {
        if (schedulerStarted) return;
        ctx.scheduler.setup();
        schedulerStarted = true;
        heapAfter = mm::platform::freeHeap();
        if (heapBefore > 0) {
            long delta = static_cast<long>(heapBefore) - static_cast<long>(heapAfter);
            std::printf("\n  Heap: %u → %u (pipeline: %ld bytes)\n",
                        static_cast<unsigned>(heapBefore),
                        static_cast<unsigned>(heapAfter), delta);
        }
        std::printf("  Memory:\n");
        for (uint8_t m = 0; m < ctx.scheduler.moduleCount(); m++) {
            auto* mod = ctx.scheduler.module(m);
            if (mod) printModuleMemory(mod, 2);
        }
    };

    // Three sections in order, fixture then reset then steps: @xref{the-fixture-reset-and-steps-sections}.
    std::vector<const JsonVal*> allSteps;
    size_t fixtureSize = 0, resetSize = 0;
    if (scenario.has("fixture")) {
        for (auto& s : scenario["fixture"].arr) allSteps.push_back(&s);
        fixtureSize = scenario["fixture"].arr.size();
    }
    if (scenario.has("reset")) {
        for (auto& s : scenario["reset"].arr) allSteps.push_back(&s);
        resetSize = scenario["reset"].arr.size();
    }
    for (auto& s : scenario["steps"].arr) allSteps.push_back(&s);
    enum class Section { Fixture, Reset, Steps };
    Section section = fixtureSize > 0 ? Section::Fixture
                    : resetSize > 0   ? Section::Reset
                    :                   Section::Steps;
    if (section == Section::Fixture) {
        std::printf("  --- fixture (%u steps) ---\n", static_cast<unsigned>(fixtureSize));
    } else if (section == Section::Reset) {
        std::printf("  --- reset (%u steps) ---\n", static_cast<unsigned>(resetSize));
    }

    for (size_t stepIdx = 0; stepIdx < allSteps.size(); stepIdx++) {
        const JsonVal& step = *allSteps[stepIdx];
        // Section boundary banners + lazy scheduler start.
        if (section == Section::Fixture && stepIdx == fixtureSize) {
            // Fixture done, so start the scheduler and set_control works: @xref{why-the-scheduler-starts-lazily}.
            ensureStarted();
            section = resetSize > 0 ? Section::Reset : Section::Steps;
            std::printf(section == Section::Reset
                        ? "  --- reset (%u steps) ---\n"
                        : "  --- steps ---\n",
                        static_cast<unsigned>(resetSize));
        }
        if (section == Section::Reset && stepIdx == fixtureSize + resetSize) {
            section = Section::Steps;
            std::printf("  --- steps ---\n");
        }
        const char* name = step["name"].c_str();
        const char* op = step["op"].c_str();

        if (std::strcmp(op, "add_module") == 0) {
            const char* type = step["type"].c_str();
            const char* id = step["id"].c_str();

            auto* mod = ctx.createModule(type);
            if (!mod) {
                // A failed create is a failed scenario: @xref{why-an-unknown-module-type-fails}.
                if (step.has("optional") && step["optional"].boolean) {
                    std::printf("  SKIP  %s (optional, type %s unavailable here)\n", name, type);
                    continue;
                }
                std::printf("  ADD   %s — unknown type: %s\n", name, type);
                result.check(false, name);
                continue;
            }
            mod->setName(id);
            ctx.modules[id] = mod;
            ctx.wireModule(type, id, step);

            // Only register top-level modules (no parent_id) with scheduler
            if (!step.has("parent_id")) {
                ctx.scheduler.addModule(mod);
            }

            // Mid-scenario adds set up the new module at once and rebuild pipeline state: @xref{why-the-scheduler-starts-lazily}.
            if (schedulerStarted) {
                mod->defineControls();
                mod->setup();
                ctx.scheduler.prepareTree();
            }
            std::printf("  +     %s (%s)\n", id, type);
        } else if (std::strcmp(op, "set_control") == 0) {
            if (!step.has("id") || !step.has("key")) {
                std::printf("  SET   %s — missing id/key, skipped\n", name);
                continue;
            }
            const char* targetId = step["id"].c_str();
            const char* key = step["key"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target) {
                std::printf("  SET   %s — module %s not found, skipped\n", name, targetId);
                continue;
            }
            if (!applySetControl(ctx.scheduler, target, key, step["value"])) {
                std::printf("  SET   %s — control %s.%s not applied\n", name, targetId, key);
            } else {
                std::printf("  SET   %s (%s.%s)\n", name, targetId, key);
            }
        } else if (std::strcmp(op, "reboot") == 0) {
            // The scheduler IS the process here, so a restart belongs to the live tier and says so rather than pretending.
            std::printf("  REBOOT %s — skipped (no process to restart in-process)\n", name);
        } else if (step.has("optional") && step["optional"].boolean
                   && std::strcmp(op, "add_module") != 0 && std::strcmp(op, "set_control") != 0
                   && std::strcmp(op, "measure") != 0 && std::strcmp(op, "remove_module") != 0
                   && std::strcmp(op, "delete_module") != 0) {
            // `optional` is honoured by the ops above and nowhere else: @xref{why-an-unknown-module-type-fails}.
            std::printf("  %s %s — `optional` does nothing on this op\n", op, name);
            result.check(false, name);
        } else if (std::strcmp(op, "expect_file") == 0) {
            // Read a file back, the only way to prove a write reached the filesystem rather than a cache.
            if (!step.has("path")) {
                std::printf("  EXPECT %s — missing path\n", name);
                result.check(false, name);
                continue;
            }
            const char* filePath = step["path"].c_str();
            // `contains` matches a substring and `equals` the whole file, since a step that meant one and got the other would pass on a file it never described.
            const bool exact = step.has("equals");
            if (!exact && !step.has("contains")) {
                std::printf("  EXPECT %s — needs `contains` or `equals`\n", filePath);
                result.check(false, name);
                continue;
            }
            const std::string want = exact ? step["equals"].str : step["contains"].str;
            // One byte of headroom over the cap, so a read that fills it proves the file is longer rather than exactly this size.
            char buf[4096] = {};
            const int got = mm::platform::fsRead(filePath, buf, sizeof(buf));
            const std::string have(buf, got > 0 ? static_cast<size_t>(got) : 0);
            // A truncated read cannot answer `equals`, which is about the whole file, so it fails as unevaluable rather than comparing a prefix.
            if (got >= static_cast<int>(sizeof(buf) - 1) && exact) {
                std::printf("  EXPECT %s — file exceeds %u bytes, so `equals` cannot be evaluated\n",
                            filePath, static_cast<unsigned>(sizeof(buf) - 1));
                result.check(false, name);
                continue;
            }
            const bool holds = got >= 0 && (exact ? have == want
                                                  : !want.empty() && have.find(want) != std::string::npos);
            std::printf(holds ? "  EXPECT %s %s \"%s\"\n" : "  EXPECT %s does not %s \"%s\"\n",
                        filePath, exact ? "is" : "hold", want.c_str());
            result.check(holds, name);
        } else if (std::strcmp(op, "expect_control") == 0) {
            // The only op that fails on a VALUE rather than a timing contract, and `not_equals` is its negation: @xref{what-a-green-run-is-allowed-to-mean}.
            const bool negated = !step.has("equals") && step.has("not_equals");
            if (!step.has("id") || !step.has("key") || (!step.has("equals") && !negated)) {
                std::printf("  EXPECT %s — missing id/key/equals\n", name);
                result.check(false, name);
                continue;
            }
            const char* targetId = step["id"].c_str();
            const char* key = step["key"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target) {
                std::printf("  EXPECT %s — module %s not found\n", name, targetId);
                result.check(false, name);
                continue;
            }
            const mm::ControlDescriptor* found = nullptr;
            for (uint8_t i = 0; i < target->controls().count(); i++) {
                if (std::strcmp(target->controls()[i].name, key) == 0) { found = &target->controls()[i]; break; }
            }
            if (!found) {
                std::printf("  EXPECT %s — %s has no control %s\n", name, targetId, key);
                result.check(false, name);
                continue;
            }
            char rendered[256] = {};
            {
                mm::JsonSink sink(rendered, sizeof(rendered));
                mm::writeControlValue(sink, *found);
            }
            // The rendering quotes a string, which a scenario should not have to write.
            const char* actual = rendered;
            std::string unquoted;
            if (rendered[0] == '"') {
                unquoted.assign(rendered + 1);
                if (!unquoted.empty() && unquoted.back() == '"') unquoted.pop_back();
                actual = unquoted.c_str();
            }
            // A JSON number carries no `str`, so an unquoted `equals` would assert against "".
            const std::string want = asWritten(step[negated ? "not_equals" : "equals"]);
            const bool same = (want == actual);
            const bool holds = negated ? !same : same;
            if (holds) std::printf("  EXPECT %s (%s.%s %s %s)\n", name, targetId, key,
                                   negated ? "!=" : "==", negated ? want.c_str() : actual);
            else       std::printf("  EXPECT %s — %s.%s is \"%s\", expected %s\"%s\"\n",
                                   name, targetId, key, actual, negated ? "not " : "", want.c_str());
            result.check(holds, name);
        } else if (std::strcmp(op, "write_file") == 0) {
            // Stage a file the way the UI's editor does, a malformed step being a failed scenario: @xref{why-write-file-exists}.
            if (!step.has("path") || !step.has("value")) {
                std::printf("  WRITE %s — missing path/value\n", name);
                result.check(false, name);
                continue;
            }
            const char* filePath = step["path"].c_str();
            const std::string body = step["value"].str;
            // mkdir -p the parent, which a fresh build tree may not have yet: @xref{why-write-file-exists}.
            if (const char* slash = std::strrchr(filePath, '/')) {
                if (slash != filePath) {
                    std::string dir(filePath, static_cast<size_t>(slash - filePath));
                    mm::platform::fsMkdir(dir.c_str());
                }
            }
            // A FAILED write is a failed scenario rather than a printed note: @xref{why-write-file-exists}.
            const bool wrote = mm::platform::fsWriteAtomic(filePath, body.c_str(), body.size());
            if (wrote) {
                std::printf("  WRITE %s (%s, %zu bytes)\n", name, filePath, body.size());
            } else {
                std::printf("  WRITE %s — write to %s FAILED\n", name, filePath);
            }
            result.check(wrote, name);
        } else if (std::strcmp(op, "remove_module") == 0 || std::strcmp(op, "delete_module") == 0) {
            // `remove_module` and `delete_module` are aliases, and both remove a child from its parent: @xref{why-remove-module-and-delete-module-are-aliases}.
            const char* targetId = step["id"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target || !target->parent() || !target->userEditable()) {
                // Mirror the live API: top-level and non-editable submodules stay: @xref{why-remove-module-and-delete-module-are-aliases}.
                std::printf("  -     %s — %s not found / top-level / not editable, skipped\n", name, targetId);
                continue;
            }
            auto* parent = target->parent();
            parent->removeChild(target);
            target->release();
            ctx.purgeSubtree(target);  // erase target + any registered descendants before freeing
            mm::Scheduler::deleteTree(target);
            if (schedulerStarted) ctx.scheduler.prepareTree();
            std::printf("  -     %s (%s)\n", name, targetId);
        } else if (std::strcmp(op, "clear_children") == 0) {
            // Delete every child of a container, leaving the container itself: @xref{why-clear-children-exists}.
            const char* targetId = step["id"].c_str();
            auto* container = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!container) {
                std::printf("  clr     %s — container %s not found, skipped\n", name, targetId);
                continue;
            }
            int cleared = 0;
            for (uint8_t i = container->childCount(); i > 0; i--) {
                mm::MoonModule* childMod = container->child(i - 1);
                // Mirror handleDeleteModule: a non-editable submodule is apparatus, so skip it: @xref{why-clear-children-exists}.
                if (!childMod->userEditable()) continue;
                container->removeChild(childMod);
                childMod->release();
                // Purge the child AND any registered descendants before freeing: @xref{why-a-delete-purges-the-whole-subtree}.
                ctx.purgeSubtree(childMod);
                mm::Scheduler::deleteTree(childMod);
                cleared++;
            }
            if (schedulerStarted) ctx.scheduler.prepareTree();
            std::printf("  clr     %s (%s: %d cleared)\n", name, targetId, cleared);
        } else if (std::strcmp(op, "replace_module") == 0) {
            // Replace a child with a fresh module of another type at the same slot: @xref{why-remove-module-and-delete-module-are-aliases}.
            const char* targetId = step["id"].c_str();
            const char* newType = step["type"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target || !target->parent() || !target->userEditable()) {
                // Mirror the live API: a top-level or non-editable submodule stays: @xref{why-remove-module-and-delete-module-are-aliases}.
                std::printf("  ~     %s — %s not found / top-level / not editable, skipped\n", name, targetId);
                continue;
            }
            auto* parent = target->parent();
            uint8_t index = 0; bool found = false;
            for (uint8_t i = 0; i < parent->childCount(); i++) {
                if (parent->child(i) == target) { index = i; found = true; break; }
            }
            auto* fresh = ctx.createModule(newType);
            if (!found || !fresh) {
                if (fresh) mm::Scheduler::deleteTree(fresh);
                std::printf("  ~     %s — slot not found or unknown type %s, skipped\n", name, newType);
                continue;
            }
            fresh->setName(targetId);
            mm::MoonModule* old = parent->replaceChildAt(index, fresh);
            fresh->defineControls();
            fresh->setup();
            fresh->prepare();
            if (old) {
                // Purge any ctx.modules entry pointing at old or a descendant before freeing, targetId re-registering to fresh below: @xref{why-a-delete-purges-the-whole-subtree}.
                ctx.purgeSubtree(old);
                old->release();
                mm::Scheduler::deleteTree(old);
            }
            ctx.modules[targetId] = fresh;
            if (schedulerStarted) ctx.scheduler.prepareTree();
            std::printf("  ~     %s (%s → %s)\n", name, targetId, newType);
        } else if (std::strcmp(op, "measure") == 0) {
            // Pure measurement step with no side effects: @xref{why-a-measure-step-has-two-spellings}.
            std::printf("  ...   %s\n", name);
        }

        // Per-step measurement: warmup, measure, bounded assertions: @xref{why-a-measure-step-has-two-spellings}.
        const bool isMeasure = (step.has("measure") && step["measure"].boolean)
                            || std::strcmp(op, "measure") == 0;
        if (isMeasure) {
            ensureStarted();
            double fpsBound = 0;
            double fpsLedProduct = 0;
            if (step.has("bounds") && step["bounds"].has("fps")) {
                if (step["bounds"]["fps"].has("min"))
                    fpsBound = step["bounds"]["fps"]["min"].num;
                else if (step["bounds"]["fps"].has("min_pct")) {
                    // min_pct needs a live baseline, so log a clear skip: @xref{why-a-measure-step-has-two-spellings}.
                    double pct = step["bounds"]["fps"]["min_pct"].num;
                    std::printf("  WARN  %s: bounds.fps.min_pct=%g requires a live "
                                "baseline; in-process runner cannot enforce — skipped\n",
                                name, pct);
                    fpsBound = 0;
                }
                if (step["bounds"]["fps"].has("min_fps_led_product"))
                    fpsLedProduct = step["bounds"]["fps"]["min_fps_led_product"].num;
            }
            long maxHeapDelta = 0;
            bool hasHeapBound = false;
            if (step.has("bounds") && step["bounds"].has("heap") &&
                step["bounds"]["heap"].has("max_delta_bytes")) {
                maxHeapDelta = static_cast<long>(step["bounds"]["heap"]["max_delta_bytes"].num);
                hasHeapBound = true;
            }

            for (int i = 0; i < WARMUP_FRAMES; i++) ctx.scheduler.tick();
            size_t heapBeforeMeasure = mm::platform::freeHeap();
            uint32_t startUs = mm::platform::micros();
            // Ticking for a SPAN rather than a count, so the window covers real animation: @xref{what-a-measurement-covers}.
            const uint32_t windowUs = MEASURE_WINDOW_MS * 1000u;
            int frames = 0;
            uint32_t elapsedUs = 0;
            while (elapsedUs < windowUs && frames < MEASURE_FRAME_CAP) {
                ctx.scheduler.tick();
                frames++;
                elapsedUs = mm::platform::micros() - startUs;
            }
            // The cap ends the loop before the clock does, so the sample covers less animation than a window, and that is SAID rather than passed off as a full span.
            if (frames >= MEASURE_FRAME_CAP && elapsedUs < windowUs) {
                std::printf("  NOTE  frame cap reached after %ums of a %ums window\n",
                            elapsedUs / 1000u, MEASURE_WINDOW_MS);
            }
            // Rounded rather than floored: a desktop tick is a fraction of a microsecond, and flooring reported 0 or 1 for everything, which is what made the trend unreadable.
            uint32_t tickTimeUs = frames > 0
                ? (elapsedUs + static_cast<uint32_t>(frames) / 2) / static_cast<uint32_t>(frames)
                : 0;
            // FPS from the undivided numbers, so it keeps its precision where the rounded tick has lost it.
            const uint32_t fpsFromSpan = elapsedUs > 0
                ? static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000u) / elapsedUs)
                : 0;
            uint32_t fps = fpsFromSpan;
            size_t heapAfterMeasure = mm::platform::freeHeap();
            // Largest contiguous block in INTERNAL RAM, which diagnoses internal-heap fragmentation: @xref{why-the-block-size-is-internal-ram-only}.
            size_t maxBlock = mm::platform::maxInternalAllocBlock();

            // Buffer state at this measurement (may be empty in early build-up steps).
            auto* layer = static_cast<mm::Layer*>(
                ctx.modules.count("Layer") ? ctx.modules["Layer"] : nullptr);
            unsigned lights = layer ? static_cast<unsigned>(layer->buffer().count()) : 0;

            // `heap=` is the absolute free heap and `(step: ±N)` the signed step delta: @xref{what-the-heap-numbers-mean}.
            long stepDelta = heapBefore > 0
                ? static_cast<long>(heapAfter) - static_cast<long>(heapAfterMeasure)
                : 0;
            std::printf("  MEASURE %s: tick=%uus FPS=%u lights=%u heap=%u (step: %+ld) block=%u\n",
                        name,
                        static_cast<unsigned>(tickTimeUs), static_cast<unsigned>(fps),
                        lights, static_cast<unsigned>(heapAfterMeasure), stepDelta,
                        static_cast<unsigned>(maxBlock));
            (void)heapBeforeMeasure;  // tracked through stepDelta above

            // FPS bound (when set)
            if (fpsBound > 0) {
                char msg[96];
                std::snprintf(msg, sizeof(msg), "%s fps >= %.0f", name, fpsBound);
                result.check(fps >= static_cast<float>(fpsBound), msg);
            }
            // FPS×lights throughput floor, compared against the measured tick
            // *time* (native unit), not derived FPS.
            if (fpsLedProduct > 0 && lights > 0) {
                double maxTickUs = lights * 1000000.0 / fpsLedProduct;
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s tick <= %.0fus (%u lights, throughput floor)",
                              name, maxTickUs, lights);
                result.check(static_cast<double>(tickTimeUs) <= maxTickUs, msg);
            }
            // Heap-delta bound, which catches a leak or an unintended alloc: @xref{what-the-heap-numbers-mean}.
            if (hasHeapBound && heapBefore > 0) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s heap step delta %+ld <= %ld bytes",
                              name, stepDelta, maxHeapDelta);
                result.check(stepDelta <= maxHeapDelta, msg);
            }

            // Per-step performance contract, optional and shared with the live runner: @xref{the-performance-contract}.
            if (step.has("contract") && step["contract"].has(hostTarget())) {
                const auto& exp = step["contract"][hostTarget()];
                // Per-target defaults reflect run-to-run variance, and KEEP IN SYNC with the live runner: @xref{why-the-tolerance-defaults-differ-per-target}.
                const bool isDesktop = std::strncmp(hostTarget(), "desktop-", 8) == 0;
                double tickTolPct = exp.has("tick_tolerance_pct") ? exp["tick_tolerance_pct"].num
                                                                   : (isDesktop ? 20.0 : 10.0);
                double heapTolPct = exp.has("heap_tolerance_pct") ? exp["heap_tolerance_pct"].num
                                                                   : (isDesktop ? 20.0 : 10.0);
                // Absolute floor, since OS scheduling jitter dwarfs a percentage at a small tick: @xref{why-the-tolerance-defaults-differ-per-target}.
                double tolUs = exp.has("tolerance_us") ? exp["tolerance_us"].num
                                                       : (isDesktop ? 200.0 : 5.0);
                if (exp.has("tick_us") && exp["tick_us"].num > 0) {
                    // tick is a *ceiling*, so a speedup never fails: @xref{the-performance-contract}.
                    double expTick = exp["tick_us"].num;
                    double overshoot = static_cast<double>(tickTimeUs) - expTick;
                    double allowed = expTick * tickTolPct / 100.0;
                    if (allowed < tolUs) allowed = tolUs;
                    char msg[200];
                    if (overshoot <= 0) {
                        std::snprintf(msg, sizeof(msg),
                                      "%s tick %uus <= contract %.0fus (margin %.0fus)",
                                      name, static_cast<unsigned>(tickTimeUs), expTick, -overshoot);
                        result.check(true, msg);
                    } else {
                        std::snprintf(msg, sizeof(msg),
                                      "%s tick %uus vs contract %.0fus (over by %.0fus <= %.0fus)",
                                      name, static_cast<unsigned>(tickTimeUs), expTick, overshoot, allowed);
                        result.check(overshoot <= allowed, msg);
                    }
                }
                // free_heap is a *floor*, meaningful on ESP32 and unlimited on desktop: @xref{the-performance-contract}.
                if (exp.has("free_heap") && exp["free_heap"].num > 0 &&
                    heapAfterMeasure > 0) {
                    double expHeap = exp["free_heap"].num;
                    double dropPct = (heapAfterMeasure < expHeap)
                        ? (expHeap - heapAfterMeasure) * 100.0 / expHeap
                        : 0.0;
                    char msg[180];
                    std::snprintf(msg, sizeof(msg),
                                  "%s free_heap %u vs contract %.0f (drop %.1f%% <= %.0f%%)",
                                  name, static_cast<unsigned>(heapAfterMeasure), expHeap, dropPct, heapTolPct);
                    result.check(dropPct <= heapTolPct, msg);
                }
                // max_alloc_block is also a *floor*, optional and skipped on desktop: @xref{the-performance-contract}.
                if (exp.has("max_alloc_block") && exp["max_alloc_block"].num > 0 &&
                    maxBlock > 0) {
                    double expBlock = exp["max_alloc_block"].num;
                    double dropPct = (maxBlock < expBlock)
                        ? (expBlock - maxBlock) * 100.0 / expBlock
                        : 0.0;
                    char msg[200];
                    std::snprintf(msg, sizeof(msg),
                                  "%s max_alloc_block %u vs contract %.0f (drop %.1f%% <= %.0f%%)",
                                  name, static_cast<unsigned>(maxBlock), expBlock, dropPct, heapTolPct);
                    result.check(dropPct <= heapTolPct, msg);
                }
            }

            heapAfter = heapAfterMeasure;  // roll forward for the next step's delta
        }
    }

    // What the scenario's own steps asserted, since the buffer checks below run for any scenario with a Layer and would let steps that assert nothing satisfy the floor.
    const int stepChecks = result.checks;

    // The legacy end-of-scenario buffer check runs when a Layer is present, since existing scenarios depend on it.
    ensureStarted();
    auto* layer = static_cast<mm::Layer*>(
        ctx.modules.count("Layer") ? ctx.modules["Layer"] : nullptr);
    auto* drivers = static_cast<mm::Drivers*>(
        ctx.modules.count("Drivers") ? ctx.modules["Drivers"] : nullptr);
    if (layer) {
        auto& buf = layer->buffer();
        result.check(buf.data() != nullptr, "buffer allocated");
        result.check(buf.count() > 0, "buffer has lights");
        std::printf("  Buffer: %u lights, %u bytes  LUT: %s  dynamicBytes: Layer=%u Drivers=%u\n",
                    static_cast<unsigned>(buf.count()),
                    static_cast<unsigned>(buf.bytes()),
                    layer->lut().hasLUT() ? "has LUT" : "identity",
                    static_cast<unsigned>(layer->dynamicBytes()),
                    static_cast<unsigned>(drivers ? drivers->dynamicBytes() : 0));
        bool hasNonZero = false;
        if (buf.data()) {
            for (size_t i = 0; i < buf.bytes(); i++) {
                if (buf.data()[i] != 0) { hasNonZero = true; break; }
            }
        }
        // Only assert non-zero output if the scenario actually rendered (lights > 0).
        if (buf.count() > 0) {
            result.check(hasNonZero, "buffer non-zero after render");
        }
    }

    ctx.scheduler.release();

    // A scenario whose own steps asserted nothing is a failure, since it is indistinguishable from one whose steps all silently did nothing.
    if (stepChecks == 0) {
        std::printf("---\nFAILED (ran %u step(s) and asserted nothing)\n",
                    static_cast<unsigned>(allSteps.size()));
        return 1;
    }

    // Summary
    std::printf("---\n");
    if (result.passed) {
        std::printf("PASSED (%d checks)\n", result.checks);
    } else {
        std::printf("FAILED (%d/%d checks)\n", result.failures, result.checks);
    }
    return result.passed ? 0 : 1;
}

// Directory iteration can throw filesystem_error, which escapes main deliberately: @xref{why-a-filesystem-error-escapes-main}. NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Run all scenarios in the scenarios/ directory tree, recursively: @xref{why-a-filesystem-error-escapes-main}.
        int failed = 0;
        int skipped = 0;
        int total = 0;
        for (auto& entry : std::filesystem::recursive_directory_iterator("test/scenarios")) {
            if (entry.path().extension() == ".json") {
                total++;
                // path::c_str() is wchar_t* on Windows, so round-trip through .string() for a portable narrow-char view.
                const int rc = runScenario(entry.path().string().c_str());
                if (rc == kSkipped) skipped++;
                else if (rc != 0) failed++;
                std::printf("\n");
            }
        }
        // Skipped is reported on its own line: a run that tested nothing must not read as a clean pass.
        std::printf("=== %d scenario(s), %d passed, %d failed, %d skipped ===\n",
                    total, total - failed - skipped, failed, skipped);
        return failed > 0 ? 1 : 0;
    }

    return runScenario(argv[1]);
}
