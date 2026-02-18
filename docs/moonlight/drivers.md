# Drivers module

<img width="320" src="https://github.com/user-attachments/assets/587bfb07-2da1-41fe-978e-43e431fd9517" />

## Overview

The Drivers module defines layers and drivers.

* Layout 🚥: A layout (🚥) defines the positions of the lights to control. See [Layouts](../../moonlight/layouts)
* Driver ☸️: A driver is a link between MoonLight to hardware or the network. Drivers can both input data or output data. Examples:
    * LED drivers (FastLED, Parallel LED Drivers, hub75 panels)
    * Light driver (Art-Net / DMX)
    * Audio driver
    * Sensor drivers (microphone, gyro, MIDI controller)

Layouts need to be defined before drivers as the driver takes the layouts defined before itself, e.g. to define which LEDs to drive on which pins.

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
Want to add a Driver to MoonLight, see [develop](../../develop/overview/). See also [Live scripts](../../moonlight/livescripts/) to add runtime drivers.

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Parallel LED Driver | <img width="100" src="https://github.com/user-attachments/assets/9cbe487e-f330-40a5-8b40-6663c83e5d90"/> | <img width="320" alt="Parallel" src="https://github.com/user-attachments/assets/0c6f1543-623a-45bf-98d7-f5ddd072a1c6" /> | Drive multiple LED types, all devices including ESP32-P4(-nano) supported<br>Light preset: See below<br>DMA buffer: set higher when LEDs flicker<br>See [below](#parallel-led-driver) |
| FastLED Driver | <img width="100" src="https://avatars.githubusercontent.com/u/5899270?s=48&v=4"/> | <img width="320" alt="FastLed" src="https://github.com/user-attachments/assets/d5ea1510-9766-4687-895a-b68c82575b8f" /> | Most used LED driver. Drive most common LEDs (WS2812). |
| Art-Net Out| <img width="100" src="https://github.com/user-attachments/assets/9c65921c-64e9-4558-b6ef-aed2a163fd88"> | <img width="320" src="../../media/moonlight/drivers/ArtNetOutControls.png" /> | Send Art-Net to Drive LEDS and DMX lights over the network. See [below](#art-net-out) |
| Art-Net In | <img width="100" src="../../media/moonlight/drivers/Art-Net-In.png"> | <img width="320" src="../../media/moonlight/drivers/ArtNetInControls.png" /> | Receive Art-Net (or DDP) packages e.g. from [Resolume](https://resolume.com/) or Touch Designer. See [below](#art-net-in) |
| Audio Sync | <img width="100" src="https://github.com/user-attachments/assets/bfedf80b-6596-41e7-a563-ba7dd58cc476"/> | No controls | Listens to audio sent over the local network by WLED-AC or WLED-MM and allows audio reactive effects (♪ & ♫) to use audio data (volume and bands (FFT)) |
| HUB75 Driver | <img width="100" src="https://github.com/user-attachments/assets/620f7c41-8078-4024-b2a0-39a7424f9678"/> | <img width="100" src="https://github.com/user-attachments/assets/4d386045-9526-4a5a-aa31-638058b31f32"/> | Drive HUB75 panels<br>Not implemented yet |
| IR Driver | <img width="100" src="../../media/moonlight/drivers/IRDriver.jpeg"/> | <img width="100" src="../../media/moonlight/drivers/irdrivercontrols.png"/> | Receive IR commands and [Lights Control](../../moonlight/lightscontrol/) |
| IMU Driver | <img width="100" src="../../media/moonlight/drivers/MPU-6050.jpg"/> | <img width="100" src="../../media/moonlight/drivers/IMUDriverControls.png"/> | Receive inertial data from an IMU / I2C peripheral, see [IO](../../moonbase/inputoutput/#i2c-peripherals)<br>Used in [particles effect](../../moonlight/effects/#moonlight-effects) |

### Light Preset

* **Max Power**: moved to [IO Module](../../moonbase/inputoutput) board presets.

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

### Parallel LED Driver

<img width="320" alt="Parallel" src="https://github.com/user-attachments/assets/0c6f1543-623a-45bf-98d7-f5ddd072a1c6" />

* The Parallel LED driver uses different hardware peripherals depending on the MCU type: ESP32-D0: I2S, ESP32-S3: LCD_CAM, ESP32-P4: Parallel IO (ParLIO).
* For ESP32-D0 and ESP32-S3 the [I2S clockless driver](https://github.com/hpwit/I2SClocklessLedDriver) is used
* For ESP32-P4 the [parlio driver](https://github.com/troyhacks/WLED) is used.
* Virtual LED Driver will be part of the Parallel LED driver: Driving max 120! outputs (E.g. 48 panels of 256 LEDs each run at 50-100 FPS) using shift registers. Integrated within the Parallel LED Driver architecture. Not implemented yet
  
    <img width="100" src="https://github.com/user-attachments/assets/98fb5010-7192-44db-a5c9-09602681ee15"/><img width="100" src="https://github.com/user-attachments/assets/c81d2f56-00d1-4424-a716-8e3c30e76636"/>

**Parlio (ESP32-P4)**

*Created by @TroyHacks, extended by @ewowi*

The ESP32-P4 Parallel LED Driver uses the hardware PARLIO peripheral to control up to **16 LED strips simultaneously** with independent pixel counts per strip. This enables high-performance setups with thousands of LEDs while maintaining accurate timing through DMA-based transmission.

**Key Features:**

- **Variable LEDs per strip**: Each GPIO pin can drive a different number of WS2812/SK6812 LEDs (e.g., Pin 0: 100 LEDs, Pin 1: 50 LEDs, Pin 2: 120 LEDs)
- **Automatic padding**: Shorter strips receive black pixels to maintain timing alignment—no visual impact
- **Memory efficient**: Only stores actual LED data, padding happens during hardware transmission
- **High-speed operation**: Supports 800 kHz to 1.2 MHz clock speeds with auto-overclocking for smaller LED counts
- **RGB/RGBW support**: Configurable color ordering and per-component brightness correction
- **Configuration**: Assign GPIO pins in the MoonLight interface and specify LED counts per pin. The driver automatically calculates the maximum LEDs per pin and handles synchronization.

### Art-Net Out ☸️

<img width="300" src="../../media/moonlight/drivers/ArtNetOutControls.png"/>

Sends Lights in Art-Net compatible packages to an Art-Net controller specified by the IP address(es) provided.

**Controls**

* **Light preset**: See above.
* **Controller IPs**: The last segment of the IP address within your local network, of the hardware Art-Net controller. Add more IPs if you send to more than one controller, comma separated.
* **Port**: The network port added to the IP address, 6454 is the default for Art-Net.
* **FPS Limiter**: set the max frames per second Art-Net packages are send out (also all the other nodes will run at this speed).
    * Art-Net specs recommend about 44 FPS but higher framerates will work mostly (up to until ~130FPS tested)
* **Universe size**: How many channels per universe. 510 and 512 most common. Make sure it corresponds with the Art-Net receiver used.
* **Used channels**: Calculated! Shows how many channels are used (e.g. in a universe of 512 only 170 RGB LEDs fits which is 510 channels, so 510 of the 512 channels are used).
* **Nr of Outputs per IP**: Art-Net LED controllers can have more than 1 output (e.g. 12) If all outputs are sent, Art-Net will be sent to the next IP number.
* **Universes per output**: How many universes can each output handle. This determines the maximum number of lights an output can drive (nr of universe x nr of channels per universe / channels per light)
* **Total universes**: Calculated! Based on the nr of lights (specified by the [layout](../../moonlight/layouts/)), how many universes needs to be configured to sent all lights out.
* **Channels per output**: How many channels will be sent to each output
* **Total channels**: Calculated! Based on the nr of lights (specified by the [layout](../../moonlight/layouts/)), how many channels should be send to all outputs together to sent all lights out

!!! tip "Controller settings"
    Set the number of universes and channels per universe also on the controller!

* **Channels per output**: each output can drive a maximum number of channels, determined by the universes per output

!!! warning "DMX start with 1"
    Dmx channels count from 1 to 512. At the moment MoonLight counts from 0..511 which translates to 1..512.

### Art-Net In ☸️

<img width="300" src="../../media/moonlight/drivers/ArtNetInControls.png"/>

Receives Art-Net data from the network to setup a MoonLight device as an Art-Net receiver. Can receive Art-Net from other MoonLight devices (see Art-Net out above) and other tools like Resolume, XLights, TouchDesigner, Chataigne etc.

* DDP: If unchecked, processes data in Art-Net format, if checked, process data in DDP format
* Port: The port listening for Art-Net. When using DDP, change to 4048 (the default port for DDP).
* Universe Min-Max: Filters Universes (Art-Net only).
* View: 
    * Select physical layer to directly store the received channels into the physical layer
    * Select one of the (virtual layers) to take mapping into account (using layout specification and modifiers specified (recommended), see [Modifiers](../../moonlight/modifiers/), part of the [Effects Module](../../moonlight/effects/))

!!! tip "Other setup"
    * Add a Layout driver to specifify the fixture you are displaying on, e.g. Single Line for Tubes or Panel for Matrices
    * Add the Parallel LED Driver to enable connected LEDs
    * Go to the [IO Module](../../moonbase/inputoutput) to define a board preset.

!!! tip "Running effects and Art-Net In"
    Effects can run at the same time, disable or delete them if you only want to run Art-Net In.

### Recommended Art-Net controllers

Next to using MoonLight as an Art-Net receiver, the following devices have been tested and can also be used:

[PKNight ArtNet2-CR021R](https://s.click.aliexpress.com/e/_ExRrKe4)

<img width="200" src="https://github.com/user-attachments/assets/e3d605b6-a023-4abb-b604-77b44267b1a3">

* Driving DMX fixtures: Used to drive the Light Presets for DMX lights / moving heads (see below)
* **Nr of outputs**: max 2 outputs
* **Universes per output**: 1 universe
* **Channels per output**:  512 channels

[Club Lights 12 Pro Artnet Controller - CL12P](https://s.click.aliexpress.com/e/_Ex9uaOk)

<img width="200" src="https://github.com/user-attachments/assets/9c65921c-64e9-4558-b6ef-aed2a163fd88">

* Driving LEDs: : 12 output leds controller, max 8 universes per channel. Max 512 channels per universe. Select IC-Type UCS2903

* **Nr of outputs**: Max 12 outputs
* **Universes per output**: Max 8 universes
* **Channels per output**: max 8 * 512. 

Each color in a LED is one channel: For RGB max 170 LEDs is 510 channels per universe, for RGBW max 128 LEDs per universe is 512 channels per universe => max 1360 RGB LEDs and 1024 RGBW LEDs per output. 

!!! tip "channels per universe"
    Set channels per universe to 510 for RGB and 512 for RGBW (no proof yet it makes a difference ...) on the controller. 

The real number of channels per output can be less then the amount of universes available. e.g. if each output drives one 256 LED RGB panel, channels per output is 768. One package (= one universe) sends 170 LEDs (510 channels) and the second 86 LEDs / 256 channels. The next package for the next panel on the next output will then be in the first universe for that output (so unused universes for a channel will be skipped)
