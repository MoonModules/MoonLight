/// @module Hub75Driver
/// @also Hub75PioOutput, DriverBase

/// What Hub75Driver does with a Raspberry Pi's PIO beside the bus: the settings a saved card keeps, the output it swaps to and back, the peripheral one sibling drives refused to the next, and what its status says while the panels run and after they stop. The Pi output's own behavior is in unit_Hub75PioOutput.cpp and unit_Hub75PiOutput.cpp; the bus output's is in unit_Hub75Driver_output.cpp.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/util/JsonSink.h"
#include "../core/conditional_controls.h"   // mm::isHidden, mm::isVisible
#include "light/drivers/Hub75Driver.h"
#ifdef MM_HUB75_PIO_HAS_ENGINE   // the stub backend and the engine are built where the compiler takes them (the root CMakeLists.txt)
    #include "platform/rpi/hub75_pio_backend_stub.h"
    #include "../core/hub75_engine_bench.h"
    #include <chrono>
    #include <thread>
#endif
#include "platform/rpi/hub75_gpio_backend.h"   // kGpioTestRefusal: what a closed gate says
#include "hub75_pi_bench.h"
#include "platform/platform_leds.h"   // Hub75Backend, hub75BackendLabel

#include <cstring>
#include <memory>
#include <string>

namespace {

// A driver in a running tree, so a stored document applies to it as a boot does; on the heap, past a test's frame limit.
struct Booted {
    mm::Scheduler scheduler;
    mm::Hub75Driver* driver = new mm::Hub75Driver();   // the scheduler owns it, as the module owner does, and releases and deletes it with the tree

    Booted() {
        driver->setTypeName("Hub75Driver");
        driver->setName("Hub75Driver");
        scheduler.addModule(driver);
        scheduler.setup();
    }

    /// Apply a stored document, as a boot does.
    bool apply(const std::string& doc) { return mm::applyStateDocument(scheduler, doc.c_str(), mm::StateSource::Stored).ok; }

    /// The driver as a state document, the bytes a save writes.
    std::string saved() {
        mm::JsonSink sink;
        sink.append("{");
        mm::writeStateMember(sink, *driver, false);
        sink.append("}");
        return std::string(sink.data(), sink.size());
    }
};

// The PIO card as a user saved it: a small wall at 20 MHz with two blank words at 8 bits.
const char* const kSavedPioCard =
    "{\"Hub75Driver\":{\"peripheral\":0,\"rows\":32,\"cols\":64,\"panelsPerLane\":1,\"pixelClock\":20000,\"latchBlank\":2,\"bitDepth\":8}}";

bool has(const std::string& doc, const char* text) { return doc.find(text) != std::string::npos; }

// The label the select `name` shows for the option it holds.
std::string textOfSelect(mm::MoonModule& m, const char* name) {
    const int i = mm::controlIndex(m, name);
    if (i < 0) return {};
    const mm::ControlDescriptor& c = m.controls()[static_cast<uint8_t>(i)];
    const char* const* options = reinterpret_cast<const char* const*>(c.aux);
    return options[*static_cast<const uint8_t*>(c.ptr)];
}

// Two HUB75 drivers under one container on a 16x32 layout, the box the default 1/16 scan pairs into rows; on the heap, since drivers and a layer outgrow a test's stack frame.
struct Siblings {
    mm::MoonModule owner;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::Buffer source;
    mm::Hub75Driver first;
    mm::Hub75Driver second;

    Siblings() {
        grid.width = 16;
        grid.height = 32;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(16 * 32, 3));
        for (mm::Hub75Driver* d : {&first, &second}) {
            owner.addChild(d);
            d->setLayer(&layer);
            d->setSourceBuffer(&source);
            mm::Correction correction;
            mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);
            d->correctionForTest() = correction;
            d->applyState();
        }
    }
    // The owner releases a driver before destroying it, which frees what prepare() allocated.
    ~Siblings() {
        for (mm::Hub75Driver* d : {&first, &second}) {
            d->release();
            owner.removeChild(d);
        }
    }
};

// A 16x32 raster on the Pi PIO output, opened and showing its plain status; the seam is the caller's guard's.
std::unique_ptr<mm::PiRig<mm::PioSeam>> openedHealthyRaster() {
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    return r;
}

bool says(const mm::MoonModule& m, const char* text) {
    return m.status() && std::strstr(m.status(), text) != nullptr;
}

}  // namespace

