# Editor module

<img width="396" alt="image" src="https://github.com/user-attachments/assets/965dd352-d7af-42a1-a72d-43da3b32a252" />

## Functional

The editor allows you to define the tasks to run an **effect**  (e.g. bouncing balls), to **modify** the effect (e.g mirror), to send it to a **layout** (e.g. a panel 16x16) and to run **supporting processes** (e.g. audiosync to run sound reactive effects).
Each task is defined as a node. A node can be precompiled in the firmware or defined by a live script loaded onto the File System (See File Manager). A node can be switched on and off and has custom controls, which defines the parameters of the node (e.g. effect speed).

Ultimately the nodes will be displayed in a graphical interface where nodes are connected by 'noodles' to define dependencies between nodes. For the time being nodes will be shown in a list.

Typically a node will define a layout (🚥), or an effect (🔥), or a modifier (💎) or a supporting process (☸️) but can also combine these tasks (experimental at the moment). To avoid duplication it's in most cases recommended to keep them seperated so an effect can run on multiple layouts and a modifier can modify any effect. 

* **Layout** 🚥: a layout defines what lights are connected to MoonLight. Currently only one layout node is supported defining all the lights (But might change in the future). It defines the coordinates of all lights (addLight) and assigns lights to the GPIO pins of the ESP32 (addPin) and how many channels each light has (normal LEDs 3: Red, Green and Blue). 
    * The **coordinates** of each light are defined in a 3D coordinate space where each coordinate range between 1 and 255. Currently a strip until 255 leds is supported, a panel until 128x96 LEDS and a cube max 20x20x20. 
        * Coordinates needs to be specified in the order the lights are wired so MoonLight knows which light is first, which is second etc.
        * If a 1D strip is longer, you can address more leds by pretending it is a 2D fixture, e.g. 32x32 to address a strip of 1024 LEDs. 
        * In the future we might look at redefining Coord3D where width is 12 bytes, height is 7 bytes and depth is 5 bytes allowing for max 4096 x 128 x 32 ...
    * Currently **pins** are by default driven by FastLED.show. FastLED needs all LEDs specifications be defined at compile time, e.g. LED type, color order, etc. Normally also the pin(s) need to be defined beforehand but all pins are predefined in the code to avoid this (at a cost of larger firmware). As we want to be able to define all LED specs in the UI instead of during compile, we need to see how / if this can be done.
    * In the future, instead of pins, also **IP addresses and universes** can be specified to support sending lights data to ArtNet devices - e.g. [Pknight Artnet DMX 512](https://s.click.aliexpress.com/e/_ExQK8Dc) to control DMX lights or [ArtNet LED controller](https://s.click.aliexpress.com/e/_Ex9uaOk) to control LED strips or panels over local network. Currently this is handled by [Module ArtNet](https://moonmodules.org/MoonLight/moonbase/module/artnet/).
    * Alternatively Moonlight supports 2 other drivers: [Physical Driver](https://github.com/hpwit/I2SClocklessLedDriver) and [Virtual Driver](https://github.com/ewowi/I2SClocklessVirtualLedDriver). Firmware ending with PD or VD will have this enabled. Code has been included but not tested yet (tested in StarLight), this is planned for June/July. 
        * The physical driver is an alternative to the FastLED driver and does not need LED specifications defined at compile time, all can be controlled via UI. 
        * The virtual driver is another beast and with the help of shift registers allows for driving 48 panels of 256 LEDs each at 100 FPS!.
    * MoonLight will use the layout definition to generate a **mapping** of a virtual coordinate space to a physical coordinate space. Most simple example is a panel which has a snake layout. The mapping will create a virtual layer where the snake layout is hidden.

* **Effect** 🔥: An effect runs in a virtual layer (see above about mapping to a physical layer). Historically there are 1D, 2D and 3D effects. A 1D effect only fills leds in x space, leaving y and z blank. 2D also the y space, 3D all spaces. Future goal is that all effects fill all spaces (example is bouncing balls which was a 1D effect but has been made 2D). See also Modifiers which can take a 1D effect and make a 2D or 3D effect out of it: e.g. a 1D effect can be presented as a circle, or sphere.
    * An effect has a loop which is ran for each frame produced. In each loop, the pixels in the virtual layer gets it's values using the setLight function. It can take a CRGB value as parameter (default) but can also deal with multichannel lights like Moving Heads.
    * Multiple effects can be defined, they all run during one frame. In the future each effect will have a start and end coordinate so they can also be displayed on parts of the fixture.

* **Modifier** 💎: a modifier is an effect on an effect. It can change the size of the virtual layer, it can map the pixels other then 1:1 (e.g. mirror, multiply them or a 1D line can become a 2D circle) and it can change each light set in an effect during a loop (e.g. rotate the effect)
    * Multiple modifiers are allowed, e.g. to first rotate then mirror (or first mirror then rotate). The UI allows for reordering nodes.

## Nodes

Emoji coding:
🔥 Effect
🚥 Layout
💎 Modifier
☸️ Supporting node
🎨 Using palette
💡 WLED origin
💫 MoonLight origin
♫ Audio reactive FFT based
♪ Audio reactive volume based
🧊 3D

### Effect 🔥 Nodes

### Layout 🚥 Nodes

### Modifier 💎 Nodes

## Archive

This page is 🚧, text below will be rewritten.

## Q&A

Collecting questions and answers (from [Discord](https://discord.com/channels/700041398778331156/1203994211301728296))
WIP !!

* I'm therefore wondering if esplive only supports xtensa ASM and not RISCV? we're actually not supposed to enable esplivescript on anything else than the s3. For the moment it’s esp32 , esp32 s2 esp32s3. I am revamping the compiler for it to be more compact and less memory hungry. Once that is done I will work on creating risc assembly language so it can be used with the esp32 C family and also I want to do arm for the pi pico.
* Technically live scripts works on normal esp32, but MoonLight with live scripts is 103% flash size. I didn’t look into other partitioning (preferably keeping Ota working): note use esp32 16MB!!
* why do some animations allow me to specifiy the led strip pin but not others?
* is there a wiki page I can read for me to understand pin mapping and led strip physical format?  there are layout nodes 🚥 (defining how the lights layed out in a matrix, cube etc. There you can define pins) effect nodes 🔥 (obviously) and modifier nodes 💎(modify an effect)
* how does the lights control module interact with the animations module?
* what does "snake" mean for a moving head configuration?
* how do i specify which solid color i want? Added!
* getting a few "Software reset due to exception/panic" depending on the effects i set 😄 but that might be my PSU
* how I can specify the color order.... I see the define I want in fastled.h, i'm guessing it's passed somehow to the template through maybe the ML_CHIPSET define? oh.... it seems to require a modification of void LedsDriver::init
sorry for the spam.... it also looks to me that even with the ML_CHIPSET=SK6812 define there's no support for RGBW as on a small led strip, setting a solid color of red for example, i see green & white - blue - red - green & white - blue - red (eg: it's forgetting to send 4x 8b per led)
* is the monitor only available with the s3? I enabled "Monitor On" in the control tab oh that's interesting, the platform booted full white (when configured in solid animation) but went back to what was supposed to be when I started moving the control sliders. should be working on all platforms
* not sure if this has been flagged, but when changing a selected node type, the parameters of the previous node will stay displayed and when switching node types, I did manage to get LEDs frozen (impossible to get them ot update again)
it takes a platform reboot, and changing the movinghead number of LEDs in my case. it looks like some refresh isn't happening. even the platform reboot trick sometimes isn't happening... looking at the console output i'm seeing that my actions on the user interface aren't registered
* how complex would it be to map several led strips on several IOs to a virtual 2d matrix? I'm currently looking at the code, more particularly void mapLayout() and it seems the layout is reset every time that function is called, so it is not possible to increase the display size
* [single line and row layouts](https://github.com/MoonModules/MoonLight/pull/19)
* personally i'd make 2 different menus in the esp32 GUI.. a layout is more something you set and forget about as it is tied to a physical install
* one layout to define all the lights is definitely a blocker for me (and I imagine more people) as physical installs have their own constraints. let me know what you think. I will add that possibility , I see ‘some’ usage, but I don’t understand why it is a blocker as in general the whole setup is a number of lights which you know in advance so then it is most clear to define that in one go ?


## Technical

* Nodes
    * See Nodes.h: class Node
        * name(): name of the node
        * hasFunctions: enables features of the Node (Layout, Modifier)
        * on: (de)activates the node
        * constructor: sets the corresponding layer
        * setup: if layout sets channelsPerLight and request map
    * Nodes manipulate the leds / channels array and the virtual to physical layer mappings.
    * specify which functions (layout, effect, modifier): One node in general implements one, but can also implement all three (e.g. Moving Head...  wip...)
        * layout
        * effect
        * modifier
    * Live scripts
        * See Nodes.h / nodes.cpp
    * Lights
        * Regular patterns (CRGB as default but also others like Moving Head ...)

* See [Modules](../modules.md)
* Upon changing a pin, driver.init will rerun (FastLED.addLeds, PD and VD driver.init)
* Uses ESPLiveScripts, see compileAndRun. compileAndRun is started when in Nodes a file.sc is choosen
    * To do: kill running scripts, e.g. when changing effects
* [Nodes.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes.cpp): class Node (constructor, destructor, setup, loop, hasFunctions, map, modify, addControl(s), updateControl)
* [Nodes.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes.cpp): implement LiveScriptNode

### Mapping model (WIP)

<img width="500" src="https://github.com/user-attachments/assets/6f76a2d6-fce1-4c72-9ade-ee5fbd056c88" />

* Multiple Nodes can be created (1)
    * Each node can have controls (compare controls in WLED / StarLight) ✅
    * Each node can run precompile code or Live scripts (with or without loop) ✅
    * Each node has a type:
        * Layout: tell where each light is in a 1D/2D/3D physical coordinate space (based on StarLight fixtures) ✅
        * Effect: 
            * run an effect in a virtual coordinate space ✅
            * in the physical space if you want to run at highest performance, e.g. a random effect doesn't need to go through mappings ✅
        * Modifier: Mirror, rotate, etc, multiple modfiers allowed (projection in StarLight) 🚧
            * A modifier can also map lights dimensions to effect dimensions: change the lights to a 1D/2D/3D virtual coordinate space
                * e.g. if the light is a globe, you can map that to 2D using mercator projection mapping
                * if the light is 200x200 you can map it to 50x50
                * if the light is 2D, a 1D effect can be shown as a circle or a bar (as WLED expand1D)
        * Driver show: show the result on Leds (using FastLED, hpwit drivers), ArtNet, DDP, ...
* Future situation: Nodes and noodles (2)
    * Replace the nodes table (1) by a graphical view (2)
* Virtual Layer (MappingTable) (3)
    * Array of arrays. Outer array is virtual lights, inner array is physical lights. ✅
    * Implemented efficiently using the StarLight PhysMap struct ✅
    * e.g. [[],[0],[1,2],[3,4,5],[6,7,8,9]] ✅
        * first virtual light is not mapped to a physical light
        * second virtual light is mapped to physical light 0
        * third virtual light is mapped to physical lights 1 and 2
        * and so on
    * Virtual lights can be 1D, 2D or 3D. Physical lights also, in any combination
        * Using x + y * sizeX + z * sizeX * sizeY 🚧
    * set/getLightColor functions used in effects using the MappingTable ✅
    * Nodes manipulate the MappingTable and/or interfere in the effects loop 🚧
    * A Virtual Layer mapping gets updated if a layout, mapping or dimensions change 🚧
    * An effect uses a virtual layer. One Virtual layer can have multiple effects. ✅
* Physical layer
    * CRGB leds[MAX_LEDS] are physical lights (as in FASTLED) ✅
    * A Physical layer has one or more virtual layers and a virtual layer has one or more effects using it. ✅
* Presets/playlist: change (part of) the nodes model

✅: Done

### Server

[ModuleEditor.h](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/ModuleEditor.h)

### UI

Generated by [Module.svelte](https://github.com/MoonModules/MoonLight/blob/main/interface/src/routes/moonbase/module/Module.svelte)
