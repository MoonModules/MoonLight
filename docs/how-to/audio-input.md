# Audio input

Audio-reactive effects read the [Audio](../moonmodules/core/services.md#audio) service, which listens to a wired microphone or to what a computer plays. Wire the source, then set the levels.

## Microphone wiring

An `I2S` microphone has three wires: the bit clock, the word select and the data line. The INMP441 and most MEMS microphones are of this kind, as are line-in ADCs. Enter the three GPIOs as `sckPin`, `wsPin` and `sdPin`.

A `PDM` microphone has two wires, a clock and a data line, as the QuinLED Dig-Next-2's onboard microphone has. Set `micMode` to `PDM`: it uses `wsPin` as its clock and `sdPin` as its data, and hides the two clock pins it does not have.

An ADC or codec in front of the microphone needs `mclkPin`, and a codec is chosen with `codec` and its I2C address with `codecAddr`. The [MHC-WLED ESP32-P4 shield](../reference/hardware/mhc-wled-esp32-p4-shield.md) wires its onboard PCM1808 line-in ADC this way.

On an ESP32 the Local source idles until real GPIOs are entered. On the desktop it captures the device picked in `device` right away.

## Setting the levels

`levels` picks who sets the display window. In `automatic`, the service maps each band's own floor and peak onto the window, so a treble band reads level with a bass one. In `manual`, the `gain` slider sets the window's width.

1. Set `floor`, the silence threshold, first. It depends on the part and the room.
2. Raise it until a quiet room reads still. A learner that studies an empty room maps its noise floor onto the whole display and shows silence at full scale.
3. In `manual`, set `gain`. The default suits a quiet MEMS microphone, so turn it down hard for a loopback device or everything clips.

## Capturing what the machine plays

Pick the loopback source in `device`. How to get one depends on the operating system.

### macOS

macOS has no native loopback.

1. Install [BlackHole](https://existential.audio/blackhole/).
2. In Audio MIDI Setup, create a **Multi-Output Device** with your speakers first and BlackHole second, with drift correction on BlackHole.
3. Set it as the system output. The speakers keep playing while an identical copy lands in BlackHole, which `device` captures.

The Mac's volume keys go dead on a multi-output device, so set the volume in the player.

### Windows

Windows usually needs nothing: enable **Stereo Mix** in the Recording tab and pick it in `device`. Where a driver lacks it, VB-Cable is the fallback.

### Linux

PulseAudio and PipeWire expose a **Monitor of** the output as a source. Pick it in `device`.

## Driving a fleet of devices

A desktop in Local mode with `send audio` on is an audio-sync source: one machine's microphone or loopback drives every device in Receive mode. Sending and receiving use the WLED multicast group, so WLED devices hear it too.

If a device on WiFi hears nothing, set `addressing` to `unicast` and list the devices in `hosts`.