TEST_CASE("Hub75Driver reloads a saved Pi PIO card unchanged") {
    mm::PioPortGuard guard;
    auto first = std::make_unique<Booted>();
    REQUIRE(first->apply(kSavedPioCard));
    CHECK(first->driver->panelRows == 32);
    CHECK(first->driver->panelCols == 64);
    CHECK(first->driver->panelsPerLane == 1);
    CHECK(first->driver->pixelClockKhz == 20000);
    CHECK(first->driver->latchBlankWords == 2);
    CHECK(first->driver->bitDepth == 8);
    CHECK(first->driver->selectedBackend() == mm::platform::Hub75Backend::Pio);

    const std::string doc = first->saved();
    for (const char* kept : {"\"rows\":32", "\"cols\":64", "\"panelsPerLane\":1", "\"pixelClock\":20000", "\"latchBlank\":2", "\"bitDepth\":8"}) {
        CAPTURE(kept);
        CHECK(has(doc, kept));
    }
    auto second = std::make_unique<Booted>();   // a restart: the saved bytes, as written, into a fresh driver
    REQUIRE(second->apply(doc));
    CHECK(second->saved() == doc);
}

TEST_CASE("Hub75Driver on a Pi 5 keeps the PIO output when the PIO refuses, shows why, and loses no saved setting, the 8-bit depth included") {
    std::string doc;
    {
        mm::PioPortGuard guard;
        auto booted = std::make_unique<Booted>();
        REQUIRE(booted->apply(kSavedPioCard));
        doc = booted->saved();
    }
    {
        // The next boot finds the PIO refused (a stall kernel, a missing node, no permission): the machine is still a Pi 5, so the PIO output stays and says why.
        mm::PioPortGuard refusing;   // a Pi 5 whose PIO the seam refuses, for a reason the case names
        mm::platform::setTestHub75PioOpenFails(true);
        auto refused = std::make_unique<Booted>();
        REQUIRE(refused->apply(doc));
        CHECK(refused->driver->selectedBackend() == mm::platform::Hub75Backend::Pio);
        CHECK(refused->driver->hwBlock() == mm::LedHwBlock::None);   // the output refused, so no block is held
        CHECK(mm::platform::hub75PioWidth() == 0);
        const std::string saved = refused->saved();
        CHECK(saved == doc);   // every setting survives untouched
        CHECK(has(saved, "\"bitDepth\":8"));
        for (const char* name : {"rows", "cols", "panelsPerLane", "pixelClock", "latchBlank", "bitDepth"}) {
            CAPTURE(name);
            CHECK(mm::isVisible(*refused->driver, name));   // the Pi card, not the bus's
        }
    }
    {
        mm::PioPortGuard guard;   // the PIO is back
        auto back = std::make_unique<Booted>();
        REQUIRE(back->apply(doc));
        CHECK(back->saved() == doc);
        CHECK(back->driver->bitDepth == 8);
    }
}

TEST_CASE("Hub75Driver on a Pi 5 whose PIO refuses shows the refusal as its status") {
    mm::PioPortGuard guard;
    mm::platform::setTestHub75PioOpenFails(true);   // the seam refuses with its reason
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(r->driver.status()) == "the test seam refuses the open");
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    CHECK(std::string(r->driver.status()).find("HUB75 needs an ESP32") == std::string::npos);   // the PIO's reason, not the bus's
}

TEST_CASE("Hub75Driver on an earlier Pi keeps the GPIO output when the engine refuses, shows why, and keeps every setting") {
    mm::GpioPortGuard pi4(mm::platform::Hub75GpioMode::Off);   // a Pi 3 or 4 whose GPIO the seam refuses, the test binary keeping the real backend closed
    auto booted = std::make_unique<Booted>();
    REQUIRE(booted->apply(kSavedPioCard));
    CHECK(std::string(mm::platform::hub75BackendLabel(booted->driver->selectedBackend())) == "Pi GPIO");   // the Pi card's output, offered whether or not it opens
    CHECK(textOfSelect(*booted->driver, "peripheral") == "Pi GPIO");
    CHECK(mm::isVisible(*booted->driver, "rows"));
    CHECK(mm::isHidden(*booted->driver, "pixelClock"));   // the PIO's clock and blanking, which the GPIO output ignores, so its card hides them
    CHECK(mm::isHidden(*booted->driver, "latchBlank"));
    CHECK(booted->driver->bitDepth == 8);
    const std::string saved = booted->saved();
    for (const char* kept : {"\"rows\":32", "\"cols\":64", "\"panelsPerLane\":1", "\"pixelClock\":20000", "\"latchBlank\":2", "\"bitDepth\":8"}) {
        CAPTURE(kept);
        CHECK(has(saved, kept));
    }
    // A layout for it to prepare on: the seam's refusal, not an open.
    auto bench = std::make_unique<mm::Hub75Bench>();
    bench->grid.width = 16;
    bench->grid.height = 32;
    bench->grid.depth = 1;
    bench->layouts.addChild(&bench->grid);
    bench->wire(16 * 32);
    bench->driver.defineControls();
    bench->driver.panelRows = 32;
    bench->driver.panelCols = 16;
    bench->driver.panelsPerLane = 1;
    bench->open();
    CHECK(bench->driver.severity() == mm::MoonModule::Severity::Error);
    CHECK(std::string(bench->driver.status()) == mm::platform::kGpioTestRefusal);   // the GPIO's reason, not the PIO's or the bus's
    CHECK(bench->driver.hwBlock() == mm::LedHwBlock::None);
}

