#pragma once

/// The radio chip's link as the update path needs it, split from platform_esp32.cpp, which owns the link's state, for platform_esp32_ota.cpp, which streams an image over it.

namespace mm::platform {

/// Whether the link to the radio chip carries calls now.
bool coprocessorLinkUp();
/// Start the link where it was not started yet, waiting until it is up or esp_hosted gives up, up to 20 s on a chip that does not answer: a task's call, since the render thread cannot wait that long. True once the link is up.
bool coprocessorStartLinkSync();
/// Whether the chip's firmware takes the activate call after an update, which ESP-Hosted 2.6.0 introduced; older firmware switches to the new image at the update's end by itself.
bool coprocessorActivateSupported();

}  // namespace mm::platform
