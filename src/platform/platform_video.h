#pragma once
/// The platform's video: NDI, HDMI, the encoder behind HLS and RTSP, and USB video capture.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::edid { struct Monitor; }   // what the HDMI seam reports about the monitor; edid.h defines it for the code that reads it

namespace mm::platform {


// --- NDI output, gated by `hasNdi`: the user's proprietary runtime, resolved on demand ---------

/// Is the NDI runtime present and loaded? Loads on first call.
bool ndiAvailable();

/// Create a named NDI source, replacing any sender already open.
bool ndiSenderOpen(const char* name);

/// Destroy the sender; safe with none open, so a driver's release need not track state.
void ndiSenderClose();

/// Send one frame of tightly-packed RGB, w*h*3 bytes, declaring `fps` to the receiver.
bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls

// A host seam recording what the driver sent, pinning geometry, packing and pacing with no runtime installed.

/// Force the runtime's apparent presence, overriding what is installed.
enum class NdiTestMode : uint8_t { Off, ForceAvailable, ForceMissing };
/// Apply a test mode.
void setTestNdiMode(NdiTestMode mode);
/// Force availability on or off, in the older two-state form.
inline void setTestNdiAvailable(bool available) {
    setTestNdiMode(available ? NdiTestMode::ForceAvailable : NdiTestMode::Off);
}
/// How many frames were recorded.
size_t ndiTestFrameCount();
/// The recorded frame's width.
uint16_t ndiTestFrameWidth(size_t i);
/// The recorded frame's height.
uint16_t ndiTestFrameHeight(size_t i);
/// The rate declared with the recorded frame.
uint8_t  ndiTestFrameFps(size_t i);
/// The recorded frame's tight-RGB bytes, as the driver handed them over.
const uint8_t* ndiTestFrameData(size_t i);
/// The name the sender was opened with, pinning the blank-means-device-name rule.
const char* ndiTestSenderName();
/// Drop every recorded frame.
void ndiTestClearFrames();
#endif

// --- HDMI out, gated by `hasHdmi`: a display mode, a frame, and hot-plug ------------------------

/// One display mode the HDMI output runs; all zero is the display's own mode, which a computer offers so the display stays as it is.
struct HdmiMode {
    uint16_t width;    ///< active pixels across
    uint16_t height;   ///< active lines
    uint8_t  hz;       ///< refresh rate
};

/// The modes this platform offers, in the order the driver's `mode` control lists them; 0 where it has none.
size_t hdmiModes(const HdmiMode** out) MM_NONBLOCKING;

/// Start the output in mode `index`, replacing a running one; false where the platform or board has none, such as no I2C bus open for the bridge. `ownScreen` lets a computer with one display show on it, where `hasHdmiOwnScreen`.
bool hdmiOpen(uint8_t index, bool ownScreen);

/// Stop the output and free its framebuffer; safe when closed.
void hdmiClose();

/// Hand one tight-RGB frame to the output, which scales it to the screen, keeping its aspect when `fit`. Returns at once: a frame arriving before the previous one is shown replaces it.
bool hdmiWrite(const uint8_t* rgb, uint16_t w, uint16_t h, bool fit) MM_NONBLOCKING;

/// What the link reports back: Down while the output is not set up and is retried (an ESP32's bridge absent or failing, a Linux screen held by a host), then the monitor on its hot-plug detect.
enum class HdmiLink : uint8_t { Down, NoMonitor, Monitor };

/// The link's state now; NoMonitor until the first poll after opening.
HdmiLink hdmiLink() MM_NONBLOCKING;

/// The mode the display runs now: the one opened, or the display's own where it has no such size; false while no display is up.
bool hdmiActiveMode(HdmiMode& out) MM_NONBLOCKING;

/// The connected monitor's EDID, reduced to this platform's modes in `hdmiModes` order; false until one is read after the monitor connected, or where the monitor gives none.
bool hdmiMonitor(edid::Monitor& out) MM_NONBLOCKING;

/// Whether Esc was pressed on the host's own screen while the output covered it, once per press: the driver gives the desktop back.
bool hdmiTakeDismiss() MM_NONBLOCKING;

/// The bridge's own view of the stream it sends.
struct HdmiBridgeState {
    uint16_t dds;       ///< its clock synthesizer's read-back, the word's two middle bytes that settle
    uint16_t ddsWant;   ///< the same bytes of the word the mode's pixel clock programs
    uint16_t hsync;     ///< the horizontal sync it measures on its input
    uint16_t vsync;     ///< the vertical sync it measures on its input
};

/// The bridge's state as last read; false while the link is down or before the first read.
bool hdmiBridge(HdmiBridgeState& out) MM_NONBLOCKING;
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls

/// Report a link state: the host's stand-in records what the driver handed over, as the NDI seam does, so the driver is tested with no bridge.
void setTestHdmiLink(HdmiLink link);
/// Report a monitor's EDID as read.
void setTestHdmiMonitor(const edid::Monitor& monitor);
/// Report the bridge's state as read.
void setTestHdmiBridge(const HdmiBridgeState& state);
/// Report the mode the display runs, as one lacking the opened size keeps its own.
void setTestHdmiActiveMode(const HdmiMode& mode);
/// The mode the output was opened in, -1 while closed.
int hdmiTestOpenMode();
/// Whether the output was opened with the host's own screen allowed.
bool hdmiTestOwnScreen();
/// Report Esc pressed on the host's own screen.
void setTestHdmiDismiss();
/// How many frames were handed over since the last clear.
size_t hdmiTestFrameCount();
/// The last frame's width.
uint16_t hdmiTestFrameWidth();
/// The last frame's height.
uint16_t hdmiTestFrameHeight();
/// Whether the last frame asked to keep its aspect.
bool hdmiTestFrameFit();
/// The last frame's tight-RGB bytes.
const uint8_t* hdmiTestFrameData();
/// Drop the recorded frames and close the output.
void hdmiTestClear();
#endif

// --- HLS output, gated by `hasHls`: the seam carries numbers rather than an encoder argv -------

/// What to encode; geometry and rate are the frame contract.
struct EncoderConfig {
    uint16_t    width;         ///< frame width
    uint16_t    height;        ///< frame height
    uint8_t     fps;           ///< also the GOP, since a cut needs a keyframe and a long GOP lengthens every segment
    uint16_t    bitrateKbit;   ///< target bitrate
    const char* encoderName;   ///< a host ffmpeg encoder; ignored where the platform has only one
    const char* outDir;        ///< absolute directory for the playlist and segments
};

/// The frames every platform's encoder takes, the ESP32's H.264 block being the strictest (esp_h264_types.h): 80x80 to 1920x2032.
constexpr uint16_t kEncoderMinSide = 80, kEncoderMaxWidth = 1920, kEncoderMaxHeight = 2032;

/// Start encoding to `cfg`, replacing any encoder already running.
bool encoderStart(const EncoderConfig& cfg);

/// Hand one whole frame to the encoder, which returns to the caller at once.
int encoderWrite(const uint8_t* data, size_t len);

/// Is the spawned encoder still alive? Reaps the child when it exited.
bool encoderRunning();

/// Stop the encoder, letting it finalize the playlist first; safe with none running.
void encoderStop();

/// The one claim slot, since there is one encoder. Shared by every caller through the accessors below.
inline const void*& encoderOwnerSlot() {
    static const void* owner = nullptr;
    return owner;
}

/// Which module holds the encoder, null where none does. For a status line only.
inline const void* encoderOwner() { return encoderOwnerSlot(); }

/// Claim the one encoder for `owner`, false where another module already holds it.
inline bool encoderClaim(const void* owner) {
    if (!owner) return false;
    const void* held = encoderOwnerSlot();
    if (held && held != owner) return false;   // another module is streaming: refused, not stolen
    encoderOwnerSlot() = owner;
    return true;
}

/// Release the claim where `owner` holds it, and stop the encoder; a non-holder is ignored.
inline void encoderRelease(const void* owner) {
    if (!owner || encoderOwnerSlot() != owner) return;   // only the owner releases its claim
    encoderOwnerSlot() = nullptr;
    encoderStop();
}

/// Serve an HLS file the platform holds in RAM; false where this platform writes segments to disk.
bool hlsSegment(const char* name, const uint8_t** data, size_t* len);
/// Release what hlsSegment handed out, required after every call that answered true.
void hlsSegmentRelease();

// --- The encoder has one owner at a time ------------------------------------------------------
// One instance per target, so a second encoderStart would silently reconfigure the first driver's stream, and the claim below makes that visible. Unsynchronised: both callers claim from prepare(), on the render thread.

// --- RTSP output, gated by `hasRtsp`: the encoded frame itself, before any muxing ---------------

/// One encoded frame as the encoder produced it, valid until the encoder writes the next.
struct EncodedFrame {
    const uint8_t* nal;       ///< the frame's NAL units, Annex B, start codes included
    size_t         len;       ///< bytes at `nal`
    uint32_t       pts90;     ///< presentation time in the RTP clock's 90 kHz units
    bool           keyframe;  ///< an IDR, which a joining client decodes from
};

/// The frame the encoder produced since the last take, valid until the next `encoderWrite`.
bool rtspTakeFrame(EncodedFrame* out);

/// Release what `rtspTakeFrame` handed out, required after every call that answered true: the frame stays valid until then, and the encoder reuses the buffer after.
void rtspReleaseFrame();

/// The encoder's parameter sets where it emits them separately, false where each keyframe carries its own.
bool rtspParameterSets(EncodedFrame* sps, EncodedFrame* pps);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls

/// Record instead of encoding, or force the not-installed path, since CI has no ffmpeg.
enum class EncoderTestMode : uint8_t { Off, Record, ForceMissing };
/// Apply a test mode.
void setTestEncoderMode(EncoderTestMode mode);
/// Force encoderWrite's next result in Record mode, 0 being would-block and -1 dead.
void setTestEncoderWriteResult(int result);
/// How many frames were recorded.
size_t encoderTestFrameCount();
/// One recorded frame's size.
size_t encoderTestFrameSize(size_t i);
/// One recorded frame's bytes.
const uint8_t* encoderTestFrameData(size_t i);
/// The argv encoderStart was called with, joined by spaces, for pinning the arg builder.
const char* encoderTestArgs();
/// Drop every recorded frame.
void encoderTestClearFrames();
#endif

// --- USB video capture, gated by `hasUsbVideo`: an HDMI grabber presenting itself as a webcam ----

/// Opaque per-device state, owned by the platform between init and deinit.
struct VideoCaptureHandle { void* impl = nullptr; };

/// One mode a grabber advertises, so a source offers what the device has rather than guesses.
struct VideoCaptureFormat {
    uint16_t width = 0;    ///< frame width in pixels
    uint16_t height = 0;   ///< frame height in pixels
    uint8_t  fps = 0;      ///< frames a second offered at that size
};

/// Moves whenever that list is rewritten, a device enumerating or leaving, and each value it takes is new; 0 while no device is listed.
uint32_t videoCaptureFormatGeneration() MM_NONBLOCKING;

/// Claim the first grabber and stream MJPEG near this mode; false when none is attached or none fits.
bool videoCaptureInit(VideoCaptureHandle& h, uint16_t width, uint16_t height, uint8_t fps);

/// Wait up to `timeoutMs` for the next frame; true once one is ready to decode.
bool videoCaptureAwaitFrame(VideoCaptureHandle& h, uint32_t timeoutMs);

/// Decode the frame videoCaptureAwaitFrame announced into `dst`, no larger than `maxWidth` x `maxHeight` x 3 bytes, counting a source byte at or below `floor` as black.
bool videoCaptureDecodeInto(VideoCaptureHandle& h, uint8_t* dst, uint16_t maxWidth, uint16_t maxHeight,
                            uint16_t& outWidth, uint16_t& outHeight, uint8_t floor);

/// Which step the last open failed at, or null where none has failed since; a static string the caller may show.
const char* videoCaptureFault() MM_NONBLOCKING;

/// Release the device and free its buffers.
void videoCaptureDeinit(VideoCaptureHandle& h);

/// Start uninstalling what the capture installed to see devices, once no capture is open. It returns before the teardown finishes, so an init is refused while videoCaptureSettling() holds.
void videoCaptureShutdown();

/// Whether a teardown is still unwinding, so an open would be refused rather than failing for a reason worth naming.
bool videoCaptureSettling() MM_NONBLOCKING;

/// Why frames did not reach the lights, cumulative since the device was opened.
struct VideoCaptureStats {
    uint32_t decoded = 0;   ///< frames decoded into a picture
    uint32_t dropped = 0;   ///< frames that arrived while the one before was still being decoded
    uint32_t failed = 0;    ///< frames the decoder refused, a mis-read payload stride among them
    VideoCaptureFormat stream;   ///< the mode the device streams, 0 wide until it streams
};
/// The counters above, read atomically enough for a once-a-second status line.
VideoCaptureStats videoCaptureStats() MM_NONBLOCKING;

/// Attach a stand-in grabber advertising these modes, so a capture source is host-testable; 0 detaches.
void setTestVideoCaptureFormats(const VideoCaptureFormat* formats, size_t count);
/// Make the stand-in decode every pixel this color, so a test tells its picture from the test card.
void setTestVideoCaptureColor(uint8_t r, uint8_t g, uint8_t b);
/// Make the stand-in refuse to decode, as a bad bitstream does; false lets it decode again.
void setTestVideoCaptureDecodeFails(bool fail);

}  // namespace mm::platform
