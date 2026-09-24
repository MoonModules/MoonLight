/// Scenario runner: reads scenario JSON files, replays steps in-process.
/// When HTTP API is added, the same JSON files work with a Python runner against a live system.

#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Control.h"
#include "core/util/JsonSink.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/GridBlacksLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/layouts/RectangleLayout.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Effects.h"
#include "light/effects/LinesEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "light/moonlive/MoonLiveModifier.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "light/effects/AuroraEffect.h"
#include "light/effects/FluidEffect.h"
#include "light/effects/NebulaEffect.h"
#include "light/effects/TrailsEffect.h"
#include "light/effects/ColorTrailsEffect.h"
#include "light/effects/PolarNoiseEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/CheckerboardModifier.h"
#include "light/modifiers/RegionModifier.h"
#include "light/modifiers/RotateModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/PreviewDriver.h"
#include "core/system/SystemModule.h"
#include "core/services/AudioService.h"
#include "light/effects/RadialSpectrumEffect.h"
#include "light/effects/VuMetersEffect.h"
#include "light/effects/BeatRipplesEffect.h"
#include "core/services/VideoService.h"
#include "core/services/Services.h"
#include "light/effects/AmbilightEffect.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
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
            // Decode the escape rather than dropping the backslash. Appending the next character raw turned "\n" into a literal 'n', so a multi-line string arrived as one line, invisible until write_file staged a script and the compiler reported "expected '(' after the function name" on a file that looked correct in the JSON.
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
                    // \uXXXX is NOT decoded, no scenario needs one, and a half-done UTF-16 surrogate decoder would be worse than not having one. But it must not pass through silently either: appending a literal 'u' would stage a script containing "u00e9" and the failure would surface as a confusing compile error in a file that looks right in the JSON. Say so, loudly, once.
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

// Register the module types this runner can replay. Heap-allocated by the factory (new T()) so Scheduler::release()'s deleteTree can validly delete them, same ownership model as production main.cpp. Idempotent: safe to call before every scenario.
static void registerScenarioTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<mm::Layouts>("Layouts");
    mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout");
    mm::ModuleFactory::registerType<mm::GridBlacksLayout>("GridBlacksLayout");
    mm::ModuleFactory::registerType<mm::SphereLayout>("SphereLayout");
    mm::ModuleFactory::registerType<mm::RectangleLayout>("RectangleLayout");
    mm::ModuleFactory::registerType<mm::Effects>("Effects");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::LinesEffect>("LinesEffect");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::PlasmaEffect>("PlasmaEffect");
    mm::ModuleFactory::registerType<mm::MetaballsEffect>("MetaballsEffect");
    mm::ModuleFactory::registerType<mm::FireEffect>("FireEffect");
    mm::ModuleFactory::registerType<mm::ParticlesEffect>("ParticlesEffect");
    mm::ModuleFactory::registerType<mm::MoonLiveEffect>("MoonLiveEffect");
    mm::ModuleFactory::registerType<mm::MoonLiveModifier>("MoonLiveModifier");
    mm::ModuleFactory::registerType<mm::MoonLiveLayout>("MoonLiveLayout");
    mm::ModuleFactory::registerType<mm::AuroraEffect>("AuroraEffect");
    mm::ModuleFactory::registerType<mm::FluidEffect>("FluidEffect");
    mm::ModuleFactory::registerType<mm::NebulaEffect>("NebulaEffect");
    mm::ModuleFactory::registerType<mm::TrailsEffect>("TrailsEffect");
    mm::ModuleFactory::registerType<mm::ColorTrailsEffect>("ColorTrailsEffect");
    mm::ModuleFactory::registerType<mm::PolarNoiseEffect>("PolarNoiseEffect");
    mm::ModuleFactory::registerType<mm::SpiralEffect>("SpiralEffect");
    mm::ModuleFactory::registerType<mm::RingsEffect>("RingsEffect");
    mm::ModuleFactory::registerType<mm::RipplesEffect>("RipplesEffect");
    mm::ModuleFactory::registerType<mm::LavaLampEffect>("LavaLampEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");
    mm::ModuleFactory::registerType<mm::CheckerboardModifier>("CheckerboardModifier");
    mm::ModuleFactory::registerType<mm::RegionModifier>("RegionModifier");
    mm::ModuleFactory::registerType<mm::RotateModifier>("RotateModifier");
    mm::ModuleFactory::registerType<mm::RandomMapModifier>("RandomMapModifier");
    mm::ModuleFactory::registerType<mm::Drivers>("Drivers");
    mm::ModuleFactory::registerType<mm::NetworkSendDriver>("NetworkSendDriver");
    mm::ModuleFactory::registerType<mm::PreviewDriver>("PreviewDriver");
    mm::ModuleFactory::registerType<mm::SystemModule>("SystemModule");
    mm::ModuleFactory::registerType<mm::Services>("Services");
    mm::ModuleFactory::registerType<mm::AudioService>("AudioService");
    mm::ModuleFactory::registerType<mm::RadialSpectrumEffect>("RadialSpectrumEffect");
    mm::ModuleFactory::registerType<mm::VuMetersEffect>("VuMetersEffect");
    mm::ModuleFactory::registerType<mm::BeatRipplesEffect>("BeatRipplesEffect");
    mm::ModuleFactory::registerType<mm::VideoService>("VideoService");
    mm::ModuleFactory::registerType<mm::AmbilightEffect>("AmbilightEffect");
    mm::ModuleFactory::registerType<mm::AudioSpectrumEffect>("AudioSpectrumEffect");
    mm::ModuleFactory::registerType<mm::GameOfLifeEffect>("GameOfLifeEffect");
    mm::ModuleFactory::registerType<mm::GEQ3DEffect>("GEQ3DEffect");
    mm::ModuleFactory::registerType<mm::PaintBrushEffect>("PaintBrushEffect");
    done = true;
}


