# Audio input

Audio-reactive effects read the [Audio](../moonmodules/core/services.md#audio) service, which listens to a wired microphone or to what a computer plays. Wire the source, then set the levels.

Steps: [Set up audio input](../moonwalk.md#audio-input).

## Microphone wiring

An `I2S` microphone has three wires: the bit clock, the word select and the data line. The INMP441 and most MEMS microphones are of this kind, as are line-in ADCs. Enter the three GPIOs as `sckPin`, `wsPin` and `sdPin`.

A `PDM` microphone has two wires, a clock and a data line, as the QuinLED Dig-Next-2's onboard microphone has. Set `micMode` to `PDM`: it uses `wsPin` as its clock and `sdPin` as its data, and hides the two clock pins it does not have.

An ADC or codec in front of the microphone needs `mclkPin`, and a codec is chosen with `codec` and its I2C address with `codecAddr`. The [MHC-WLED ESP32-P4 shield](hardware/mhc-wled-esp32-p4-shield.md) wires its onboard PCM1808 line-in ADC this way.

On an ESP32 the Local source idles until real GPIOs are entered. On the desktop it captures the device picked in `device` right away.

## Setting the levels

`levels` picks who sets the display window. In `automatic`, the service maps each band's own floor and peak onto the window, so a treble band reads level with a bass one. In `manual`, the `gain` slider sets the window's width.

`floor` is the silence threshold, and it depends on the part and the room. A learner that studies an empty room maps its noise floor onto the whole display and shows silence at full scale. The default `gain` suits a quiet MEMS microphone, so a loopback device needs it turned down hard or everything clips.

## Capturing what the machine plays

Pick the loopback source in `device`. macOS has no native loopback, so it takes [BlackHole](https://existential.audio/blackhole/) behind a Multi-Output Device. Windows usually offers Stereo Mix, with VB-Cable as the fallback. PulseAudio and PipeWire list a Monitor of the output as a source. [Set up audio input](../moonwalk.md#audio-input) walks each one.

## Driving a fleet of devices

A desktop in Local mode with `send audio` on is an audio-sync source: one machine's microphone or loopback drives every device in Receive mode. Sending and receiving use the WLED multicast group, so WLED devices hear it too.

If a device on WiFi hears nothing, set `addressing` to `unicast` and list the devices in `hosts`.
