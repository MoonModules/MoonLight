# Adding a picture or channel-data source

A new way for frames to reach the lights, from a capture device, a file format or a network protocol.
Every source is one of two families, and each family has one place where its frames land.
So a source is small: it fetches frames and hands them over, and everything after that is already written and tested.

> This assumes the desktop build and tests run: see [Building, running, flashing](building.md). A source is proven on the desktop before a board sees it.

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

1. **Add the option.** Append it to the `Source` enum and its name to the `source` select, and say whether it runs in a task of its own in `kRunsInTask`.
2. **A still picture** draws once in `prepare()`, as the test pattern does: write into the picture `prepareBuffers` sized, and set the frame.
3. **A moving picture** comes from a task of its own, a capture or a receiver, so the render loop never waits on a device. Start the task in `prepare()` and stop it in `release()`. For each picture:

```cpp
uint8_t* px = beginWrite();          // the back picture, width x height x 3 bytes, top row first
decodeInto(px, width, height);       // your source: a decoder writes here directly, nothing is copied
endWrite();                          // swap it to the front and bump the generation
```

That is the whole contract.
With room for two pictures, `beginWrite` hands out the back one and `endWrite` swaps the pointers, so effects read the front picture undisturbed and never see half of one.
Short of memory, the service keeps one picture and locks it while you write: frames may be skipped, never torn, and its status says why.
Effects read through `PictureService::Reader`, which holds the picture still for one gather; you never touch them.

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
Beside another effect it is mapped back to the grid, and under a modifier it is placed by the layout's positions, so the modifier transforms it.
A source writing from a task of its own calls `write` there, which waits out at most one render copy; one that reads in `tick()` calls `write<false>`.

**What a channel-data source brings:**

- its effect, its settings and its spec card;
- unit tests that write frames and check each light against what was sent, with `moondeck/run/fseq_play.py` writing reference FSEQ files and streaming them over DDP.

The sink's placement on serpentine, 3D, mirrored and transposed layouts is pinned by Network Receive's tests.

---

## Before the pull request

One topic, based on main, about 400 changed lines and ten files at most, as [the process](../contributing/principles-and-process.md) describes.
The desktop tests pass, the source works on the board it is for, and a board without the device builds and shows a status rather than failing.
