# Nodes

## Overview

A node can be an Effect, a Modifier, a Layer or a Driver.

* See the [MoonLight functional video](https://www.youtube.com/watch?v=tdrU9yGkyVo) for an introduction to nodes.
* See the [Developer quickstart video](https://studio.youtube.com/video/bJIgiBBx3lg) how to add of nodes.
* MoonLight community approach encourages nodes to be added by 'everybody'.
* MoonLight only supports one firmware per type of board. This means that all nodes contributed by the community will be available in that firmware and it is expected to be maintained by the author of a node specifically and the community in general.

Nodes are inspired by WLED usermods, further developed in StarBase and now in MoonLight (using the ESP32-Sveltekit infrastructure)

A node implements the following (overloaded) functions:

* setup() and loop()
* controls: each node has a variable number of flexible variables of different types (sliders/range, checkboxes, numbers etc). They are added with the addControl() function in the setup()

* Node types: it is recommended that a node is one of the 4 types (Effect, Modifier, Layer, Driver). However each node could perform functionality of all types. To recognize what a node does the emojis 🚥, 🔥, 💎 and ☸️ are used in the name. The function hasOnLayout() and hasModifier() indicate the specific functionality the node supports. They control when a physical to virtual mapping is recalculated
    * **hasOnLayout()**: a layout node specify the positions of lights controlled. E.g. a panel of 16x16 or a cube of 20x20x20. If hasOnLayout() is true you should implement onLayout calling addLight(position) for all the lights and nextPin()
        * nextPin() is needed to define how many ledsPerPin are used for each pin
        * onLayout and hasOnLayout is also used by driver nodes and is used to init or update the driver based on the layouts, a driver will use the layout nodes which are defined before the driver node, so order matters (Layouts can be reordered)
    * **hasModifier()**: a modifier node which manipulates virtual size and positions and lights using one or more of the functions modifySize, modifyPosition and modifyXYZ.
    * if the loop() function contains setXXX functions (e.g. setRGB()) , it is used it is an **effect** node. It will contain for-loops iterating over each virtual ! light defined by layout and modifier nodes. The iteration will be on the x-axis for 1D effects, but also on the y- and z-axis for 2D and 3D effects. setRGB is the default function setting the RGB values of the light. If a light has more 'channels' (e.g. Moving heads) they also can be set. 

## Moving heads

* **addLight** will show where the moving head will be on the stage. In general only an array of a few lights e.g. 4 moving heads in a row. A moving head effect will then iterate over 4 lights where each light might do something different (e.g. implement a wave of moving head movement)
* You need to define **channelsPerLight** in the layout node setup() - (it is default 3 to support normal LEDs). Currently MoonLight only supports identical moving heads with the same channels. The first light starts at DMX 0 (+1), the second at DMX channelsPerLight (+1) the third on DMX 2*channelsPerLight (+1) and so on. (+1): DMX typically starts at address 1 while MoonLight internal starts with 0... 🚧. We are working on a solution to support different lights e.g a mix of 15 channel lights and 32 channel lights etc. You could set channelsPerLight to a higher number as the real lights channels, e.g. 32 so each lights DMX address starts at a multiple of 32.
* **Layout**: The layout node also defines which functionality / channels the light support by defining **offsets**. Currently the following offsets are supported: offsetRGBW, offsetRed, offsetGreen, offsetBlue, offsetWhite, offsetBrightness, offsetPan, offsetTilt, offsetZoom, offsetRotate, offsetGobo, offsetRGBW1, offsetRGBW2, offsetRGBW3, offsetBrightness2 and need to be set in the setup() function.
* The distinction between physical and virtual layer for moving heads is not useful if you have only 2-4 moving heads. However this is a standard MoonLight feature. It might become useful if you have like 8 (identical) moving heads, 4 left and 4 right of the stage, then you can add a mirror modifier and the virtual layer will only control 4 lights, which then will be mapped to 8 physical lights. In theory you can also have a cube of like 512 moving heads and then exotic modifiers like pinwheel could be used to really go crazy. Let us know when you have one of these setups 🚨
* Moving heads will be controlled using the [ArtNed Node](https://moonmodules.org/MoonLight/moonlight/nodes/#art-net/). addPin is not needed for moving heads, although you might want to attach LEDs for a visual view of what is send to Art-Net.
* Effect nodes **set light**: Currently setRGB, setWhite, setBrightness, setPan, setTilt, setZoom, setRotate, setGobo, setRGB1, setRGB2, setRGB3, setBrightness2 is supported. In the background MoonLight calculates which channel need to be filled with values using the offsets (using the setLight function).
* If offsetBrightness is defined, the RGB values will not be corrected for brightness in [ArtNed](https://moonmodules.org/MoonLight/moonlight/nodes/#art-net/).

## Technical

* Nodes
    * See Nodes.h: class Node
        * name(): name of the node
        * hasFunctions: enables features of the Node (Layout, Modifier)
        * on: (de)activates the node
        * constructor: sets the corresponding layer
        * setup: if layout sets channelsPerLight and request map
    * Nodes manipulate the LEDs / channels array and the virtual to physical layer mappings.
    * specify which functions (layout, effect, modifier): One node in general implements one, but can also implement all three (e.g. Moving Head...  wip...)
        * layout
        * effect
        * modifier
    * Live scripts
        * See Nodes.h / nodes.cpp
    * Lights
        * Regular patterns (CRGB as default but also others like Moving Head ...)

* See [Modules](../modules)
* Upon changing a pin, driver.init will rerun (FastLED.addLeds, Parallel LED Driver.init)
* Uses ESPLiveScripts, see compileAndRun. compileAndRun is started when in Nodes a file.sc is chosen
    * To do: kill running scripts, e.g. when changing effects
* [Nodes.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes.cpp): class Node (constructor, destructor, setup, loop, hasFunctions, map, modify, addControl(s), onUpdate)
* [Nodes.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes.cpp): implement LiveScriptNode

    * An effect has a loop which is ran for each frame produced. In each loop, the lights in the virtual layer gets it's values using the setRGB function. For multichannel lights also functions as setWhite or (for Moving Heads) setPan, setTilt, setZoom etc. Also getRGB etc functions exists.

## Drivers

### Initless drivers

Initless means there is no addLeds (like in FastLed) or initLed (like in Parallel LED Driver): 

* a Context (see below) will be set
* Driver.show (see below) will use this context to set the right data to the right output.
* The context can dynamically change without needing to restart or recompile! e.g. changes in pin allocations, leds per pin, RGB or RGBW, or DMX lights like moving heads.

The Art-Net driver is currently working like this, to be added to Parallel LED Driver and parallel IO (P4).

The advantages of dynamic context change are:

* No need to recompile any changed configs (e.g. colorOrder is fully flexible, not a setting in platformio.ini)
* No need to restart while setting up a specific installation. Just change layouts until it works as intended.
* Allows for a flexible mix of different outputs e.g. send the first 1024 leds to Parallel LED Driver, next to SPI, next to one Art-Net device, next to another Art-Net device. 

#### Context

Make sure the following is available before calling driver.show. Note: setup only sets variables the loop will use.

* channels[]: pka leds array (CRGB leds) but we now support any light: not only 3 or 4 channels
* channelsPerLed: a light in the channels array can be found by index * channelsPerLed.
* offsets within the channels (RGBW and more like pan and tilt)
* outputs[]: pins[] for leds drivers, outputs[] for Art-Net. Use generic name outputs[]
* lengths[]: nr of lights per output
* driver[]: shows for each output for which driver it is ... 🚧 as currenlty all drivers process all lights which sometimes is what you want (monitor on leds what is on moving heads) but not always

This is the current list of supported lights ranging from 3 channels per light (the classic rgb LEDs) to 32 channels per light. Currently pre defined: lights preset. In the future configurable

* RGB
* RBG
* GRB: default WS2812
* GBR
* BRG
* BGR
* RGBW: e.g. 4 channel par/dmx light
* GRBW: rgbw LED eg. SK6812
* GRB6: some LED curtains
* RGBWYP: 6 channel par/dmx light with UV etc
* MHBeeEyes150W-15 🐺: 15 channels moving head, see https://moonmodules.org/MoonLight/moonbase/module/drivers/#art-net
* MHBeTopper19x15W-32 🐺: 32 channels moving head
* MH19x15W-24: 24 channels moving heads

Based on the chosen value, the channels per light and the offsets will be set e.g. for GRB: header->channelsPerLight = 3; header->offsetRed = 1; header->offsetGreen = 0; header->offsetBlue = 2;. Drivers should not make this mapping, the code calling drivers should do.

The RGB and W offsets needs to be re-ordered and brightness corrected from the channel array. Not only Art-Net but  also a LEDs driver need to accept channel arrays with more then 4 channels per light. Eg GRB6 is a type of led curtain some of us have, which a Chinese manufacturer screwed up: 6 channels per light but only rgb is used, 3 channels do nothing

#### Driver.show

Called by loop function.

* loop over all outputs (or pins) which are meant for a specific driver (e.g. all outputs for artnet)
    * pre channel actions (...)
    * loop over all lights in the output
        * for each light
            * pre light actions (e.g. setup dma ...)
            * copy all channels to the buffer (for art-net) or to the memory area / dma / parallelIO ... so the driver can process it
                * find them in the channels array
            * correct RGBW offsets when present for brightness using LUT (__red_map etc in Yves his drivers)
            * send the buffer/memory (artnetudp.writeTo sends one universe, Yves drivers ..., Troy drivers ...)
            * post light actions (e.g. increase universe etc)
    * post channel actions (...)

For an example of the loop, see [Artnet loop](https://github.com/MoonModules/MoonLight/blob/fc6c4b6c52bf0a9e9a5de677c1ec5b6536fb7a16/src/MoonLight/Mods.cpp#L260-L293). Not splitten in output loop and lights loop yet (as now all drivers process all lights). Before and after the loop are pre /post output / light actions.

#### Notes on the I2S clockless driver

* nb_components -> channelsPerLed
* p_r, p_g, p_b, p_w -> offsetRed, Green, Blue, White
* stripSize[] -> lenghts[]

#### initLed

* setBrightness / setGamma -> done by MoonLight if brightness (or gamma) changes, driver.show expects the LUT to be set right
* setPins(Pinsq): called by MoonLight as soon as pins changes: add function driver.updatePins which also cleans up previous defined pins (I think there is no cleanup, so can be called repeatedly?)
* i2sInit(): not using context...: so this (and i2sReset...) can be called once in the setup...
    * i2sReset(): not using context...
    * i2sReset_DMA(): not using context...
    * i2sReset_FIFO(): not using context...
* initDMABuffers(): uses __NB_DMA_BUFFER, nb_components -> should be called by MoonLight as soon as dma buffer (would be very nice if this is a variable instead of a constant, so can be tweaked in MoonLight without recompiling) or lights preset (channelsPerLed) changes. Add function driver.updateLightsPreset(dma_buffer, channelsPerLight)...
    * putdefaultones: uses nb_components

#### showPixels

* loadAndTranspose(): uses this for all dma buffers. All Okay
    * loop over num_strips (outputs[].length)
    * loop over stripSize (lenghts[]), light by light: triggered by _I2SClocklessLedDriverinterruptHandler
        * fills secondPixel using LUT and RGB(W) offsets
        * transpose16x1_noinline2(secondpixel)
* i2sStart: uses dma buffers: Okay
