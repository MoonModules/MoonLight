#pragma once

#include "light/effects/EffectBase.h"
#include "core/services/AudioDemand.h"   // the analysis, while this effect reads it

namespace mm {

/// An effect that reads the audio analysis, the bands, the onsets or the peak: it needs the analysis running while it is on, as Demand.h describes, and an effect reading only the level, which runs regardless, stays an EffectBase.
class AudioEffectBase : public EffectBase {
public:
    /// A switch that turns this effect's audio on or off, `audioReactive` or `goboOnBeat`, prepares it again, so its demand follows.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "audioReactive") == 0 || std::strcmp(name, "goboOnBeat") == 0; }
    /// Only this effect's demand follows it.
    PrepareScope prepareScope() const override { return PrepareScope::Self; }

    /// Need the analysis while this effect reads it; an effect's own prepare chains this first.
    void prepare() override {
        EffectBase::prepare();
        analysis_.set(readsAnalysis());
    }

protected:
    /// Whether this effect reads the analysis now; an effect whose audio is a switch answers with the switch.
    virtual bool readsAnalysis() const { return true; }

private:
    AudioDemand analysis_{*this, AudioFeature::Bands};   ///< the analysis, while this effect reads it
};

}  // namespace mm