// Target key for the per-step expected[<target>] lookup. The in-process runner builds for the host only, there's no cross-compiled scenario_runner, so the key is always desktop-<host-os>. Matches the run_live_scenario.py convention.
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

// Apply a set_control step in-process: find the module by id, find the control by name, write the typed value, then mirror what HttpServerModule::handleSetControl does, call onControlChanged(), and if affectsPrepare() returns true trigger Scheduler::prepareTree() so the pipeline reconciles. Returns true if the write applied; false on any lookup miss or unsupported type (caller may want to
static bool applySetControl(mm::Scheduler& scheduler,
                            mm::MoonModule* target,
                            const char* controlName,
                            const JsonVal& value) {
    if (!target || !controlName) return false;
    auto& controls = target->controls();
    for (uint8_t i = 0; i < controls.count(); i++) {
        const auto& c = controls[i];
        if (!c.name || std::strcmp(c.name, controlName) != 0) continue;
        // Bridge JsonVal → raw JSON text → mm::applyControlValue, so this file no longer hand-rolls the per-ControlType dispatch that Control.cpp owns. Build a tiny wrapper object `{"v":VALUE}` via JsonSink (heap-grow mode); writeNumber/writeBool/writeJsonString produce JSON-correct text per JsonVal::type. Re-serialize-then-parse cost is irrelevant in test code (≤100 set_control ops per scenario). Strict policy: out-of-range Uint8/Int16/Select fails the set_control so scenario-authoring bugs surface instead of silently clamping into a boundary value. This is stricter than the pre-refactor behavior for Uint8/Int16 (which silently clamped) but matches it for Select/IPv4 (which already failed); no existing scenario relied on the silent-clamp shape.
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

    // Erase every `modules` entry whose pointer lies in `root`'s subtree (root and all descendants), so deleting a module that has registered children (e.g. a Layer with an effect child) doesn't leave their ids pointing at freed memory. Call BEFORE deleteTree(root). Walks the live tree, so it must run while the subtree is still intact.
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
                // Wire the container's Layouts (mirrors main.cpp's effectsContainer->setLayouts). Effects re-propagates this to its child Effects at every prepareTree, so a Layer added later picks it up, the self-healing path the device relies on.
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
                // Prefer binding the Effects container (self-healing: the active Layer is re-resolved at every prepareTree, so a Layer cleared and rebuilt mid-scenario is picked up, mirrors main.cpp). Fall back to pinning a specific Layer for older fixtures.
                if (props.has("effects")) {
                    auto* effectsModule = static_cast<mm::Effects*>(modules[props["effects"].str]);
                    if (effectsModule) static_cast<mm::Drivers*>(mod)->setEffects(effectsModule);
                } else if (props.has("layer")) {
                    auto* layerModule = static_cast<mm::Layer*>(modules[props["layer"].str]);
                    if (layerModule) static_cast<mm::Drivers*>(mod)->setLayer(layerModule);
                }
            } else if (std::strcmp(type, "GridLayout") == 0) {
                // Grid dimensions set at construct time (the fixture phase runs before the scheduler starts, so set_control can't apply them yet). Without this, props.width/height were silently ignored and the grid stayed at GridLayout's default, masking the real scenario size.
                auto* grid = static_cast<mm::GridLayout*>(mod);
                if (props.has("width"))  grid->width  = static_cast<mm::lengthType>(props["width"].num);
                if (props.has("height")) grid->height = static_cast<mm::lengthType>(props["height"].num);
                if (props.has("depth"))  grid->depth  = static_cast<mm::lengthType>(props["depth"].num);
            } else if (std::strcmp(type, "GridBlacksLayout") == 0) {
                // Same construct-time dimension apply as GridLayout (the dark-column controls blackStart/blackCount are set later via set_control, which works post-start).
                auto* grid = static_cast<mm::GridBlacksLayout*>(mod);
                if (props.has("width"))  grid->width  = static_cast<mm::lengthType>(props["width"].num);
                if (props.has("height")) grid->height = static_cast<mm::lengthType>(props["height"].num);
                if (props.has("depth"))  grid->depth  = static_cast<mm::lengthType>(props["depth"].num);
            } else if (std::strcmp(type, "RectangleLayout") == 0) {
                // Same construct-time apply, since the perimeter is computed from these. The wiring controls stay on set_control, which works post-start.
                auto* rect = static_cast<mm::RectangleLayout*>(mod);
                if (props.has("width"))  rect->width  = static_cast<uint16_t>(props["width"].num);
                if (props.has("height")) rect->height = static_cast<uint16_t>(props["height"].num);
            }
        }

        // PreviewDriver needs no scenario-specific wiring: it reads its Layer + sparse source buffer through Drivers' passBufferToDrivers (set when a Drivers fixture exists), and owns its own scratch buffers. No broadcaster is wired here (the harness has no WS server), sendFrame / sendCoordTable early-return when the broadcaster is null, but the light-extraction work still runs for honest tick measurement.
    }
};

