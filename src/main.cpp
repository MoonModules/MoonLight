/// The entry point: registers every module type with the factory, builds the boot module tree, and runs the render loop.
///
/// @technical
///
/// ## How the boot tree is created
///
/// All modules come from the factory, heap-allocated and in PSRAM when available, named by stripping the role suffix (a network module keeps its direction). Null results go unchecked: a startup crash gives a usable backtrace.
///
/// ## Why markWiredByCode matters
///
/// A boot-wired module is wiring, not a user choice: a config saved before a child was added would drop it, and a load would swap a wired instance for a fresh one that lost its injected pointer (PreviewDriver's broadcaster).
///
/// ## Why the audio service is boot-wired
///
/// Auto-wiring forced an I2S init on boards with no microphone and boot-looped a classic ESP32, so its pins default to empty and the effects read a silent frame until a catalog entry names real ones.
///
/// ## Why the device-wide tools are boot-wired
///
/// ControlModule holds the presets and File Manager is a device-wide tool: both are device capabilities rather than catalog additions, so they exist whatever the persisted tree says.
///
/// ## Why MoonCloud is not a Firmware child
///
/// Parenting under Firmware showed an empty card, since that module does not chain to its children. Each MoonCloud child has its own consent: a joint lightshow, a message and a chip model are different decisions.
///
/// ## Why Improv is compile-time gated
///
/// Improv only pushes credentials, so a build without WiFi has no surface for it. It is created after the network module so its setter has a valid pointer; APPLY_OP (0xFC) carries the state document over serial.
///
/// ## Why MQTT is built on every networked target
///
/// MQTT bridges the light controls to a broker for Homebridge and Home Assistant. It uses TCP, so it runs over WiFi or Ethernet and stays disabled until a broker is set; systemModule gives the topic prefix, controlModule the effect list.
///
/// ## Why the boot layer is one Pulse effect
///
/// One default Pulse effect on a 16x16 grid, no modifier: a sparse shell shows that the lights work, the device runs and it hears the room. A catalog entry can replace it through `"$patch":"replace"` on the Layer.
///
/// ## Why output drivers are not boot-wired
///
/// Output drivers come per board through the catalog, so a bare flash has no output. The preview is the exception: it needs the HTTP server's WS broadcaster, which only this file holds and the catalog cannot supply.
///
/// ## Why registration order matters
///
/// The scheduler walks roots in registration order: filesystem first (its load hook overlays persisted values before setup), then identity, network, services, the light pipeline, and the server last, only where an IP stack exists.
///
/// ## What the boot banner prints
///
/// The server binds every interface, so a loopback name only means something on the host; a device prints its interface address, which NetworkModule logs as each interface comes up, and with no address yet the line says so.
///
/// ## Why the render loop subscribes to the task watchdog
///
/// A wedge in the render loop panics and reboots with a backtrace instead of hanging silently. The sdkconfig runs the TWDT with idle-task checking off, so this explicit subscription, reset each tick, is what the watchdog watches.
///
/// ## Why the periodic line is a plain write
///
/// A plain write the platform level cannot suppress, gated by hand: silent above a warning, printed in the first minute for the installer. `maxInternalAllocBlock` is internal RAM only; renderWait is the worst frame-boundary wait in the last second.
///
/// ## Why the address token rides the periodic line
///
/// MM_IP is the installer's post-flash serial address token, gated to the first minute and latched off so a counter wrap cannot reopen it. MM_DEVICE carries the discovery name beside it so the link survives a lease change.
///
/// ## Why the loop pacing uses goto and a sleep
///
/// Pacing lives at the loop's tail, so a goto keeps skipping the logging from skipping the sleep. A sub-millisecond sleep parks the thread where a yield would burn a core; the yield stays for the split's frame boundary, which must not sleep.
///
#include "module_types.h"

#include <csignal>   // sig_atomic_t: the stop flag a signal handler writes

