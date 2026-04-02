# Layouts

A layout (🚥) tells MoonLight **where your lights physically are** and **how they are wired**. Get this right and every effect — from a simple rainbow to a 3D plasma — automatically fits your fixture perfectly.

## How layouts work

### The coordinate grid

When you define a layout, each light gets a position in a 3D coordinate space (X, Y, Z). MoonLight takes all those positions and figures out the bounding box — the smallest grid that contains every light. Effects are then computed across that whole grid and automatically mapped to your physical lights.

This means:

- **Effects are fixture-aware.** A plasma effect on a 16×16 panel looks like a full-screen plasma. The same effect on a 3D cube wraps correctly around all three axes. You don't write a different effect for each shape.
- **Wiring order doesn't matter to effects.** Your LED strip might snake left-to-right on odd rows and right-to-left on even rows (serpentine wiring). The layout describes that wiring pattern; effects just see a clean grid.
- **Resolution scales automatically.** A 10×10 panel and a 32×32 panel both run the same effects — the effect adapts to however many lights you have.

### 1D, 2D and 3D fixtures

| Fixture type | Typical examples | Grid shape |
|---|---|---|
| **1D** — a line | LED strip, LED bar, single tube | Y axis only — X and Z are 0. The Y axis runs vertically so 1D effects like bouncing balls, drip and rain move in the natural downward direction |
| **2D** — a flat surface | LED matrix, panel, ring, wheel | X and Y axes — Z is 0 |
| **3D** — a volume | Cube, Christmas tree, spiral tower, Human Sized Cube | X, Y and Z axes all used |

MoonLight automatically detects the dimensionality from the coordinates you define, so 1D effects run on strips and 3D effects light up cubes without any extra configuration.

### How effects fill the grid

The effect runs across every point in the bounding grid, but your fixture only has lights at specific positions inside it. MoonLight builds a **mapping** that links each grid point to the nearest physical light — so even a 500-LED Christmas tree mounted inside a 25×25×100 bounding box gets a smooth, full-volume effect. Sparse fixtures (few lights in a large space) work fine as long as the pixel density is reasonable. Very sparse grids — for example, a handful of moving heads spread across a large stage — are a future use case.

### Multiple layout nodes

You can add more than one layout node — for example three separate panels or a ring combined with a bar. MoonLight merges all their positions into a single shared grid and maps effects across all of them together. Lights are assigned to the grid in the order the layouts appear in the node list.

### GPIO pin assignment

Layouts also assign groups of LEDs to the ESP32 GPIO pins that drive them. Each layout node controls which pin(s) its lights are connected to. Complex fixtures like a cube or a multi-panel wall often use one pin per face or one pin per panel so each segment can be driven in parallel, improving performance.

!!! tip "Start simple"
    Not sure which layout to use? Start with **Single Row** for a strip, **Panel** for a matrix, or **Rings** for circular fixtures. These cover the most common setups and are easy to combine into larger builds.

!!! tip "Custom shapes"
    Any shape that isn't covered by the built-in layouts can be created as a **Live Script** — a small `.sc` file with an `onLayout()` function that places lights exactly where you need them. See [Live Scripts](livescripts.md).

!!! note "Future: room-scale mapping"
    It is possible in principle to treat an entire room or stage as the coordinate space and map all fixtures — panels, moving heads, tubes — into that shared grid. This would let effects flow seamlessly from one fixture to another across physical space. MoonLight's architecture supports it, but algorithms optimised for very sparse, large-scale grids are not yet implemented.

## Layout 🚥 Nodes

