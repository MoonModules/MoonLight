# Effects module

<img width="320" src="https://github.com/user-attachments/assets/571fd676-92f7-4ea7-a521-fc298490695c" />

## Overview

The Effects module defines Effects and Modifiers. They run in a 'layer'. 

* Effect 🔥: An animation running in a layer, see below for am overview of effects. Effects can run in 1D, 2D or 3D coordinate spaces.
* Modifier 💎: An effect on an effect e.g. mirror, multiply or rotate. See [Modifiers](https://moonmodules.org/MoonLight/moonlight/modifiers/)
* Layer: An area on a (LED) display. Effects and modifier run in this area. Each layer maps a coordinate space to the display

!!! info "One layer"

    Currently there is only one layer, projected on the whole display. Multiple layers is planned later

!!! info "3D"

    MoonLight is build from the ground to support 3D. First there where LED strips, later LED matrices and now also 3D shapes like spheres or cubes, making it suitable for art-installations. 1D and 2D objects can be seen as 3D objects where some dimensions have a width of 1. Most effects are 1D or 2D, they will slowly be adapted to support also 3D. Modifiers can be used to project 1D on 2D or 2D on 3D layouts.

## Controls

* Layer: Choose the layer (currently only one layer supported)
    * Start, End and Brightness: read only for now, when multiple layers are implemented, these can be set per layer
* Nodes: a list of Effects and Modifiers for a specific layer
    * Nodes can be added (+), deleted (🗑️) or edited (✎) or reordered (drag and drop). The node to edit will be shown below the list, press save (💾) if you want to preserve the change when the device is restarted or you want to save as a preset (see Light Control)
    * Reorder: Nodes can be reordered, defining the order of execution
        * Effects: which effect on top of the other effect.
        * Modifiers: which modifier is done first, e.g. circle then multiply will yield to a different result than multiply then circle
    * Controls. A node can be switched on and off and has custom controls, which defines the behaviour of the node (e.g. effect speed).
    * See below for a list of existing Effects and Modifiers

## Effect 🔥 Nodes

Below is a list of effects in MoonLight.
Want to add an effect to MoonLight, see [develop](https://moonmodules.org/MoonLight/develop/overview/). See also [Live scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) to add runtime effects.

### MoonLight effects

These effects have first been created in MoonLight:

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Solid | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_000.gif) | <img width="320" src="https://github.com/user-attachments/assets/844bdbf1-b42b-4b5c-94f9-d1334bce2b10" /> | Can be used as background to other effects when ordered before other effects |
| Lines | ![lines](../../media/moonlight/effects/lines.gif) | <img width="320" alt="Speed" src="https://github.com/user-attachments/assets/b99aa9cb-161f-4ade-8d93-bc5343214ac7" /> | To test a layout, Red = x, Green = y, Blue = z |
| Frequency Saws ♫ | ![FreqSaws](https://github.com/user-attachments/assets/9e0f1f22-eba9-470e-850e-bd0e2f7c787f) | <img width="320" src="https://github.com/user-attachments/assets/78828181-c1b3-4d0f-85dc-356ced10fcd9"/> | |
| Moon Man | <img width="100" alt="MoonMan" src="https://github.com/user-attachments/assets/cd3acd08-2725-4d2c-b809-37a61314d620" /> | No controls | only on devices supporting M5 library, e.g. AtomS3R |
| Particles 🧊 | ![Particles](https://github.com/user-attachments/assets/c2df7f6b-3b5a-4836-8e21-127de3eb3364) | <img width="320" alt="Particles" src="https://github.com/user-attachments/assets/415d746a-5b9d-4936-9cf7-343661ac86b5" /> | Supports Gyro 🚧 |
| Rainbow | ![Rainbow](https://github.com/user-attachments/assets/d74832fa-0e77-481c-9823-588042751f87) | <img width="320" alt="Rainbow" src="https://github.com/user-attachments/assets/ca28f78f-dc54-4999-a0ee-700390b24f97" /> | |
| Random | ![Random](https://github.com/user-attachments/assets/b7874741-7a30-4ad1-9a8b-db0093da1297) | <img width="320" alt="Speed" src="https://github.com/user-attachments/assets/b99aa9cb-161f-4ade-8d93-bc5343214ac7" /> | |
| Ripples 🧊 | ![Ripples](https://github.com/user-attachments/assets/ad1d96d9-60fc-4301-8f17-239e45de8d38) | <img width="320" alt="Ripples" src="https://github.com/user-attachments/assets/d9c83988-d60a-4eed-9e04-ea7ddbfc37ab" /> | |
| Rubiks Cube 🧊 | ![Rubik](https://github.com/user-attachments/assets/a9f9b0f7-d9e9-48a2-8b02-e602593e21f6) | <img width="320" alt="Rubik" src="https://github.com/user-attachments/assets/a1b2ee8d-3445-408c-bd53-1721eb6bc249" /> | |
| Scrolling Text | ![ScrollingText](https://github.com/user-attachments/assets/9ebcc5eb-2b4a-4eff-993c-7836fc07dcea) | <img width="320" alt="ScrollingText" src="https://github.com/user-attachments/assets/b32c2b4b-2774-42a9-8174-77602d1bc547" /> | Preset: Auto, IP (.ddd), FPS (ddds), Time (HHMM), Uptime (s/m/h), Status (AP/STA), Clients (dC), Connected (dCC), Active (dAC) |
| Sinus | ![Sinus](https://github.com/user-attachments/assets/ad42f5f1-3670-4238-a9e7-d9d1388760d4) | <img width="320" alt="Speed" src="https://github.com/user-attachments/assets/b99aa9cb-161f-4ade-8d93-bc5343214ac7" /> | |
| Sphere Move 🧊 | ![SphereMove](https://github.com/user-attachments/assets/f02013a4-9aca-4831-9bae-79db8e421008) | <img width="320" alt="Speed" src="https://github.com/user-attachments/assets/b99aa9cb-161f-4ade-8d93-bc5343214ac7" /> | |
| StarField | ![StarField](https://github.com/user-attachments/assets/a5fcc733-0365-4109-bdcf-f353aa2cf3c0) | <img width="320" alt="StarField" src="https://github.com/user-attachments/assets/20afd6a8-df4c-4532-b640-92e84c1c6849" /> | |
| Praxis | ![Praxis](https://github.com/user-attachments/assets/f9271d1c-bcd1-4a79-bc1a-cac951758195) | <img width="320" alt="Praxis" src="https://github.com/user-attachments/assets/536ab4c8-5c90-4b76-9f80-2aaed4170901" />| |
| Wave | ![Wave](https://github.com/user-attachments/assets/a699f3a6-c981-4159-a96e-85d43c9a853c) | <img width="320" alt="Wave" src="https://github.com/user-attachments/assets/2e8408e8-4610-45dd-af36-8560fe5ec024" /> | Type: Saw, Triangle, Sinus, Square, Sin3, Noise |
| Fixed Rectangle | <img width="120" alt="Rectangle" src="https://github.com/user-attachments/assets/474bd313-d961-4a95-8e44-015539a0ba7f" /> | <img width="320" alt="RectangleC" src="https://github.com/user-attachments/assets/e9c1fca4-d7a2-42f4-9d23-643371b3c615" /> | To test a layout |

### MoonModules effects

The following effects are created by the MoonModules team, mostly first in WLED-MM and enhanced in MoonLight:

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| GEQ 3D ♫ | ![GEQ3D](https://github.com/user-attachments/assets/decb902d-1ecf-4be5-a83d-dff0a83cc65f) | <img width="320" alt="GEQ3D" src="https://github.com/user-attachments/assets/5ee5fcdf-86a4-4e88-a59e-b84166be91e2" /> | 2D effect |
| PaintBrush ♫ 🧊 | ![PaintBrush](https://github.com/user-attachments/assets/213629af-eb8b-4c7b-9768-c0f1f2be3ed5) | <img width="320" alt="PaintBrush" src="https://github.com/user-attachments/assets/de1f9379-9984-4d9e-a95a-954df1ba69f4" /> | |
| Game Of Life 🧊 | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_172.gif) | <img width="320" src="https://github.com/user-attachments/assets/18498c30-b249-4390-bfdf-084deedbfc49" /> | |


### WLED effects

The following effects originated from WLED, most have been enhanced in WLED-MM and further enhanced in MoonLight:

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Blackhole | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_183.gif) | | |
| Bouncing Balls | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_091.gif) | <img width="320" src="https://github.com/user-attachments/assets/30aa2614-d891-4d39-a789-72026f7ab036" /> | |
| Blurz ♫ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_MM163.gif) | <img width="320" alt="Blurz" src="https://github.com/user-attachments/assets/bf50662f-6be1-44b2-9970-89a5cd003450" /> | |
| Distortion Waves | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_124.gif) | <img width="320" src="https://github.com/user-attachments/assets/b8f4111d-1f78-4ac0-a0f9-17909ad78599" /> | |
| Frequency Matrix ♪ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_138.gif) | <img width="320" alt="FreqMatrix" src="https://github.com/user-attachments/assets/bb6e4575-2c9e-4aa4-bc7c-12b638d98984" /> | Not really a matrix ... |
| GEQ ♫ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_MM139.gif) | <img width="320" src="https://github.com/user-attachments/assets/ca485d1e-e6e4-4b4a-910f-eed7fde7a21f" /> | |
| Lissajous | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_MM176.gif) | <img width="320" src="https://github.com/user-attachments/assets/a28c19a7-012a-49bf-aff1-6b414c9d08dd" /> | |
| Noise 2D | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_146.gif) | <img width="320" src="https://github.com/user-attachments/assets/b8f4111d-1f78-4ac0-a0f9-17909ad78599" /> | |
| Noise Meter ♪ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_136.gif) | <img width="320" alt="Noise meter" src="https://github.com/user-attachments/assets/918b6d52-1e13-4846-8ded-19ba4a722383" /> | |
| PopCorn ♪ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_095.gif) | <img width="320" alt="Popcorn" src="https://github.com/user-attachments/assets/a98636f4-d1cd-4ab7-a261-5df541aaa430" /> | |
| Waverly ♪ | ![](https://raw.githubusercontent.com/scottrbailey/WLED-Utils/master/gifs/FX_MM165.gif) | <img width="320" alt="Waverly" src="https://github.com/user-attachments/assets/feb2f380-c173-456b-9abf-eebb9f765ab0" /> | |

### Moving head effects

<img width="320" src="https://github.com/user-attachments/assets/6e61c41f-e128-4adc-b9c1-6239fe4736dc"/>

The following effects are created in MoonLight, supporting driving Moving Heads over [Art-Net](https://moonmodules.org/MoonLight/moonlight/drivers/#art-net):

Select Moving heads in light presets, see [Light presets](https://moonmodules.org/MoonLight/moonlight/drivers/#max-power-and-light-preset)

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Troy1 Color ♫ | ![]() | <img width="320" alt="T1Color" src="https://github.com/user-attachments/assets/ebf89b1c-c70e-4385-8d23-14caa488d747" /> | |
| Troy1 Move ♫ | ![]() | <img width="320" alt="T1Move" src="https://github.com/user-attachments/assets/c2e7d28a-3974-4ddd-976c-d83965fe62a8" /> | |
| Troy2 Color ♫ | ![]() | <img width="320" alt="T2Color" src="https://github.com/user-attachments/assets/0523ef3f-bee2-4345-98e1-56256867d24e" /> | |
| Troy2 Move ♫ | ![]() | <img width="320" alt="T2Move" src="https://github.com/user-attachments/assets/bebfcf19-bc17-4c24-9ab0-86b47e4dcaa0" /> | |
| FreqColors ♫ | ![]() | <img width="320" alt="FColors" src="https://github.com/user-attachments/assets/475ef59a-31af-4455-894c-fd0bb76ff431" /> | |
| Wowi Move ♫ | ![]() | <img width="320" alt="WMove" src="https://github.com/user-attachments/assets/45f17d92-391e-4e9f-9e90-d931563198aa" /> | |
| Ambient Move ♫ | ![]() | <img width="320" alt="AmbientMove" src="https://github.com/user-attachments/assets/0a67a64f-6656-47f6-81c1-c6d737e4eb99" /> | |
