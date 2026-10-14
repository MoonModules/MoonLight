#pragma once

#include "light/effects/ChannelDataSink.h"   // where the frames land, held and placed, as Network Receive's do
#include "light/effects/EffectBase.h"

namespace mm {

/// An FSEQ show played from a file: the frame-by-frame channel data xLights and Falcon Player write.
/// @card FseqPlayerEffect.gif The reference sweep fseq_play writes, the show an FSEQ player plays
///
/// It plays uncompressed FSEQ v2. A show lands where a sender-mapped [Network Receive](https://moonmodules.org/MoonLight/moonmodules/light/effects.html#networkreceive) stream does, so alone on its layer it reaches the lights in one copy on any layout.
///
/// @attribution FSEQ v2 as Falcon Player documents it in FSEQ_Sequence_File_Format.txt.
class FseqPlayerEffect : public EffectBase {
public:
    /// Catalog tags: a recorded show, played back.
    const char* tags() const override { return "🎞️"; }
    /// The show's path, an `.fseq` file on flash or under `/sd`.
    char file[64] = "";
    /// Start again at the end.
    bool loop = true;
    /// The playback speed, in percent of the file's step time.
    uint8_t speed = 100;

    /// Publish the file, looping and the speed.
    void defineControls() override {
        controls_.addText("file", file, sizeof(file));   // a path: the file picker opens a text editor, which a binary show has no use for
        controls_.addControl("loop", loop);
        controls_.addControl("speed", speed, 1, 255);
    }

    /// A new file sizes the sink again.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "file") == 0; }

    /// Size the sink to the layer in wiring order, a frame of the file being one light per physical light.
    void prepare() override {
        sink_.prepare(/*senderMapped=*/true);
        setStatus("reads no FSEQ file yet: playback is the next step");
    }

    /// Let the sink place the held frame.
    void tick() MM_NONBLOCKING override { sink_.tick(); }

    /// An FSEQ frame is in wiring order.
    bool deliversWiringOrder() const MM_NONBLOCKING override { return sink_.wiringOrder(); }
    /// The sink hands its held frame to the output.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) override { sink_.copyWiringFrame(out, bytes, force); }

private:
    ChannelDataSink sink_{*this};   ///< where the frames land, held and placed
};

}  // namespace mm