#include "core/util/JsonFetch.h"   // poll(): the JSON worker stops once nothing uses it
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/FixtureProfilesModule.h"
#include "light/drivers/PreviewDriver.h"
#include "core/system/HttpServerModule.h"
#include "core/system/SystemModule.h"
#include "core/system/ControlModule.h"
#include "core/services/Services.h"
#include "core/services/AudioService.h"
#include "core/system/I2cBusModule.h"
#include "core/system/TasksModule.h"
#include "core/system/PinsModule.h"
#include "core/system/FileManagerModule.h"
#include "core/system/FirmwareUpdateModule.h"
#include "core/system/MoonCloudModule.h"
#include "core/system/MoonStatsModule.h"
#include "core/system/MoonTalkModule.h"
#include "core/system/ImprovProvisioningModule.h"
#include "core/system/MqttModule.h"
#include "core/system/DevicesModule.h"
#include "core/system/FilesystemModule.h"
#include "core/system/EthernetModule.h"
#include "core/system/WiFiModule.h"
#include "core/system/AccessPointModule.h"
#include "core/system/NetworkModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>


/// Create a boot module or stop, since every type here is registered and a null means the build is wrong rather than the device.
template <typename T>
static T* createOrDie(const char* typeName) {
    auto* mod = static_cast<T*>(mm::ModuleFactory::create(typeName));
    if (!mod) {
        std::printf("FATAL: module type %s is not registered\n", typeName);
        std::abort();
    }
    return mod;
}

static void printModuleMetrics(mm::MoonModule* mod, int depth) {
    if (!mod) return;
    if (mod->dynamicBytes() > 0) {
        std::printf("  %s:%uus/%uKB", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->tickTimeUs()),
                    static_cast<unsigned>(mod->dynamicBytes() / 1024));
    } else {
        std::printf("  %s:%uus", mod->name() ? mod->name() : "?",
                    static_cast<unsigned>(mod->tickTimeUs()));
    }
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        printModuleMetrics(mod->child(i), depth + 1);
    }
}