TEST_CASE("Hub75Driver on another host takes the bus output and its own refusal, the Pi settings hidden and kept") {
    mm::MachineGuard other(mm::platform::Machine::Other);
    auto booted = std::make_unique<Booted>();
    CHECK(booted->driver->outputForTest()->hwBlock() != mm::LedHwBlock::PiHeader);
    REQUIRE(booted->apply(kSavedPioCard));
    for (const char* name : {"rows", "cols", "panelsPerLane", "pixelClock", "latchBlank"}) {
        CAPTURE(name);
        CHECK(mm::isHidden(*booted->driver, name));
    }
    const std::string saved = booted->saved();
    CHECK(has(saved, "\"rows\":32"));
    CHECK(has(saved, "\"pixelClock\":20000"));
    CHECK(has(saved, "\"bitDepth\":6"));   // the bus's range clamps the depth
}

TEST_CASE("Hub75Driver on an ESP32 offers the bus backends and falls back within them") {
    mm::MachineGuard esp(mm::platform::Machine::Esp32);
    {
        mm::PortGuard port(true);   // LCD_CAM and Parlio both
        auto r = std::make_unique<Booted>();
        CHECK(textOfSelect(*r->driver, "peripheral") == "LCD_CAM");
        const int parlio = mm::optionIndex(*r->driver, "peripheral", "Parlio");
        REQUIRE(parlio >= 0);
        CHECK(mm::optionIndex(*r->driver, "peripheral", "Pi PIO") == -1);   // an ESP32 lists its bus backends only
        r->driver->peripheralSel_ = static_cast<uint8_t>(parlio);
        r->driver->rebuildControls();
        CHECK(r->driver->selectedBackend() == mm::platform::Hub75Backend::Parlio);
        CHECK(r->driver->outputForTest()->hwBlock() == mm::LedHwBlock::Parlio);
        r->driver->peripheralSel_ = 9;   // a select past the options answers the first on offer
        CHECK(r->driver->selectedBackend() == mm::platform::Hub75Backend::LcdCam);
    }
    {
        mm::PortGuard port(false);   // the silicon has LCD_CAM alone: a saved Parlio finds it gone and takes the first
        auto r = std::make_unique<Booted>();
        CHECK(mm::optionIndex(*r->driver, "peripheral", "Parlio") == -1);
        CHECK(r->driver->selectedBackend() == mm::platform::Hub75Backend::LcdCam);
    }
}

TEST_CASE("Hub75Driver holds both outputs by value, so its size and memory readout count them") {
    // The bus output and the PIO output are members side by side, the PIO output an empty type where no Pi 5 can run it.
    CHECK(sizeof(mm::Hub75Driver) >= sizeof(mm::DriverBase) + sizeof(mm::Hub75BusOutput) + (mm::platform::hasHub75Pio ? sizeof(mm::Hub75PioOutput) : 0));
    mm::PioPortGuard guard;
    auto r = mm::PiRig<mm::PioSeam>::cabled(2, 1, 16, 32);   // two 16x32 panels in one lane: a canvas to count
    r->driver.panelsPerLane = 2;
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(r->driver.dynamicBytes() == 2 * 1024 * 3);   // the frame and the canvas, counted once each
}

