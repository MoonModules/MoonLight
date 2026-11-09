#pragma once

#include <cstdint>

namespace mm {

class MoonModule;  // forward: registering needs the full definition, which ModuleResource.cpp includes

/// Something a module holds only while it is enabled, which the module lets go of on release, as it frees its buffers: a ScratchBuffer, a Demand.
///
/// A module declares one as a member; the resource registers itself, and the module's release walks the list, so no module has to remember to let go.
class ModuleResource {
public:
    /// Non-copyable: a member tied to one module and threaded into its list.
    ModuleResource(const ModuleResource&) = delete;
    /// Nor copy-assigned.
    ModuleResource& operator=(const ModuleResource&) = delete;
    /// Nor moved, which would dangle the module's list.
    ModuleResource(ModuleResource&&) = delete;
    /// Nor move-assigned.
    ModuleResource& operator=(ModuleResource&&) = delete;

    /// The features this resource asks of the module of type `provider`, a bit each by the provider's numbering; none for a buffer.
    virtual uint8_t demandsOf(const char* /*provider*/) const { return 0; }
    /// The next resource of the same module, null at the end.
    const ModuleResource* nextResource() const { return next_; }

protected:
    /// Registers with the owning module, which lets go of it on release.
    explicit ModuleResource(MoonModule& owner);
    /// Deregisters from the still-alive module.
    virtual ~ModuleResource();
    /// Let go of what this holds, as the module's release asks.
    virtual void releaseResource() = 0;

    MoonModule& owner_;   ///< the module this belongs to

private:
    friend class MoonModule;           // walks next_ on release
    ModuleResource* next_ = nullptr;   // intrusive singly-linked list node
};

} // namespace mm
