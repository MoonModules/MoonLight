# Drivers module

<img width="320" src="https://github.com/user-attachments/assets/587bfb07-2da1-41fe-978e-43e431fd9517" />

## Overview

The Drivers module defines layers and drivers.

* Layout 🚥: A layout (🚥) defines the positions of the lights to control. See [Layouts](https://moonmodules.org/MoonLight/moonlight/layouts/)
* Driver ☸️: A driver is a link between MoonLight to hardware or the network. Drivers can both input data or output data. Examples:
    * LED drivers (FastLED, Physical and Virtual drivers, hub75 panels)
    * Light driver (Art-Net / DMX)
    * Audio driver
    * Sensor drivers (microphone, gyro, MIDI controller)

## Controls

* Nodes: a list of Layouts and Drivers
    * Nodes can be added (+), deleted (🗑️) or edited (✎) or reordered (drag and drop). The node to edit will be shown below the list, press save (💾) if you want to preserve the change when the device is restarted
    * Reorder: Nodes can be reordered, defining the order of execution
        * Layouts: Need to be before drivers, multiple layouts can be added
        * Drivers: After Layouts, choose one LEDs driver and optionally add Art-Net and Audio Sync, reordering might need a restart.
    * Controls. A node can be switched on and off and has custom controls, which defines the behaviour of the node 
    * See below for a list of existing Layouts and Drivers

## Driver ☸️ nodes

Below is a list of Drivers in MoonLight. 
Want to add a Driver to MoonLight, see [develop](https://moonmodules.org/MoonLight/develop/overview/). See also [Live scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) to add runtime drivers.

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Physical LED Driver | <img width="100" src="https://github.com/user-attachments/assets/9cbe487e-f330-40a5-8b40-6663c83e5d90"/> | <img width="320" alt="Physical" src="https://github.com/user-attachments/assets/0c6f1543-623a-45bf-98d7-f5ddd072a1c6" /> | Drive multiple LED types, all devices including ESP32-P4(-nano) supported<br>Max Power and Light preset: See below<br>DMA buffer: set higher when LEDs flicker |
| FastLED Driver | <img width="100" src="https://avatars.githubusercontent.com/u/5899270?s=48&v=4"/> | <img width="320" alt="FastLed" src="https://github.com/user-attachments/assets/d5ea1510-9766-4687-895a-b68c82575b8f" /> | Most used LED driver. Drive most common LEDs (WS2812).<br>Max Power: See below |
| Art-Net | <img width="100" src="https://github.com/user-attachments/assets/9c65921c-64e9-4558-b6ef-aed2a163fd88"> | <img width="320" alt="Art-Net" src="https://github.com/user-attachments/assets/1428e990-daf7-43ba-9e50-667d51b456eb" /> | Drive LEDS and DMX lights over the network. See below |
| AudioSync | <img width="100" src="https://github.com/user-attachments/assets/bfedf80b-6596-41e7-a563-ba7dd58cc476"/> | No controls | Listens to audio sent over the local network by WLED-AC or WLED-MM and allows audio reactive effects (♪ & ♫) to use audio data (volume and bands (FFT)) |
| Virtual LED Driver | <img width="100" src="https://github.com/user-attachments/assets/98fb5010-7192-44db-a5c9-09602681ee15"/> | <img width="100" src="https://github.com/user-attachments/assets/c81d2f56-00d1-4424-a716-8e3c30e76636"/> | Driving max 120! outputs. E.g. 48 panels of 256 LEDs each run at 50-100 FPS using shift registers<br>Not implemented yet |
| HUB75 Driver | <img width="100" src="https://github.com/user-attachments/assets/620f7c41-8078-4024-b2a0-39a7424f9678"/> | <img width="100" src="https://github.com/user-attachments/assets/4d386045-9526-4a5a-aa31-638058b31f32"/> | Drive HUB75 panels<br>Not implemented yet |
| IR Driver 🆕🚧 | <img width="100" src="../../media/moonlight/IRDriver.jpeg"/> |  | Receive IR commands and [Lights Control](https://moonmodules.org/MoonLight/moonlight/lightscontrol/) |

### Max Power and Light Preset

* **Max Power**: max amount of power in watts to send to LEDs. Default 10: 5V * 2A = 10W (so it runs fine on USB)

* **Light preset**: Defines the channels per light and color order

    <img width="183" alt="Light preset" src="https://github.com/user-attachments/assets/532265d5-b375-425c-8e29-cf8fa60b4f2c" />

!!! warning "Same Light preset"
    Currently, if using multiple drivers, all drivers need the same Light preset !!

* RGB to BGR: 3 lights per channel, RGB lights, GRB is default
* GRBW: LEDs with white channel like SK6812.
* RGBW: for Par/DMX Lights
* GRB6: for LED curtains with 6 channels per light (only rgb used)
* RGBWYP: Compatible with [DMX 18x12W LED RGBW/RGBWUAV](https://s.click.aliexpress.com/e/_EJQoRlM) (RGBW is 4x18=72 channels, RGBWUAV is 6x18=104 channels). Currently setup to have the first 36 lights 4 channel RGBW, after that 6 channel RGBWYP ! Used for 18 channel light bars
* MH*: Moving Heads lights
    * MHBeTopper19x15W-32: [BeTopper / Big Dipper](https://betopperdj.com/products/betopper-19x15w-rgbw-with-light-strip-effect-moving-head-light)
    * MHBeeEyes150W-15: [Bee eyes](https://a.co/d/bkTY4DX)
    * MH19x15W-24: [19x15W Zoom Wash Lights RGBW Beam Moving Head](https://s.click.aliexpress.com/e/_EwBfFYw)

!!! info "Custom setup"
    These are predefined presets. In a future release custom presets will be possible.

### Art-Net ☸️

Sends Lights in Art-Net compatible packages to an Art-Net controller specified by the IP address provided.

#### Controls

* **Controller IP**: The last segment of the IP address within your local network, of the the hardware Art-Net controller.
* **Port**: The network port added to the IP address, 6454 is the default for Art-Net.
* **FPS Limiter**: set the max frames per second Art-Net packages are send out (also all the other nodes will run at this speed).
    * Art-Net specs recommend about 44 FPS but higher framerates will work mostly (up to until ~130FPS tested)
* **Nr of outputs**: Art-Net LED controllers can have more then 1 output (e.g. 12)
* **Universes per output**: How many universes can each output handle. This determines the maximum number of lights an output can drive (nr of universe x nr of channels per universe / channels per light)

!!! tip "Set universes"
    Set the number of universes also on the controller!

* **Channels per ouput**: each output can drive a maximum number of channels, determined by the universes per output

!!! warning "DMX start with 1"
    Dmx channels count from 1 to 512. At the moment we start counting from 0..511.

#### Recommended Art-Net controllers

The following devices have been tested and are recommended:

[PKNight ArtNet2-CR021R](https://s.click.aliexpress.com/e/_ExRrKe4)

<img width="200" src="https://github.com/user-attachments/assets/e3d605b6-a023-4abb-b604-77b44267b1a3">

* Driving DMX fixtures: Used to drive the Light Presets for DMX lights / moving heads (see below)
* **Nr of outputs**: max 2 outputs
* **Universes per output**: 1 universe
* **Channels per ouput**:  512 channels

[Club Lights 12 Pro Artnet Controller - CL12P](https://s.click.aliexpress.com/e/_Ex9uaOk)

<img width="200" src="https://github.com/user-attachments/assets/9c65921c-64e9-4558-b6ef-aed2a163fd88">

* Driving LEDs: : 12 output leds controller, max 8 universes per channel. Max 512 channels per universe. Select IC-Type UCS2903

* **Nr of outputs**: Max 12 outputs
* **Universes per output**: Max 8 universes
* **Channels per ouput**: max 8 * 512. 

Each color in a LED is one channel: For RGB max 170 LEDs is 510 channels per universe, for RGBW max 128 LEDs per universe is 512 channels per universe => max 1360 RGB LEDs and 1024 RGBW LEDs per output. 

!!! tip "channels per universe"
    Set channels per universe to 510 for RGB and 512 for RGBW (no proof yet it makes a difference ...) on the controller. 

The real number of channels per output can be less then the amount of universes available. e.g. if each output drives one 256 LED RGB panel, channels per output is 768. One package (= one universe) sends 170 LEDs (510 channels) and the second 86 LEDs / 256 channels. The next package for the next panel on the next output will then be in the first universe for that output (so unused universes for a channel will be skipped)
