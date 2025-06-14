# Editor module

<img width="396" alt="image" src="https://github.com/user-attachments/assets/965dd352-d7af-42a1-a72d-43da3b32a252" />

## Functional

* Nodes: One or more processes, 
    * Can be light layouts, effects or modifiers (in fact one node can also be a combination of these)
    * On/off button defines if a node is active or not
    * A node can be a precompiled Node or a livescript (loaded in the file system)
    * Nodes define their own controls which are dynamically shown in the UI (changes when the node changes). Also livescripts can define their own controls using addControl(s)
* If a live script file is updated (here or in the [File Manager](https://moonmodules.org/MoonLight/moonbase/files/)) and the file is part of an active node, it will recompile and reload controls

<img width="498" alt="Screenshot 2025-03-29 at 14 12 01" src="https://github.com/user-attachments/assets/3a5a3743-c0a4-4456-96cb-f4abd0d01450" />

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