Below is a list of Layouts in MoonLight. 
Want to add a Layout to MoonLight, see [develop](https://moonmodules.org/MoonLight/develop/overview/). You can also create custom layouts as [Live Scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) using `onLayout()`, `addLight()`, and `nextPin()`.

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Panel | ![Panel](https://github.com/user-attachments/assets/1a69758a-81e3-4f1f-a47e-a242de105c93)| <img width="320" alt="Panel" src="https://github.com/user-attachments/assets/60e6ba73-8956-45bc-9706-581faa17ba16" /> | Defines a 2D panel with width and height<br>Wiring Order (orientation): horizontal (x), vertical (y), depth (z)<br>X++: starts at Top or bottom, Y++: starts left or right<br>snake aka serpentine layout|
| Panels | ![Panels](https://github.com/user-attachments/assets/422b5842-773b-4173-99c5-7b25cd39b176) | <img width="320" alt="Panels" src="https://github.com/user-attachments/assets/ad5a15ea-f3f9-42b9-b8cf-196e7db92249" /> | Panel layout + Wiring order, directions and snake also for each panel |
| Cube | ![Cube](https://github.com/user-attachments/assets/3ece6f28-519e-4ebf-b174-ea75c30e9fbe) | <img width="320" alt="Cube" src="https://github.com/user-attachments/assets/56393baa-3cc3-4c15-b0b2-dc72f25d36d1" /> | Panel layout + depth<br> Z++ starts front or back<br>multidimensional snaking, good luck 😜 |
| Rings | ![Ring](https://github.com/user-attachments/assets/7f60871d-30aa-4ad4-8966-cdc9c035c034) | <img width="320" alt="Rings" src="https://github.com/user-attachments/assets/ee2165aa-cf01-48cd-9310-9cfde871ac33" /> | 241 LEDs in 9 rings |
| Wheel | ![Wheel](https://github.com/user-attachments/assets/52a63203-f955-4345-a97b-edb0b8691fe1) | <img width="320" alt="Wheel" src="https://github.com/user-attachments/assets/7b83e30b-e2e1-49e6-ad80-5b6925b23018" /> | |
| Human Sized Cube | ![HCS](https://github.com/user-attachments/assets/8e475f9d-ab7a-4b5c-835a-e0b4ddc28f0f) | <img width="320" alt="HCS" src="https://github.com/user-attachments/assets/de1eb424-6838-4af4-8652-89a54929bb03" /> | |
| Toronto Bar Gourds | ![gourds](../media/moonlight/layouts/BarGuords.gif) | <img width="320" alt="gourds" src="../media/moonlight/layouts/BarGuords.png"/> | |
| Car Lights | ![carlights](../media/moonlight/layouts/CarLights.gif) | <img width="320" alt="gourds" src="../media/moonlight/layouts/CarLights.png"/> | |
| Single Column | ![Single column](https://github.com/user-attachments/assets/a88cea0f-9227-4da4-9a43-b944fd8bef97) | <img width="320" alt="Single column" src="https://github.com/user-attachments/assets/9f9918b9-e1ee-43a8-a02d-7f1ee182888b" /> | Choose Single Column for LED strips |
| Single Row | ![Single row](https://github.com/user-attachments/assets/4ba5a3ac-9312-4bac-876d-cfa3dce41215) | <img width="320" alt="Single row" src="https://github.com/user-attachments/assets/70455279-646c-467d-b8e5-492b1aeae0fa" /> | |
| SE16 | ![SE16](https://github.com/user-attachments/assets/45c7bec7-2386-4c42-8f24-5a57b87f0df9) | <img width="320" alt="SE16" src="https://github.com/user-attachments/assets/0efe941a-acf5-4a2c-a7d6-bdfa91574d1a" /> | Layout(s) including pins for Stephan Electronics 16-Pin ESP32-S3 board<br>see below |
| LightCrafter16 | ![LightCrafter16](https://github.com/user-attachments/assets/45c7bec7-2386-4c42-8f24-5a57b87f0df9) | <img width="320" alt="LightCrafter16" src="https://github.com/user-attachments/assets/0efe941a-acf5-4a2c-a7d6-bdfa91574d1a" /> | Layout(s) for Stephan Electronics LightCrafter16 ESP32-S3 board<br>see below |

!!! warning "Choosing pins"

    Choose the right pins with care. See also the IO module to see which pins can in general be used for LEDs (💡). But depending on a specific boards some pins might also be in use already. 

!!! tip "Multiple layouts"
    Single line, single row or panel are suitable layouts to combine into a larger fixture.

### SE16

16-channel LED strip driver by Stephan Electronics

![SE-16p](../firmware/installer/images/esp32-s3-stephanelec-16p.jpg)

* Leds Per Pin: the number of LEDs connected to each pin
* Pins Are Columns: are the LEDs on a pin a row of the effect (width is 1 (or 2) x ledsPerPin). If not set the LEDs are a column (height is 1 (or 2) x ledsPerPin)
* Mirrored Pins: If set it is assumed that LEDs are connected with increasing positions on 8 pins on one side of the board and decreasing positions on the 8 pins of the other side of the board. The resulting size will have a width of 8 and the height (or width) will be 2 * ledsPerPin. If not set, the width will be 16 and the height (or width) = ledsPerPin

### LightCrafter16

16-channel LED strip driver by Stephan Electronics

![SE-16p](../firmware/installer/images/esp32-s3-lightcrafter16.jpg)

* Leds Per Pin: the number of LEDs connected to each pin
* Pins Are Columns: are the LEDs on a pin a row of the effect (width is 1 (or 2) x ledsPerPin). If not set the LEDs are a column (height is 1 (or 2) x ledsPerPin)

X0Y0 position is on the top left when the board is positioned in such a way that the Ethernet connector is on the top left.