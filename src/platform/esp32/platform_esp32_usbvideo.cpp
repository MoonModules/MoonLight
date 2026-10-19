/// USB video capture on the ESP32: the stubs every image without `CONFIG_MM_USB_VIDEO` links, which report no device.

#include "platform/platform.h"

namespace mm::platform {

#if !defined(CONFIG_MM_USB_VIDEO)

// An image built without the capture reports no device, which the Picture service shows as a status.
uint32_t videoCaptureFormatGeneration() MM_NONBLOCKING { return 0; }
bool videoCaptureInit(VideoCaptureHandle&, uint16_t, uint16_t, uint8_t) { return false; }
bool videoCaptureAwaitFrame(VideoCaptureHandle&, uint32_t) { return false; }
bool videoCaptureDecodeInto(VideoCaptureHandle&, uint8_t*, uint16_t, uint16_t, uint16_t&, uint16_t&) { return false; }
void videoCaptureDeinit(VideoCaptureHandle&) {}
VideoCaptureStats videoCaptureStats() MM_NONBLOCKING { return VideoCaptureStats{}; }

#endif

}  // namespace mm::platform