void mm_main(volatile std::sig_atomic_t& keepRunning, uint16_t httpPort) {
    mm::registerModuleTypes();
    mm::Scheduler scheduler;

    // Every container is created from the one table the scenario runner shares, wired by code so a saved tree that predates a child keeps it: @xref{how-the-boot-tree-is-created}, @xref{why-markwiredbycode-matters}.
    const auto top = [](const char* typeName) {
        mm::MoonModule* m = mm::createTopLevel(typeName);
        if (!m) {
            std::printf("FATAL: module type %s is not registered\n", typeName);
            std::abort();
        }
        return m;
    };
    // A wired child, found by its type to hand it the pointers it needs.
    const auto wired = [](mm::MoonModule* parent, const char* typeName) -> mm::MoonModule* {
        for (uint8_t i = 0; i < parent->childCount(); i++)
            if (std::strcmp(parent->child(i)->typeName(), typeName) == 0) return parent->child(i);
        return nullptr;
    };

    // Filesystem (first, wires the load hook into the scheduler so persisted values overlay into other modules' bound variables before their setup() runs)
    auto* filesystemModule = static_cast<mm::FilesystemModule*>(top("FilesystemModule"));
    filesystemModule->setScheduler(&scheduler);

    // File Manager, a boot-wired device-wide tool for browsing the filesystem, distinct from FilesystemModule which is the persistence engine: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* fileManagerModule = top("FileManagerModule");

    // System (deviceName needed by other modules), with the device's inspection toolkit and its I2C bus wired in, which no container accepts as an editable child.
    auto* systemModule = static_cast<mm::SystemModule*>(top("SystemModule"));
    systemModule->setScheduler(&scheduler);

    // Services, the core-domain twin of Effects/Drivers, with the AudioService wired in: @xref{why-the-audio-service-is-boot-wired}.
    auto* servicesModule = top("Services");

    // ControlModule puts the device into a named state, top-level because a preset reaches ACROSS Layouts, Effects, Drivers and Services: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* controlModule = static_cast<mm::ControlModule*>(top("ControlModule"));

    // The device identity is SystemModule's own pair of controls rather than a separate module. Tooling injects the model like any catalog default, over HTTP or serial, both routed through the state document engine and the control's validator.

    // Surfaces the install's status as read-only controls, polling the shared globals so the push picks up progress while HTTP drives the flash itself. Named Firmware because the card hosts the install picker.
    auto* firmwareUpdateModule = top("FirmwareUpdateModule");

    // The container for everything talking to a server we run, Stats and Talk wired in, each with its own consent: @xref{why-mooncloud-is-not-a-firmware-child}.
    auto* moonCloudModule = top("MoonCloudModule");

    // Network, its interfaces first in the order the cascade tries them, then Improv (compile-time gated: @xref{why-improv-is-compile-time-gated}) and MQTT (@xref{why-mqtt-is-built-on-every-networked-target}).
    auto* networkModule = static_cast<mm::NetworkModule*>(top("NetworkModule"));
    networkModule->setScheduler(&scheduler);
    networkModule->setSystemModule(systemModule);
    networkModule->setEthernet(static_cast<mm::EthernetModule*>(wired(networkModule, "EthernetModule")));
    networkModule->setWiFi(static_cast<mm::WiFiModule*>(wired(networkModule, "WiFiModule")));
    networkModule->setAccessPoint(static_cast<mm::AccessPointModule*>(wired(networkModule, "AccessPointModule")));
    if (auto* improv = static_cast<mm::ImprovProvisioningModule*>(wired(networkModule, "ImprovProvisioningModule"))) {
        improv->setSystemModule(systemModule);
        improv->setNetworkModule(networkModule);
        improv->setScheduler(&scheduler);   // the APPLY_OP vendor RPC applies its document to this tree
    }
    if (auto* mqtt = static_cast<mm::MqttModule*>(wired(networkModule, "MqttModule"))) {
        mqtt->setSystemModule(systemModule);
        mqtt->setControlModule(controlModule);   // look-only presets as the HA effect list
    }
    // Devices, what reaches past this device to the others, with Discovery wired in; setSelfName matches the self row to the device identity (static lifetime as SystemModule's member).
    auto* devicesContainer = top("Devices");
    if (auto* devices = static_cast<mm::DevicesModule*>(wired(devicesContainer, "DevicesModule"))) devices->setSelfName(systemModule->deviceName());

    // Layouts, the top-level container for one or more layouts, today one GridLayout that self-initializes to defaultGridSize with no boot-time dimensions threaded in.
    auto* layouts = static_cast<mm::Layouts*>(top("Layouts"));
    auto* grid = createOrDie<mm::GridLayout>("GridLayout");
    layouts->addChild(grid);

    // Effects: top-level container; one or more layers, each rendering into its own buffer. Effects finds the top-level Layouts itself when it prepares, and hands it to every child Layer.
    auto* effectsContainer = top("Effects");
    auto* layer = createOrDie<mm::Layer>("Layer");
    effectsContainer->addChild(layer);

    // One default effect so a bare device still shows lights out of the box, but NO default modifier: @xref{why-the-boot-layer-is-one-pulse-effect}.
    auto* pulse = createOrDie<mm::MoonModule>("PulseEffect");
    layer->addChild(pulse);

    // Drivers finds the top-level Effects itself when it prepares. Output drivers are added per board through the catalog, with the fixture-profile library and the preview wired in: @xref{why-output-drivers-are-not-boot-wired}.
    auto* drivers = static_cast<mm::Drivers*>(top("Drivers"));

    auto* httpServer = static_cast<mm::HttpServerModule*>(top("HttpServerModule"));
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    // PreviewDriver pushes the coordinate table and per-frame RGB to the HTTP server's WS broadcaster.
    if (auto* preview = static_cast<mm::PreviewDriver*>(wired(drivers, "PreviewDriver"))) preview->setBroadcaster(httpServer);

    // Registration order matters, and the scheduler walks the roots in it each tick: @xref{why-registration-order-matters}.
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(fileManagerModule);
    scheduler.addModule(firmwareUpdateModule);
    scheduler.addModule(moonCloudModule);
    scheduler.addModule(networkModule);
    scheduler.addModule(devicesContainer);
    scheduler.addModule(servicesModule);
    scheduler.addModule(controlModule);
    scheduler.addModule(layouts);
    scheduler.addModule(effectsContainer);
    scheduler.addModule(drivers);
    // Only where an IP stack exists, which is what makes a network-less build supported: @xref{why-registration-order-matters}.
    if constexpr (mm::platform::hasNetwork) scheduler.addModule(httpServer);

    scheduler.setup();

    uint32_t lights = layouts->totalLightCount();
    uint32_t bufBytes = lights * 3;
    std::printf("MoonLight running — grid %dx%d, %lu lights, buffer %lu bytes\n",
                grid->width, grid->height,
                static_cast<unsigned long>(lights),
                static_cast<unsigned long>(bufBytes));
    std::printf("sizeof: MoonModule=%zu Layer=%zu Drivers=%zu Grid=%zu HttpServer=%zu\n",
                sizeof(mm::MoonModule), sizeof(mm::Layer), sizeof(mm::Drivers),
                sizeof(mm::GridLayout), sizeof(mm::HttpServerModule));
    // The server binds every interface, so it is reachable across the LAN: @xref{what-the-boot-banner-prints}.
    const char* hostIp = mm::platform::hostIp();
    if (hostIp && hostIp[0]) {
        std::printf("HTTP server → http://%s:%u\n", hostIp, httpServer->port);
    } else {
        // No address yet, which is true for two different reasons: @xref{what-the-boot-banner-prints}.
        std::printf("HTTP server on port %u — no network address yet\n", httpServer->port);
    }

    size_t heap = mm::platform::freeHeap();
    if (heap > 0) {
        std::printf("Free heap: %u bytes\n", static_cast<unsigned>(heap));
    }
    std::fflush(stdout);

    uint32_t lastLog = mm::platform::millis();
    const uint32_t bootMillis = lastLog;   // window start for the MM_IP serial token
    bool mmIpWindowClosed = false;         // latches true once the 60 s window elapses

    // Subscribe THIS render-loop task to the task watchdog, so a genuine wedge here panics and reboots instead of hanging silently: @xref{why-the-render-loop-subscribes-to-the-task-watchdog}.
    mm::platform::taskWdtSubscribe();

    while (keepRunning) {
        scheduler.tick();
        mm::platform::taskWdtReset();   // feed the render-loop WDT subscription — a live tick is not a wedge

        // Log every second
        uint32_t now = mm::platform::millis();
        if (now - lastLog >= 1000) {
            lastLog = now;
            // The JSON client stops its worker once nothing uses it, which only the loop can ask safely.
            mm::JsonFetch::poll();
            // `goto`, not `continue`, because the loop's pacing lives at its TAIL: @xref{why-the-loop-pacing-uses-goto-and-a-sleep}.
            if (scheduler.tickTimeUs() == 0) goto paced; // no measurement yet

            // A plain write rather than a log call, gating on the log level by hand: @xref{why-the-periodic-line-is-a-plain-write}.
            const bool inBootWindow = !mmIpWindowClosed && (now - bootMillis < 60000);
            if (systemModule->logLevel() < mm::platform::LogLevel::Info && !inBootWindow) goto paced;

            heap = mm::platform::freeHeap();
            std::printf("tick: %uus (FPS: %u)", static_cast<unsigned>(scheduler.tickTimeUs()),
                        static_cast<unsigned>(scheduler.fps()));
            if (heap > 0) {
                // maxInternalAllocBlock, internal RAM only: @xref{why-the-periodic-line-is-a-plain-write}.
                std::printf("  free: %u  maxBlock: %u",
                            static_cast<unsigned>(heap),
                            static_cast<unsigned>(mm::platform::maxInternalAllocBlock()));
            }
            // The worst wait at a frame boundary in the last second, not a single frame's: @xref{why-the-periodic-line-is-a-plain-write}.
            if (drivers->renderSplitActive())
                std::printf("  renderWait: %uus", static_cast<unsigned>(drivers->renderWaitPeakUs()));
            // A stable address token for the installer's post-flash serial read, gated to the first minute: @xref{why-the-address-token-rides-the-periodic-line}.
            if (!mmIpWindowClosed) {
                if (now - bootMillis >= 60000) {
                    mmIpWindowClosed = true;   // first 60 s elapsed; stop for the rest of uptime
                } else {
                    uint8_t ip[4];
                    networkModule->currentIp(ip);
                    if (ip[0] || ip[1] || ip[2] || ip[3]) {
                        char ipStr[16];
                        mm::formatDottedQuad(ipStr, ip);
                        std::printf("  MM_IP=%s", ipStr);
                        // The discovery name alongside the address, so the installer's link survives a lease change: @xref{why-the-address-token-rides-the-periodic-line}.
                        std::printf("  MM_DEVICE=%s.local", systemModule->deviceName());
                    }
                }
            }
            // Per-module timing (walk tree recursively)
            for (uint8_t i = 0; i < scheduler.moduleCount(); i++) {
                printModuleMetrics(scheduler.module(i), 0);
            }
            std::printf("\n");
            std::fflush(stdout);
        }

    paced:
        // Pace the loop rather than spin, since a bare yield burns a whole core: @xref{why-the-loop-pacing-uses-goto-and-a-sleep}.
        mm::platform::pauseLoop();
    }

    std::printf("\nShutting down.\n");
    scheduler.release();
}
