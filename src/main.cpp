/// The entry point: registers every module type with the factory, builds the boot module tree, and runs the render loop.
///
/// @moreinfo
///
/// ## How the boot tree is created
///
/// All modules are created via the factory: heap-allocated, PSRAM when available, `classSize` set.
/// Names come from the factory, which strips the role suffix, keeping the direction on a network module so send and receive stay distinguishable.
/// An explicit name is needed only for a genuine rename.
/// Creation can return null in principle, and these results are deliberately not checked.
/// At startup the right behavior is a crash with a usable backtrace, which both targets already give, rather than boilerplate that reports the same failure with less to go on.
///
/// ## Why markWiredByCode matters
///
/// A boot-wired module is wiring rather than a user choice, so the persisted tree must not decide whether it exists.
/// Without the mark, a config written before a child was added drops that child on load, which is exactly what happened when Talk was introduced beside Stats.
/// The file listed one child, so the tree came back with one.
/// It also stops a persistence load replacing a wired instance with a fresh factory one that lost an injected pointer, which is what protects PreviewDriver's broadcaster.
/// Devices is the same case: the mark preserves it on a device whose saved Network.json predates the child.
///
/// ## Why the audio service is boot-wired
///
/// Auto-wiring once forced an I2S init on boards with no microphone, which hung setup and boot-looped a classic ESP32.
/// Its pins now default to empty so it idles until real ones are entered, and the effects read a silent frame when no microphone exists.
/// `mode` is synthesized by default, so a board with a microphone names it in its catalog entry the way it already names its pins.
///
/// ## Why the device-wide tools are boot-wired
///
/// ControlModule holds the presets, which are a device capability rather than something a user adds, so it exists whatever the persisted tree says.
/// File Manager is the same kind of thing: a device-wide tool rather than a per-board one, so no catalog entry adds it.
/// Its setName only changes the card's label, and the type stays FileManagerModule, which is what a persisted tree and a type lookup both key on.
///
/// ## Why MoonCloud is not a Firmware child
///
/// Parenting it under Firmware was tried: that module does not chain to its children, so anything parented there showed an empty card.
/// Each MoonCloud child carries its own consent, because wanting a joint lightshow is not agreeing to usage reporting.
/// Talk is a SECOND child with its own consent, because publishing a message and sharing a chip model are different decisions.
///
/// ## Why Improv is compile-time gated
///
/// Improv is the one exception to registering everything and letting modules guard themselves.
/// Its only purpose is pushing credentials, so on a build without WiFi there is no surface to push to.
/// It is created after the network module so its setter has a valid pointer, and one module answers the device-info request while the other takes the credentials.
/// The APPLY_OP vendor RPC (0xFC) carries the device-model's catalog ops over serial during provisioning.
/// ImprovProvisioningModule routes each to the HttpServerModule apply-core, the same code `/api/modules` and `/api/control` use: "Improv = REST over serial".
///
/// ## Why MQTT is built on every networked target
///
/// MQTT bridges the light controls to a broker for Homebridge and Home Assistant.
/// It is built on every networked target because it uses TCP, so it works over WiFi or Ethernet alike, and it stays disabled until the user sets a broker.
/// systemModule is injected for the default topic prefix, which is the device name.
/// controlModule is injected so the look-only presets become the Home Assistant effect list.
///
/// ## Why the boot layer is one Pulse effect
///
/// One default effect so a bare device with no catalog inject still shows lights out of the box, but NO default modifier. The boot Layer is one effect on a 16x16 grid.
/// A device-model catalog entry can REPLACE it through `replaceChildren` with its own effects and modifiers, the way the testbench swaps in AudioSpectrum plus RandomMap.
/// Pulse is the one because a first boot has to answer three questions at once: the lights work, the device runs, and it hears the room.
/// A sparse shell answers all three, where a dense field answers only the first since every light is already lit.
///
/// ## Why output drivers are not boot-wired
///
/// Output drivers are added per board through the catalog, so a device carries only what its board has.
/// The container wires any child generically, so one added at runtime is wired exactly like one added at boot and persists across a reboot.
/// A bare flash therefore has no output until a board is selected, which is the deliberate model.
/// The preview is the one exception: it needs the broadcaster only this file holds, which the catalog cannot supply.
/// PreviewDriver pushes the coordinate table plus per-frame RGB to the HTTP server's WS broadcaster, HttpServerModule being a BinaryBroadcaster.
/// Light owns the preview wire format end to end; core writes the bytes.
///
/// ## Why registration order matters
///
/// The scheduler walks the roots in registration order each tick.
/// The filesystem comes first so its load hook runs before any setup, overlaying persisted values into other modules' bound variables.
/// Then the identity and status surfaces, then network with its credential and bridge children, then the services a later stage may consume, then the light pipeline, and the server last.
/// The server is added only where an IP stack exists. Setup binds a socket, and with no interface compiled in nothing initializes the stack, so its thread never exists and the board goes down before the light pipeline runs.
/// That gate is what makes a network-less build supported.
///
/// ## What the boot banner prints
///
/// The server binds every interface, so it is reachable across the LAN, and a loopback name only means anything where the browser runs on the host.
/// A device prints its interface address instead, and the network module logs the real one as each interface comes up.
/// With no address yet the line states exactly that, which covers two different reasons.
/// On a device it is normal this early, an interface not being up, and NetworkModule logs the address when it comes up.
/// On a desktop it means `hostIp()` found no route at all, and stating what is true promises no follow-up message an offline desktop never prints.
///
/// ## Why the render loop subscribes to the task watchdog
///
/// A genuine wedge in the render loop now panics and reboots, the self-heal, with a backtrace, instead of hanging silently.
/// The sdkconfig runs the TWDT with idle-task checking OFF, a saturated core being healthy rather than a bug, so this explicit subscription is what the watchdog watches.
/// It is reset each tick, so a heavy-but-live frame keeps feeding it.
///
/// ## Why the periodic line is a plain write
///
/// A plain write rather than a log call, so the platform level cannot suppress it, and this gates on the same level by hand.
/// Silenced above a warning, so a resting device makes no periodic serial write and a transmit-blinking LED stops flickering.
/// The first minute always prints, because the installer reads the address off this line.
/// `maxInternalAllocBlock` reports internal RAM only: the all-memory variant reports about 8 MB on S3/S2 PSRAM boards and is useless as a memory-pressure KPI, and platform.h holds the split.
/// renderWait is the worst wait at a frame boundary in the last second rather than a single frame's, which would land wherever this once-a-second line falls and read as nothing.
/// It is what says whether a second buffer would recover idle time or gain nothing.
///
/// ## Why the address token rides the periodic line
///
/// MM_IP is a stable address token for the installer's post-flash serial read, riding an already-periodic line so it costs no extra write and repeats until the port is reopened.
/// It is gated to the first minute, after which the address comes from the API, and the window latches off for good so a counter wrap cannot reopen it.
/// MM_DEVICE carries the discovery name alongside the address, so the installer's link survives a lease change.
/// It is the one network identity, and it rides serial because the installer's fallback is blocked by mixed content.
///
/// ## Why the loop pacing uses goto and a sleep
///
/// The loop's pacing lives at its TAIL, so a `continue` in the logging block would skip the yield and spin the core for that pass.
/// Jumping to the pacing point keeps "skip the logging" from meaning "skip the sleep".
/// Yielding only offers the CPU to another runnable thread, so with nothing else to run it returns at once and this burns a whole core, as a bench reported.
/// A sub-millisecond sleep parks the thread at no cost to the frame rate, and the yield stays for the split's frame boundary, which must not sleep.
///
#include "module_types.h"

