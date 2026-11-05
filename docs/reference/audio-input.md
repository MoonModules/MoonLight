# Audio input

Audio-reactive effects read the [Audio](../moonmodules/core/services.md#audio) service, which listens to a wired microphone or to what a computer plays. Wire the source, then check the squelch.

Steps: [Set up audio input](../moonwalk.md#audio-input).

## Microphone wiring

An `I2S` microphone has three wires: the bit clock, the word select and the data line. The INMP441 and most MEMS microphones are of this kind, as are line-in ADCs. Enter the three GPIOs as `sckPin`, `wsPin` and `sdPin`.

A `PDM` microphone has two wires, a clock and a data line, as the QuinLED Dig-Next-2's onboard microphone has. Set `micMode` to `PDM`: it uses `wsPin` as its clock and `sdPin` as its data, and hides the two clock pins it does not have.

An ADC or codec in front of the microphone needs `mclkPin`, and a codec is chosen with `codec`, its I2C address with `codecAddr` and its analog input gain with `codecGain`: 30 dB for a microphone, 0 dB for a line-in source. The [MHC-WLED ESP32-P4 shield](hardware/mhc-wled-esp32-p4-shield.md) wires its onboard PCM1808 line-in ADC this way.

On an ESP32 the Local source idles until real GPIOs are entered. On the desktop it captures the device picked in `device` right away.

## The squelch

The service learns the room's noise floor and opens its gate when the sound rises `squelch` dB above it, then holds the loudest band at the top of the display, so a quiet song and a loud one both fill it: the card's `dynamics` line shows the level, the floor, the window's top and whether the gate is open.

`squelch` is the one setting a room may need: raise it until a quiet room reads dark, lower it for a quiet source. The default of 10 dB suits a MEMS microphone.

## Capturing what the machine plays

Pick the loopback source in `device`. macOS has no native loopback, so it takes [BlackHole](https://existential.audio/blackhole/) behind a Multi-Output Device. Windows usually offers Stereo Mix, with VB-Cable as the fallback. PulseAudio and PipeWire list a Monitor of the output as a source. [Set up audio input](../moonwalk.md#audio-input) walks each one.

## Driving a fleet of devices

A desktop in Local mode with `send audio` on is an audio-sync source: one machine's microphone or loopback drives every device in Receive mode. Sending and receiving use the WLED multicast group, so WLED devices hear it too.

If a device on WiFi hears nothing, set `addressing` to `unicast` and list the devices in `hosts`.
