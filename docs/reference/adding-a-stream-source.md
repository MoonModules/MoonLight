# Adding a picture or channel-data source

A new way for frames to reach the lights, from a capture device, a file format or a network protocol.
Every source is one of two families, and each family has one place where its frames land.
So a source is small: it fetches frames and hands them over, and everything after that is already written and tested.

The steps are the process [Add a picture or channel-data source](../moonwalk.md#add-stream-source). What follows is the reference behind it: the families, where their frames land, and the contract of each.

---

## Which family

| Your frames are | The family | Where they land | Examples |
|---|---|---|---|
| width × height pixels, any size | picture | [Picture service](../moonmodules/core/services.md#picture) | USB capture, a camera, a GIF, a browser tab |
| one value per light, in wiring order or row by row | channel data | the channel-data sink | Art-Net, E1.31, DDP, an FSEQ file |

A picture knows nothing of the lights: the [Picture effect](../moonmodules/light/effects.md#picture) samples it onto whatever layout the layer has, fitted, filled or stretched.
Channel data is already per light: it is placed through the layer's mapping, or straight to the output when the sender mapped it.

---

## A picture source

A picture source is an option of the Picture service's `source`, in `src/core/services/PictureService.h`.

Steps: [Add a picture or channel-data source](../moonwalk.md#add-stream-source), the Picture choice.

The contract the steps rest on: `beginWrite()` hands out the back picture, `width x height x 3` bytes with the top row first, and `endWrite()` swaps it to the front and bumps the generation.
With room for two pictures, effects read the front one undisturbed and never see half of one.
Short of memory, the service keeps one picture and locks it while it is written: frames may be skipped, never torn, and its status says why.
Effects read through `PictureService::Reader`, which holds the picture still for one gather.

**What a picture source brings:**

- its option and its row on the Picture service's spec card;
- a unit test that drives it on the desktop with a stand-in for the device;
- the platform seam for the device itself, with stubs on every other platform.

The thread-safety of the swap is already pinned by the service's own tests, a writer task racing a reader.

Sources are options of one module, so a source edits that file. When there are more than a handful, a source per file behind a small registry is the next step.

---

## A channel-data source

A channel-data source is an effect of its own, with its own settings, that owns a `ChannelDataSink` and writes its frames into it.
`src/light/effects/FseqPlayerEffect.h` is the template, and `NetworkReceiveEffect.h` the complete example.

```cpp
class MyFormatEffect : public EffectBase {
public:
    void prepare() override {
        sink_.prepare(/*senderMapped=*/true);   // wiring order: one value per physical light
        // open your file or socket, check its header, set a status for what you refuse
    }
    void tick() MM_NONBLOCKING override {
        // read this frame and hand it over: sink_.write(offset, data, length);
        sink_.tick();                            // the sink places a changed frame
    }
    bool deliversWiringOrder() const override { return sink_.wiringOrder(); }
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) override { sink_.copyWiringFrame(out, bytes, force); }
private:
    ChannelDataSink sink_{*this};
};
```

The sink does the rest: it holds the last frame so the lights keep it between frames, skips a frame that did not change, and places it.
A frame mapped by the receiver is copied into the layer, which applies this board's wiring.
A frame mapped by the sender, alone on its layer, goes to the output as it is, one copy on any layout.
Beside another effect or under a modifier it is placed on the grid by the layout's positions, so a modifier transforms it.
A source writing from a task of its own calls `write` there, which waits out at most one render copy; one that reads in `tick()` calls `write<false>`.

**What a channel-data source brings:**

- its effect, its settings and its spec card;
- unit tests that write frames and check each light against what was sent, with `moondeck/run/fseq_play.py` writing reference FSEQ files and streaming them over DDP.

The sink's placement on serpentine, 3D, mirrored and transposed layouts is pinned by Network Receive's tests.