#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/LightPresetsModule.h"
#include "light/drivers/PreviewDriver.h"
#include "core/system/HttpServerModule.h"
#include "core/system/SystemModule.h"
#include "core/system/ControlModule.h"
#include "core/services/Services.h"
#include "core/services/AudioService.h"
#include "core/system/I2cScanModule.h"
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
#include "core/system/NetworkModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>


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

void mm_main(volatile bool& keepRunning, uint16_t httpPort) {
    mm::registerModuleTypes();
    mm::Scheduler scheduler;

    // Every module below is created via the factory, named by it, and its result deliberately unchecked: @xref{how-the-boot-tree-is-created}.

    // Filesystem (first, wires the load hook into the scheduler so persisted values overlay into other modules' bound variables before their setup() runs)
    auto* filesystemModule = createOrDie<mm::FilesystemModule>("FilesystemModule");
    filesystemModule->setScheduler(&scheduler);

    // File Manager, a boot-wired device-wide tool for browsing the filesystem, distinct from FilesystemModule which is the persistence engine: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* fileManagerModule = createOrDie<mm::FileManagerModule>("FileManagerModule");
    fileManagerModule->setName("File Manager");

    // System (deviceName needed by other modules)
    auto* systemModule = createOrDie<mm::SystemModule>("SystemModule");
    systemModule->setScheduler(&scheduler);

    // The device's inspection toolkit, wired by code as System children rather than user-added. Always present, exempt from the persistence trim, and accepted by no container as an editable child, so no card offers a delete.
    auto* tasksModule = createOrDie<mm::TasksModule>("TasksModule");
    tasksModule->markWiredByCode();
    systemModule->addChild(tasksModule);
    auto* i2cScanModule = createOrDie<mm::I2cScanModule>("I2cScanModule");
    i2cScanModule->markWiredByCode();
    systemModule->addChild(i2cScanModule);
    auto* pinsModule = createOrDie<mm::PinsModule>("PinsModule");
    pinsModule->markWiredByCode();
    systemModule->addChild(pinsModule);

    // Services, the core-domain twin of Effects/Drivers: a grouping node, added as a root below, whose children the user adds and removes at runtime.
    auto* servicesModule = createOrDie<mm::Services>("Services");

    // Boot-wired rather than user-added, because the default effect reacts to sound and a device without this module shows none of it: @xref{why-the-audio-service-is-boot-wired}.
    auto* audioService = createOrDie<mm::AudioService>("AudioService");
    audioService->markWiredByCode();   // simulate is the member's own default, so nothing to set
    servicesModule->addChild(audioService);

    // ControlModule puts the device into a named state, top-level because a preset reaches ACROSS Layouts, Effects, Drivers and Services: @xref{why-the-device-wide-tools-are-boot-wired}.
    auto* controlModule = createOrDie<mm::ControlModule>("ControlModule");

    // The device identity is SystemModule's own pair of controls rather than a separate module. Tooling injects the model like any catalog default, over HTTP or serial, both routed through the apply core and the control's validator.

    // Surfaces the install's status as read-only controls, polling the shared globals so the push picks up progress while HTTP drives the flash itself. Renamed because the card hosts the install picker.
    auto* firmwareUpdateModule = createOrDie<mm::FirmwareUpdateModule>("FirmwareUpdateModule");
    firmwareUpdateModule->setName("Firmware");

    // The container for everything talking to a server we run, each child carrying its own consent: @xref{why-mooncloud-is-not-a-firmware-child}.
    auto* moonCloudModule = createOrDie<mm::MoonCloudModule>("MoonCloudModule");
    moonCloudModule->setName("MoonCloud");

    auto* moonStatsModule = createOrDie<mm::MoonStatsModule>("MoonStatsModule");
    moonStatsModule->setName("Stats");

    // MoonTalk, the public message board and a SECOND MoonCloud child with its own consent: @xref{why-mooncloud-is-not-a-firmware-child}.
    auto* moonTalkModule = createOrDie<mm::MoonTalkModule>("MoonTalkModule");
    moonTalkModule->setName("Talk");

    // Network (platform stubs return false on desktop, module is a no-op)
    auto* networkModule = createOrDie<mm::NetworkModule>("NetworkModule");
    networkModule->setScheduler(&scheduler);
    networkModule->setSystemModule(systemModule);

    // Listens on the serial port for pushed WiFi credentials, compile-time gated: @xref{why-improv-is-compile-time-gated}.
    mm::ImprovProvisioningModule* improvModule = nullptr;
    if constexpr (mm::platform::hasImprov) {
        improvModule = createOrDie<mm::ImprovProvisioningModule>("ImprovProvisioningModule");
        improvModule->setSystemModule(systemModule);
        improvModule->setNetworkModule(networkModule);
        // Marked wired-by-code so the trim loop preserves it on a device whose saved tree predates this child: @xref{why-markwiredbycode-matters}.
        improvModule->markWiredByCode();
    }

    // MQTT service, a code-wired child of Network bridging the light controls to a broker: @xref{why-mqtt-is-built-on-every-networked-target}.
    mm::MqttModule* mqttModule = nullptr;
    if constexpr (mm::platform::hasNetwork) {
        mqttModule = createOrDie<mm::MqttModule>("MqttModule");
        mqttModule->setSystemModule(systemModule);
        mqttModule->setControlModule(controlModule);   // look-only presets as the HA effect list
        mqttModule->markWiredByCode();
    }

    // Layouts, the top-level container for one or more layouts, today one GridLayout that self-initializes to defaultGridSize with no boot-time dimensions threaded in.
    auto* layouts = createOrDie<mm::Layouts>("Layouts");
    auto* grid = createOrDie<mm::GridLayout>("GridLayout");
    layouts->addChild(grid);

    // Effects: top-level container; one or more layers, each rendering into its own buffer. Today one Layer with one effect + one modifier.
    auto* effectsContainer = createOrDie<mm::Effects>("Effects");
    auto* layer = createOrDie<mm::Layer>("Layer");
    layer->setChannelsPerLight(3);
    effectsContainer->addChild(layer);
    // setLayouts wires the shared Layouts to the container AND propagates to every child Layer.
    effectsContainer->setLayouts(layouts);

    // One default effect so a bare device still shows lights out of the box, but NO default modifier: @xref{why-the-boot-layer-is-one-pulse-effect}.
    auto* pulse = createOrDie<mm::MoonModule>("PulseEffect");
    layer->addChild(pulse);

    // Bound to the effects container rather than to a single layer. A layer rebuilt through the API self-heals without re-running this wiring, and one driver can read across several layer buffers from one place.
    auto* drivers = createOrDie<mm::Drivers>("Drivers");
    drivers->setEffects(effectsContainer);

    // Output drivers are added per board through the catalog rather than boot-wired, the preview being the one exception: @xref{why-output-drivers-are-not-boot-wired}.

    // The preset library, a boot-wired singleton owning the named channel-role wirings every driver references by id, resolved through its own seat since exactly one exists.
    auto* lightPresets =
        createOrDie<mm::LightPresetsModule>("LightPresetsModule");
    drivers->addChild(lightPresets);
    lightPresets->markWiredByCode();

    auto* preview = createOrDie<mm::PreviewDriver>("PreviewDriver");
    drivers->addChild(preview);
    // Marked wired-by-code, the same protection ImprovProvisioning uses: @xref{why-markwiredbycode-matters}.
    preview->markWiredByCode();

    auto* httpServer = createOrDie<mm::HttpServerModule>("HttpServerModule");
    httpServer->port = httpPort;
    httpServer->setScheduler(&scheduler);
    // PreviewDriver pushes the coordinate table and per-frame RGB to the HTTP server's WS broadcaster: @xref{why-output-drivers-are-not-boot-wired}.
    preview->setBroadcaster(httpServer);

    // The APPLY_OP vendor RPC, wired here once httpServer exists: @xref{why-improv-is-compile-time-gated}.
    if (improvModule) improvModule->setHttpServerModule(httpServer);

    // Registration order matters, and the scheduler walks the roots in it each tick: @xref{why-registration-order-matters}.
    scheduler.addModule(filesystemModule);
    scheduler.addModule(systemModule);
    scheduler.addModule(fileManagerModule);
    scheduler.addModule(firmwareUpdateModule);
    // Both are boot wiring, not user-added, so the persisted tree must not decide whether they exist: @xref{why-markwiredbycode-matters}.
    moonStatsModule->markWiredByCode(); moonCloudModule->addChild(moonStatsModule);
    moonTalkModule->markWiredByCode();  moonCloudModule->addChild(moonTalkModule);
    scheduler.addModule(moonCloudModule);
    if (improvModule) networkModule->addChild(improvModule);
    if (mqttModule) networkModule->addChild(mqttModule);
    // Devices discovers other devices on the LAN, a Network child since discovery depends on the network being up, and wired-by-code (see DevicesModule.md).
    auto* devicesModule = createOrDie<mm::DevicesModule>("DevicesModule");
    devicesModule->markWiredByCode();
    // Wire our own name so the self row in the device list matches the device's identity elsewhere. deviceName has static lifetime as SystemModule's member, so the module borrows the pointer.
    devicesModule->setSelfName(systemModule->deviceName());
    networkModule->addChild(devicesModule);
    scheduler.addModule(networkModule);
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
        mm::platform::yield();
        mm::platform::pauseLoop();
    }

    std::printf("\nShutting down.\n");
    scheduler.release();
}
