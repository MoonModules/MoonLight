#pragma once

#include "light/effects/ChannelDataSink.h"   // where the frames land, held and placed, as Network Receive's do
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: an FSEQ show played from a file, xLights' and Falcon Player's frame-by-frame channel data.
///
/// FSEQ is mapped by the sender, so its frames land in the channel-data sink as a sender-mapped Network Receive stream does, one copy alone on its layer.
///
/// It reads uncompressed v2 as FPP's FSEQ_Sequence_File_Format.txt lays it out, and `moondeck/run/fseq_play.py` writes reference files.
///
/// It reads no file yet and says so: the reading lands in prepare, which opens the file and sizes the sink, and in tick, which writes the clock's frame.
class FseqPlayerEffect : public EffectBase {
public:
    /// Catalog tags: a recorded show, played back.
    const char* tags() const override { return "🎞️"; }
    /// The show to play, a path on flash or SD.
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