TEST_CASE("Hub75Driver moves its depth into the new output's range when the machine changes its output") {
    mm::PortGuard bus;
    mm::PioPortGuard guard;   // a Pi 5
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->driver.bitDepth = 8;   // the PIO's range reaches 8
    becomeMachine(r->driver, mm::platform::Machine::Other);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::LcdCam);
    CHECK(r->driver.bitDepth == mm::kHub75PortMaxBitDepth);   // the bus takes 6: the swap clamps, so hub75Init gets 6 at most
    becomeMachine(r->driver, mm::platform::Machine::Pi5);
    CHECK(r->driver.selectedBackend() == mm::platform::Hub75Backend::Pio);
    CHECK(r->driver.bitDepth == mm::kHub75PortMaxBitDepth);   // and does not climb back by itself
    const int depth = mm::controlIndex(r->driver, "bitDepth");
    REQUIRE(depth >= 0);
    CHECK(r->driver.controls()[static_cast<uint8_t>(depth)].max == mm::kHub75EncodeMaxBitDepth);
}

TEST_CASE("Hub75Driver quiesces the render worker before it swaps outputs, and prepares as a whole tree") {
    static int quiesced;
    quiesced = 0;
    mm::MoonModule::setQuiesceRenderHook([] { quiesced++; });
    mm::PortGuard bus;
    mm::PioPortGuard guard;
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->open();
    quiesced = 0;
    becomeMachine(r->driver, mm::platform::Machine::Other);
    CHECK(quiesced >= 1);   // the encode worker reads the output in tick(), so it stops before the output moves
    CHECK(r->driver.hwBlock() == mm::LedHwBlock::None);   // and the Pi output closed
    // The default scope re-prepares the parent and its children, which joins the encode worker before an output closes; a driver scoped to itself would not.
    CHECK(r->driver.prepareScope() == mm::PrepareScope::Tree);
    mm::MoonModule::setQuiesceRenderHook(nullptr);
}

TEST_CASE("Hub75Driver shows the frames the PIO shows each second") {
    mm::PioPortGuard guard;
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->open();
    mm::platform::setTestHub75PioFps(61);
    r->driver.tick1s();
    CHECK(textOf(r->driver, "shown") == "61 fps");
    r->driver.release();
    CHECK(textOf(r->driver, "shown").empty());
}

TEST_CASE("Hub75Driver warns while the PIO's submits are failing, and says nothing once they stop") {
    mm::PioPortGuard guard;
    auto r = openedHealthyRaster();
    r->driver.tick1s();   // the count the seam starts at

    mm::platform::setTestHub75PioSubmitErrors(3);
    r->driver.tick1s();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("the PIO is failing: 3 submits failed") != std::string::npos);

    mm::platform::setTestHub75PioSubmitErrors(5);   // still rising
    r->driver.tick1s();
    CHECK(std::string(r->driver.status()).find("5 submits failed") != std::string::npos);

    r->driver.tick1s();   // no new failures in the last second
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(r->driver.status()) == "16x32, 6-bit, PIO");
}

TEST_CASE("Hub75Driver warns while a submit is stalled, alone, and says nothing once it returns") {
    mm::PioPortGuard guard;
    auto r = openedHealthyRaster();
    mm::platform::setTestHub75PioStalled(true);
    mm::platform::setTestHub75PioSubmitErrors(4);   // failures too: the stall, the graver, shows
    r->driver.tick1s();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("the PIO is stalled") != std::string::npos);
    CHECK(std::string(r->driver.status()).find("failing") == std::string::npos);
    mm::platform::setTestHub75PioStalled(false);
    r->driver.tick1s();   // the submit returned, and no failure rose since
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Status);
}

TEST_CASE("Hub75Driver joins the failing submits to the warnings it already shows, none hiding another") {
    mm::PioPortGuard guard;
    auto r = mm::PiRig<mm::PioSeam>::raster(20, 40);   // a raster wider than the panel: the walk's warning
    r->open();
    const std::string before = r->driver.status();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Warning);
    r->driver.tick1s();
    mm::platform::setTestHub75PioSubmitErrors(2);
    r->driver.tick1s();
    const std::string failing = r->driver.status();
    CHECK(failing.rfind(before, 0) == 0);   // what was there stays first, whole
    CHECK(failing.find("; the PIO is failing: 2 submits failed") != std::string::npos);
}

TEST_CASE("Hub75Driver warns of a pixel clock past the 20 MHz the bench ran, built from the seam's constant") {
    mm::PioPortGuard guard;
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->driver.pixelClockKhz = 25000;
    r->open();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("above 20 MHz") != std::string::npos);
}