static constexpr int WARMUP_FRAMES = 10;
static constexpr int MEASURE_FRAMES = 200;

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

static int runScenario(const char* path) {
    registerScenarioTypes();

    std::string text = readFile(path);
    if (text.empty()) {
        std::printf("Cannot read scenario file: %s\n", path);
        return 1;
    }

    auto scenario = parseJson(text);
    std::printf("=== Scenario: %s ===\n", scenario["name"].c_str());
    std::printf("%s\n", scenario["description"].c_str());
    std::printf("Target: %s\n\n", hostTarget());

    // Honour a scenario-level `skip_on` allowlist of host targets that lack a capability the scenario exercises (today: MoonLive scenarios opt out on desktop-windows / desktop-linux, the desktop JIT is arm64-only, so an x86_64 host renders dark and the scenario's "buffer non-zero" check would fail for a platform-capability reason it isn't the right vehicle to assert). Absent / empty `skip_on` runs everywhere (the existing default). Same field the Python run_scenario.py honours; keeping the C++ runner in step so KPI collection (which calls mm_scenarios directly) doesn't count skipped scenarios as failures.
    if (scenario.has("skip_on")) {
        for (auto& t : scenario["skip_on"].arr) {
            if (t.str == hostTarget()) {
                std::printf("  SKIP (skip_on %s)\n---\nPASSED (skipped)\n", hostTarget());
                return 0;
            }
        }
    }

    // Mode field (construct/mutate) determines what shape the scenario expects the world to be in. See docs/reference/testing.md § Scenario modes.
    //   construct → scenario builds the pipeline from an empty scheduler; runs
    //               in-process only (live device's main.cpp owns the top-level
    //               shape; constructing fresh requires an empty scheduler that
    //               only the in-process runner can provide).
    //   mutate    → scenario assumes a wired pipeline. In-process replays the
    //               embedded `fixture` array first, then the steps. Live runs
    //               steps directly against whatever's wired.
    // Default: construct (back-compat with the existing scenarios that pre-date this field; they all build pipelines explicitly).
    // Bespoke convention: the construct/mutate split + fixture + reset trinity is projectMM-specific (no off-the-shelf BDD/scenario framework borrowed wholesale). It exists because the same JSON has to serve both an in-process runner (which owns the scheduler) and a live runner (which doesn't, main.cpp does). xUnit fixtures are the closest analog for `fixture`; SQL BEGIN/ROLLBACK is the closest for `reset`.
    std::string mode = scenario.has("mode") ? scenario["mode"].str : std::string("construct");

    if (mode == "mutate") {
        // In-process replays the fixture (an array of add_module steps in the same shape as `steps`) before running the scenario's actual steps. A mutate scenario without a fixture can still run live, the device is its own fixture, but cannot run in-process.
        if (!scenario.has("fixture") || scenario["fixture"].arr.empty()) {
            std::printf("  SKIP (mutate scenario with no fixture — runs live only)\n");
            return 0;
        }
    } else if (mode != "construct") {
        std::printf("  FAIL — unknown mode: %s (expected construct or mutate)\n", mode.c_str());
        return 1;
    }

    // Legacy tier flag: live_only still honoured for any scenario that uses it. Newer scenarios should prefer mode=mutate (with/without fixture) instead.
    if (scenario.has("live_only") && scenario["live_only"].boolean) {
        std::printf("  SKIP (live_only)\n");
        return 0;
    }

    ScenarioContext ctx;
    Result result;

    // Lazy-setup model: process steps in order. First measure step (or the end of the scenario, whichever comes first) flips the scheduler into setup+running mode. After that, mid-scenario add_module / set_control steps mutate the running pipeline, same shape as the live runner driving changes over REST. Per-step heap snapshots roll forward so each `measure: true` step reports its delta against the previous one.
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

    // Three sections in order: fixture (add_module shape, in-process only, builds the wired pipeline), reset (set_control to a known state, runs both tiers, makes the scenario start from the same place regardless of previous runs), steps (the actual scenario). Each section gets its own banner so the output is easy to scan.
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
            // Fixture done: start the scheduler so set_control works (controls are populated in defineControls during setup()).
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
                std::printf("  SKIP  %s (unknown type: %s)\n", name, type);
                continue;
            }
            mod->setName(id);
            ctx.modules[id] = mod;
            ctx.wireModule(type, id, step);

            // Only register top-level modules (no parent_id) with scheduler
            if (!step.has("parent_id")) {
                ctx.scheduler.addModule(mod);
            }

            // Mid-scenario adds: setup the new module immediately and rebuild pipeline state. Mirrors what HttpServerModule does on /api/modules.
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
        } else if (std::strcmp(op, "write_file") == 0) {
            // Stage a file the way the UI's editor does, so a scenario can drive the script loop end-to-end: write a script, point a module's `script` control at it, and measure. Same primitive the HTTP save path uses (fsWriteAtomic), so a scenario exercises the file the device would actually read.
            //
            // This op exists because the interesting cases have no shipped file to select: a DELIBERATELY BROKEN script (proving the device degrades rather than dies, one could not live in moonlive/, where unit_MoonLiveScripts compiles all of them), and an edit that CHANGES A SCRIPT'S CONTROL SET (proving controls re-derive and keep their values). Selecting a shipped script covers neither.
            // A MALFORMED step is a failed scenario, for the same reason a failed write is (see below): every later step runs against a file that was never staged, and the run could still report PASSED. Skipping it silently is what makes a typo'd key pass here and fail on hardware, where run_live_scenario.py already treats this as an error.
            if (!step.has("path") || !step.has("value")) {
                std::printf("  WRITE %s — missing path/value\n", name);
                result.check(false, name);
                continue;
            }
            const char* filePath = step["path"].c_str();
            const std::string body = step["value"].str;
            // mkdir -p the parent: a scenario names /moonlive/x.mle without staging the directory, and on a fresh build tree that directory may not exist yet.
            if (const char* slash = std::strrchr(filePath, '/')) {
                if (slash != filePath) {
                    std::string dir(filePath, static_cast<size_t>(slash - filePath));
                    mm::platform::fsMkdir(dir.c_str());
                }
            }
            // A FAILED write is a failed scenario, not a printed note. Every step after this one would run against a stale or absent file and could still report PASSED, the silent pass this op exists to make impossible.
            const bool wrote = mm::platform::fsWriteAtomic(filePath, body.c_str(), body.size());
            if (wrote) {
                std::printf("  WRITE %s (%s, %zu bytes)\n", name, filePath, body.size());
            } else {
                std::printf("  WRITE %s — write to %s FAILED\n", name, filePath);
            }
            result.check(wrote, name);
        } else if (std::strcmp(op, "remove_module") == 0 || std::strcmp(op, "delete_module") == 0) {
            // `remove_module` and `delete_module` are aliases, accept both so a scenario reads identically here and on the live runner (which uses `delete_module`). The two runners must never diverge on op names, or a scenario silently no-ops on one tier. Remove a child module from its parent, mirrors HttpServerModule::handleDeleteModule (remove from parent, release + recursive delete, rebuild pipeline state). Only child modules can be removed; top-level modules are policy-fixed.
            const char* targetId = step["id"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target || !target->parent() || !target->userEditable()) {
                // Mirror the live API (handleDeleteModule): top-level and non-editable submodules (Board, Preview, Improv) can't be removed.
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
            // Delete every child of a container, leaving the container itself.
            // The "prepare my own canvas" primitive: a scenario assumes nothing about the device's starting tree, clears a container, then adds what it needs. Children are deleted including ones the scenario never added (a live device's pre-existing effects/modifiers).
            // Mirrors remove_module's release, looped over all children. Walk back-to-front since removeChild compacts the array in place.
            const char* targetId = step["id"].c_str();
            auto* container = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!container) {
                std::printf("  clr     %s — container %s not found, skipped\n", name, targetId);
                continue;
            }
            int cleared = 0;
            for (uint8_t i = container->childCount(); i > 0; i--) {
                mm::MoonModule* childMod = container->child(i - 1);
                // Mirror handleDeleteModule: non-editable submodules (Board, Preview, Improv) are apparatus, not deletable, skip them so the in-process clear matches what the live device does.
                if (!childMod->userEditable()) continue;
                container->removeChild(childMod);
                childMod->release();
                // Purge the child AND any registered descendants (e.g. a Layer's effect child) before freeing, so no id is left dangling.
                ctx.purgeSubtree(childMod);
                mm::Scheduler::deleteTree(childMod);
                cleared++;
            }
            if (schedulerStarted) ctx.scheduler.prepareTree();
            std::printf("  clr     %s (%s: %d cleared)\n", name, targetId, cleared);
        } else if (std::strcmp(op, "replace_module") == 0) {
            // Replace a child with a fresh module of another type at the same slot, mirrors HttpServerModule::handleReplaceModule. The new module re-registers under the SAME scenario id so later steps (set_control, remove) still address it by that id.
            const char* targetId = step["id"].c_str();
            const char* newType = step["type"].c_str();
            auto* target = ctx.modules.count(targetId) ? ctx.modules[targetId] : nullptr;
            if (!target || !target->parent() || !target->userEditable()) {
                // Mirror the live API (handleReplaceModule): top-level and non-editable submodules can't be replaced.
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
                // Mirror the remove_module / clear_children branches: purge any ctx.modules entries pointing at old or its descendants before freeing. Otherwise a later step addressing an old descendant by id reads a dangling pointer. purgeSubtree also removes the targetId mapping; we re-register it to fresh on the next line.
                ctx.purgeSubtree(old);
                old->release();
                mm::Scheduler::deleteTree(old);
            }
            ctx.modules[targetId] = fresh;
            if (schedulerStarted) ctx.scheduler.prepareTree();
            std::printf("  ~     %s (%s → %s)\n", name, targetId, newType);
        } else if (std::strcmp(op, "measure") == 0) {
            // Pure measurement step, no side effects. op:"measure" is the implicit-measure shape so scenarios can interleave snapshots without faking a control write. The measurement block below honours both `measure: true` AND op:"measure".
            std::printf("  ...   %s\n", name);
        }

        // Per-step measurement: warmup + measure + bounded assertions. Triggered by either `"measure": true` on the step (the explicit flag, used alongside set_control to measure after a mutation) or op:"measure" (the implicit-measure shape, a snapshot step with no other side effects).
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
                    // min_pct is relative to a live baseline (the WiFi-vs-Eth scenarios use it) and only the live runner has a baseline to compare against. In-process can't enforce it, log a clear skip so users see *why* the bound wasn't applied instead of silently treating it as "FPS > 0".
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
            for (int i = 0; i < MEASURE_FRAMES; i++) ctx.scheduler.tick();
            uint32_t elapsedUs = mm::platform::micros() - startUs;
            uint32_t tickTimeUs = MEASURE_FRAMES > 0 ? elapsedUs / MEASURE_FRAMES : 0;
            uint32_t fps = tickTimeUs > 0 ? 1000000 / tickTimeUs : 0;
            size_t heapAfterMeasure = mm::platform::freeHeap();
            // Largest contiguous block in INTERNAL RAM, diagnoses internal-heap fragmentation, which silently degrades the Layer LUT (the buffer needs 60-90 KB contiguous at 128×128 with mirror; fragmentation drops mirror without changing free_heap). Use the internal-only variant so the signal works on PSRAM boards too, maxAllocBlock would report ~8 MB regardless of internal pressure. 0 on desktop = unlimited.
            size_t maxBlock = mm::platform::maxInternalAllocBlock();

            // Buffer state at this measurement (may be empty in early build-up steps).
            auto* layer = static_cast<mm::Layer*>(
                ctx.modules.count("Layer") ? ctx.modules["Layer"] : nullptr);
            unsigned lights = layer ? static_cast<unsigned>(layer->buffer().count()) : 0;

            // `heap=` is the absolute free-heap after the measurement window, that's what observed.<target>.free_heap consumes (the rolling promise is on actual free heap, not on a delta). On desktop freeHeap() returns 0 ("unlimited") and the value is rendered as 0, which the runner treats as "no heap assertion".
            //
            // `(step: ±N)` is the signed step delta from the pre-step heap to the post-measurement heap, useful for diagnosing which step consumed memory, but not what the contract asserts on. Kept for human-readable diagnostics.
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
            // Heap-delta bound, fail if this step grew the heap by more than max_delta_bytes vs the previous measurement (catches leaks / unintended allocs).
            if (hasHeapBound && heapBefore > 0) {
                char msg[128];
                std::snprintf(msg, sizeof(msg),
                              "%s heap step delta %+ld <= %ld bytes",
                              name, stepDelta, maxHeapDelta);
                result.check(stepDelta <= maxHeapDelta, msg);
            }

            // Per-step performance contract. Each measure step can carry a `contract[<target>]` block with `tick_us`, `free_heap`, optional `tick_tolerance_pct` / `heap_tolerance_pct` / `tolerance_us`, and `set_by` + `reason` describing when/why the contract was set. Contracts are hand-blessed promises; --update-contract --reason renegotiates them. The whole block is optional; the live runner shares this shape, same scenarios serve both tiers.
            if (step.has("contract") && step["contract"].has(hostTarget())) {
                const auto& exp = step["contract"][hostTarget()];
                // Per-target defaults reflect run-to-run variance, not "I don't care": desktop-*, multi-process OS jitter, 20% pct + 200us absolute floor.
                //               The floor dominates below ~1ms tick (the realistic case).
                //   esp32-*  , bounded RTOS but lwIP/EMAC jitter, 10% pct + 5us floor.
                // KEEP IN SYNC: the live runner re-declares the same defaults at moondeck/scenario/run_live_scenario.py contract-block handler, tuning one without the other silently desyncs the two tiers.
                const bool isDesktop = std::strncmp(hostTarget(), "desktop-", 8) == 0;
                double tickTolPct = exp.has("tick_tolerance_pct") ? exp["tick_tolerance_pct"].num
                                                                   : (isDesktop ? 20.0 : 10.0);
                double heapTolPct = exp.has("heap_tolerance_pct") ? exp["heap_tolerance_pct"].num
                                                                   : (isDesktop ? 20.0 : 10.0);
                // Absolute floor: at very small ticks (sub-millisecond on desktop), OS scheduling jitter dwarfs any percentage tolerance. The desktop floor of 200us absorbs typical desktop noise; the ESP32 floor of 5us is realistic for the bounded RTOS clock.
                double tolUs = exp.has("tolerance_us") ? exp["tolerance_us"].num
                                                       : (isDesktop ? 200.0 : 5.0);
                if (exp.has("tick_us") && exp["tick_us"].num > 0) {
                    // tick is a *ceiling*, faster than contract is good news, same shape as heap being a floor. Tolerance absorbs upward jitter only; speedups never fail.
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
                // free_heap is meaningful on ESP32 (reports unlimited / 0 on desktop). Contract is a *floor*: the device must deliver at least this much free heap; more is fine; less by more than tolerance is a regression.
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
                // max_alloc_block is also a *floor*, the LUT and driver buffers need a single contiguous chunk that's much larger than total free heap when fragmentation kicks in. A scenario can opt into this assertion when its workload depends on a specific minimum (mirror LUT silently degrades when the block won't fit; see src/light/layers/Layer.h Layer::rebuildLUT). Optional field; skipped on desktop where the value is always 0 (unlimited).
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

    // After all steps, do the legacy end-of-scenario buffer check IF a Layer module is present and the scheduler was started. Build-up scenarios that explicitly assert in their measure steps don't need this redundancy, but existing scenarios depend on it.
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

    // Summary
    std::printf("---\n");
    if (result.passed) {
        std::printf("PASSED (%d checks)\n", result.checks);
    } else {
        std::printf("FAILED (%d/%d checks)\n", result.failures, result.checks);
    }
    return result.passed ? 0 : 1;
}

// Directory iteration can throw filesystem_error (a scenarios/ dir deleted mid-run). Letting it escape main is the correct outcome for a CLI test runner: it terminates with a diagnostic and a non-zero status, which is exactly what a harness needs to see. NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Run all scenarios in the scenarios/ directory tree. Recursive so the core/ + light/ split picks up every JSON without each subfolder needing its own discovery loop.
        int failed = 0;
        int total = 0;
        for (auto& entry : std::filesystem::recursive_directory_iterator("test/scenarios")) {
            if (entry.path().extension() == ".json") {
                total++;
                // path::c_str() is wchar_t* on Windows; round-trip through .string() to get a portable narrow-char view for runScenario.
                if (runScenario(entry.path().string().c_str()) != 0) failed++;
                std::printf("\n");
            }
        }
        std::printf("=== %d scenario(s), %d passed, %d failed ===\n",
                    total, total - failed, failed);
        return failed > 0 ? 1 : 0;
    }

    return runScenario(argv[1]);
}
