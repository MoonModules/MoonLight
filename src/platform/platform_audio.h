#pragma once
/// The platform's audio: the codec, the microphone and capture from a computer's audio input.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


// I2S audio input: one seam, the read, with everything after it domain code.

/// Which codec sits in front of the microphone, none for a microphone that speaks I2S itself.
enum class CodecType : uint8_t { None = 0, Es8311 = 1, Es7210 = 2 };

/// Configure the audio codec at this 7-bit address with this analog input gain, where the board has one; true when the board has no codec to configure. It is a device on the I2C bus (i2cBusOpen), and its master clock is the microphone's `mclkPin`.
bool audioCodecInit(CodecType type, uint8_t i2cAddr, uint32_t sampleRate, uint8_t gainDb);

/// Release the codec.
void audioCodecDeinit();

/// One live audio input: an I2S channel on a board, an OS capture device on host.
struct AudioMicHandle { void* impl = nullptr; };

/// Whether this target has any live audio, which is what makes the analysis path run.
constexpr bool hasAudioInput = hasI2sMic || hasAudioCapture;

/// The OS capture devices, entry 0 being the default; 0 where the target has none.
size_t audioCaptureDevices(const char* const** optionsOut);
/// The capture backend the host holds, as its OS names it (PulseAudio, ALSA, Core Audio, WASAPI, or Null where the libraries are missing), "none" before any list is read; a board names its own input.
const char* audioCaptureBackend() MM_NONBLOCKING;

/// Open one capture device as mono samples at `sampleRate`, which the backend resamples to.
bool audioCaptureInit(AudioMicHandle& h, uint8_t deviceIndex, uint32_t sampleRate);

/// How the microphone speaks, which decides its pin count and how the peripheral is configured.
enum class MicMode : uint8_t { I2sStd = 0, Pdm = 1 };

/// Bring up an I2S channel reading the mic on these pins, as 24-bit mono samples.
bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate,
                  MicMode mode = MicMode::I2sStd);

/// Read up to `maxSamples` samples, answering the count: at once with `waitMs` 0, as the render tick reads, or waiting up to `waitMs` for them, as an audio task reads.
size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples, uint32_t waitMs = 0);
/// Whether the I2S instance a PDM microphone needs is free right now.
bool audioMicSharedBusFree(MicMode mode);

/// Release the input.
void audioMicDeinit(AudioMicHandle& h);

}  // namespace mm::platform
