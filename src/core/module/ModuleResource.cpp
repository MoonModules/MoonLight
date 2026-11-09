/// The out-of-line half of ModuleResource: registering with the module, which needs its full definition.

#include "core/module/ModuleResource.h"

#include "core/module/MoonModule.h"

namespace mm {

ModuleResource::ModuleResource(MoonModule& owner) : owner_(owner) {
    owner_.registerResource(this);   // push onto the module's list
}

ModuleResource::~ModuleResource() {
    owner_.deregisterResource(this);
}

} // namespace mm
