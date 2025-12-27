# Layers

## Mapping model (🚧)
    
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
          * Driver show: show the result on Leds (using FastLED, hpwit drivers), Art-Net, DDP, ...
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
      * Lights.header and lights.channelsE/D. CRGB leds[] is using lights.channelsE/D (acting like leds[] in FASTLED) ✅
      * A Physical layer has one or more virtual layers and a virtual layer has one or more effects using it. ✅
  * Presets/playlist: change (part of) the nodes model
  
    ✅: Done
    
## Example

```json
{
  "nodes": [
    {
      "name": "Lissajous 🔥🎨🐙",
      "on": true,
      "controls": [
        {
          "name": "xFrequency",
          "type": "slider",
          "default": 64,
          "p": 1065414703,
          "value": 64
        },
        {
          "name": "fadeRate",
          "type": "slider",
          "default": 128,
          "p": 1065414704,
          "value": 128
        },
        {
          "name": "speed",
          "type": "slider",
          "default": 128,
          "p": 1065414705,
          "value": 128
        }
      ]
    },
    {
      "name": "Random 🔥",
      "on": true,
      "controls": [
        {
          "name": "speed",
          "type": "slider",
          "default": 128,
          "p": 1065405731,
          "value": 128
        }
      ]
    }
  ]
}
```