#ifdef MM_HUB75_PIO_HAS_ENGINE   // a stuck submit needs the engine on the stub backend
TEST_CASE("Hub75Driver says why a close the kernel never let finish left the PIO held") {
    mm::PioPortGuard guard(mm::platform::Hub75PioMode::Stub);
    mm::platform::setTestHub75PioJoinDeadlineMs(100);
    auto r = mm::PiRig<mm::PioSeam>::raster(16, 32);
    r->open();
    REQUIRE(r->driver.severity() == mm::MoonModule::Severity::Status);
    REQUIRE(mm::test::waitUntil([] { return mm::platform::stubBackendSubmittedFrames() > 0; }));
    mm::test::hangSubmits();

    r->driver.release();
    CHECK(r->driver.severity() == mm::MoonModule::Severity::Warning);
    CHECK(std::string(r->driver.status()).find("did not return") != std::string::npos);

    mm::platform::stubBackendSetFaults({});   // the kernel lets the submit go
    CHECK(mm::test::waitUntil([&] {           // the next prepare closes it for good, then opens
        r->driver.prepare();
        return r->driver.severity() == mm::MoonModule::Severity::Status;
    }));
}
#endif

TEST_CASE("Hub75Driver stays idle and names why while a sibling driver drives its peripheral") {
    mm::PortGuard guard;
    auto s = std::make_unique<Siblings>();
    s->first.prepare();
    REQUIRE(s->first.hwBlock() == mm::LedHwBlock::LcdCam);

    s->second.prepare();
    CHECK(says(s->second, "already in use by another driver"));
    CHECK(s->second.severity() == mm::MoonModule::Severity::Error);
    CHECK(s->second.hwBlock() == mm::LedHwBlock::None);
    CHECK(s->first.hwBlock() == mm::LedHwBlock::LcdCam);
}

TEST_CASE("Hub75Driver takes the peripheral once the sibling driving it releases it") {
    mm::PortGuard guard;
    auto s = std::make_unique<Siblings>();
    s->first.prepare();
    s->second.prepare();
    REQUIRE(s->second.hwBlock() == mm::LedHwBlock::None);

    s->first.release();
    CHECK(s->first.hwBlock() == mm::LedHwBlock::None);
    s->second.prepare();
    CHECK(says(s->second, "1/16 scan"));
    CHECK(s->second.hwBlock() == mm::LedHwBlock::LcdCam);
}

TEST_CASE("Hub75Driver gives the Pi's header to the first of two drivers that prepare, and the second keeps idle") {
    mm::PioPortGuard guard;
    auto s = std::make_unique<Siblings>();
    for (mm::Hub75Driver* d : {&s->first, &s->second}) {
        REQUIRE(pickBackend(*d, "Pi PIO"));
        d->panelRows = 32;   // one 16x32 panel on a lane, the layout's own box
        d->panelCols = 16;
        d->panelsPerLane = 1;
    }
    s->second.prepare();   // the order of the prepares decides, not the order of the siblings
    REQUIRE(s->second.hwBlock() == mm::LedHwBlock::PiHeader);
    s->first.prepare();
    CHECK(says(s->first, "already in use by another driver"));
    CHECK(s->first.hwBlock() == mm::LedHwBlock::None);
    s->second.release();
    s->first.prepare();   // and once the holder lets go, the other takes it
    CHECK(s->first.hwBlock() == mm::LedHwBlock::PiHeader);
}

TEST_CASE("Hub75Driver on the bus shows its plain status when the layout holds more lights than its box") {
    mm::PortGuard guard;
    mm::GridLayout overlap;   // a second grid on the first: a 16x32 box holding 1,024 lights
    overlap.width = 16;
    overlap.height = 32;
    overlap.depth = 1;
    auto s = std::make_unique<Siblings>();
    s->layouts.addChild(&overlap);
    s->layouts.applyState();
    s->layer.applyState();
    REQUIRE(s->layer.physicalLightCount() == 1024);

    s->first.prepare();
    CHECK(s->first.severity() == mm::MoonModule::Severity::Status);
    CHECK(std::string(s->first.status()).rfind("16x32, 1/16 scan", 0) == 0);
    s->layouts.removeChild(&overlap);
}

TEST_CASE("Hub75Driver keeps its status readable across a change of machine that swaps its output") {
    mm::PortGuard guard;
    auto s = std::make_unique<Siblings>();   // a host with a bus port: the bus output
    s->first.defineControls();
    s->first.prepare();
    REQUIRE(says(s->first, "1/16 scan"));
    const std::string shown = s->first.status();

    mm::PioPortGuard recorder;   // the host turns out to be a Pi 5
    becomeMachine(s->first, mm::platform::Machine::Pi5);
    CHECK(std::string(s->first.status()) == shown);   // the last prepare's status, until the next prepare says otherwise
}
