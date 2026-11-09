#pragma once

#include <cstdint>

#include "core/module/ModuleResource.h"

namespace mm {

/// One module's need for a feature another module computes: the provider, named by its type, computes a feature only while some module needs it, @xref{a-feature-runs-while-something-needs-it}.
///
/// @technical
///
/// ## A feature runs while something needs it
///
/// A provider, such as AudioService, computes some features only on demand: the notes are about half the audio analysis. A reader declares a Demand as a member, naming the provider's type and the feature's bit, and turns it on in its prepare from its controls.
///
/// The module's release, which every removal and every disable runs, turns it off, as it frees its buffers, so no reader forgets to. A change marks the provider, and the Scheduler tells it through `onDemandChanged` at the next frame boundary, where allocating is allowed: a release and a prepare in one frame, an effect replaced, settle once, and nothing switches on the render path.
///
/// The provider then asks the Scheduler what the tree demands of it, a reference count over the modules, walked only on a change and never per frame. It counts again rather than adding up changes, so a demand placed before the provider exists still counts.
class Demand : public ModuleResource {
public:
    /// `owner`'s need for feature bit `feature` of the module of type `provider`, off until the owner turns it on.
    Demand(MoonModule& owner, const char* provider, uint8_t feature) : ModuleResource(owner), provider_(provider), bit_(feature) {}
    /// Forget the need as the module goes, telling no one: the module's release, which every removal runs first, already told the provider, and a tree being deleted is not walked.
    ~Demand() override { on_ = false; }

    /// Need the feature, or not; a change marks the provider, which hears of it at the next frame boundary.
    void set(bool on);
    /// Whether the need is on.
    bool on() const { return on_; }
    /// The feature's bit while the need is on and addressed to `provider`.
    uint8_t demandsOf(const char* provider) const override;

protected:
    /// The module's release: the need goes with it.
    void releaseResource() override { set(false); }

private:
    const char* provider_;   // the providing module's type name
    uint8_t bit_;            // the feature's bit, by the provider's numbering
    bool on_ = false;        // whether the need is on
};

} // namespace mm
