/// @module DriverBase
/// @also HdmiDriver, NdiDriver, HlsDriver, RtspDriver, Hub75Driver, PreviewDriver

/// Every driver that lights something shows start and count, one window on the shared buffer, so two outputs can each take their part of one canvas.

#include "doctest.h"
#include "core/util/ModuleFactory.h"
#include "light/drivers/DriverBase.h"
#include "module_types.h"

#include <cstring>
#include <string>

TEST_CASE("every driver that lights something offers start and count; the preview, a picture of the output, and HUB75 do not") {
    mm::registerModuleTypes();
    int drivers = 0;
    for (uint8_t i = 0; i < mm::ModuleFactory::typeCount(); i++) {
        // The registry's own drivers, each with its doc page: a test elsewhere registers stubs of the role that are no DriverBase.
        if (mm::ModuleFactory::typeRole(i) != mm::ModuleRole::Driver || !mm::ModuleFactory::typeDocPath(i)[0]) continue;
        const char* type = mm::ModuleFactory::typeName(i);
        mm::MoonModule* m = mm::ModuleFactory::create(type);
        REQUIRE(m != nullptr);
        const auto* d = static_cast<mm::DriverBase*>(m);
        const bool expected = d->drivesLights() && std::strcmp(type, "Hub75Driver") != 0;   // HUB75 opens its panels at the layer's full size
        CHECK_MESSAGE(d->hasWindowControls() == expected, std::string(type));
        drivers++;
        delete m;
    }
    CHECK(drivers > 0);
}
