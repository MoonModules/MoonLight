#pragma once

#include "light/effects/AudioEffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // the bar scales and barsOf
#include "core/services/AudioDemand.h"           // the notes, while the bars show the keys

#include <cstring>   // strcmp

namespace mm {

/// An effect drawing the audio as bars: `bands` picks the sixteen, the third octaves or the piano's keys, and the notes run only while it shows them, as Demand.h describes.
class AudioBarsBase : public AudioEffectBase {
public:
    /// The bars: the sixteen, the third octaves or the piano's keys.
    uint8_t bands = kBandsThirds;

    /// `bands` decides whether the notes run, so its change prepares this effect again.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "bands") == 0; }
    /// Only this effect's demand follows it.
    PrepareScope prepareScope() const override { return PrepareScope::Self; }
    /// Need the notes while what this effect shows reads them; an effect's own prepare chains this first.
    void prepare() override {
        AudioEffectBase::prepare();
        notes_.set(readsNotes());
    }

protected:
    /// Whether what this effect shows reads the notes: the keys as bars.
    virtual bool readsNotes() const { return bands == kBandsNotes; }
    /// Publish `bands`, where the effect places it among its own controls.
    void addBandsSelect() { controls_.addSelect("bands", bands, kBarScaleOptions, kBandsCount); }

private:
    AudioDemand notes_{*this, AudioFeature::Notes};   ///< the notes, while what this effect shows reads them
};

}  // namespace mm
